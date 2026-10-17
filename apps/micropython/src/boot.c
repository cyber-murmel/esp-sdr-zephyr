/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Boot glue for MicroPython as the main program. The shared USB device (CDC-ACM console the
 * REPL talks to, DFU, watchdog) starts from here. The port's real_main() runs in its own
 * thread on CPU 0 with its stack in the SRAM above the ring banks: internal RAM, which flash
 * access needs (doc/hardware-quirks.md, Memory), but outside the DRAM image, which must end
 * below the ring banks. esdr_port_init() points MicroPython's stack check at that stack.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr.h>

#include "app_cpu.h"
#include "app_usb.h"

LOG_MODULE_REGISTER(boot, LOG_LEVEL_INF);

/* The port's (main.c, src/zephyr_getchar.c). */
int real_main(void);
void zephyr_getchar_init(void);

/* The port's real_main() calls this at every soft reset; app_usb.h has started USB once. */
int mp_usbd_init(void)
{
	return 0;
}

ESP_SDR_HIGH_STACK_DEFINE(mp_stack, CONFIG_APP_MP_STACK_SIZE);
static struct k_thread mp_thread;

static void mp_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	(void)real_main();
}

static void repin_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	/* The USB threads exist by now. */
	app_cpu_pin_system_threads(0);
}

static K_WORK_DELAYABLE_DEFINE(repin_work, repin_handler);

static int boot(void)
{
	app_cpu_pin_system_threads(0);
	if (app_usb_init() != 0) {
		LOG_ERR("USB init failed");
	}
	k_work_schedule(&repin_work, K_MSEC(500));
	return 0;
}

SYS_INIT(boot, APPLICATION, 99);

int main(void)
{
	/* REPL input: the console driver's receive hook. */
	zephyr_getchar_init();
	k_thread_create(&mp_thread, mp_stack, K_KERNEL_STACK_SIZEOF(mp_stack), mp_entry, NULL,
			NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY, K_FP_REGS, K_FOREVER);
	k_thread_name_set(&mp_thread, "micropython");
#if defined(CONFIG_SCHED_CPU_MASK)
	/* CPU 1 belongs to the receive ring while it runs. */
	if (arch_num_cpus() > 1) {
		k_thread_cpu_pin(&mp_thread, 0);
	}
#endif
	k_thread_start(&mp_thread);
	return 0;
}
