/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the wifi module gets from the rest of a build with the driver on. The host is built without
 * it, so it does not start esp_timer, has no wifi_module_enable(), and has a kernel heap for its
 * own needs: the driver and the blobs allocate from the area instead (--wrap, wifi.cmake), which
 * no other boot's module uses in a Wi-Fi boot.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/wifi/esp_wifi/esp_wifi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/math_extras.h>
#include <zephyr/sys/printk.h>

#include <esp_private/periph_ctrl.h>
#include <esp_private/wifi.h>
#include <esp_timer.h>
#include <hal/clk_gate_ll.h>

#include <esp_sdr/esp_sdr_arb.h>
#include <esp_sdr/esp_sdr_place.h>
#include <multiapp/multiapp.h>

ESP_SDR_BUF_DEFINE(STATE, uint8_t, wifi_heap_mem, [CONFIG_MULTIAPP_AREA_SIZE], 16, NOINIT);

static struct k_heap wifi_heap;
static int wifi_clock_refs;

/* Before anything else in the module allocates. */
static void __attribute__((constructor(101))) wifi_heap_setup(void)
{
	k_heap_init(&wifi_heap, wifi_heap_mem, sizeof(wifi_heap_mem));
}

static void __attribute__((constructor(102))) wifi_timer_setup(void)
{
	esp_err_t err = esp_timer_init();

	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		printk("wifi: esp_timer_init failed (%d)\n", err);
	}
}

void *__wrap_k_malloc(size_t size)
{
	return k_heap_alloc(&wifi_heap, size, K_NO_WAIT);
}

void *__wrap_k_aligned_alloc(size_t align, size_t size)
{
	return k_heap_aligned_alloc(&wifi_heap, align, size, K_NO_WAIT);
}

void *__wrap_k_calloc(size_t nmemb, size_t size)
{
	size_t bytes;
	void *mem;

	if (size_mul_overflow(nmemb, size, &bytes)) {
		return NULL;
	}
	mem = k_heap_alloc(&wifi_heap, bytes, K_NO_WAIT);
	if (mem != NULL) {
		memset(mem, 0, bytes);
	}
	return mem;
}

void *__wrap_k_realloc(void *ptr, size_t size)
{
	return k_heap_realloc(&wifi_heap, ptr, size, K_NO_WAIT);
}

void __wrap_k_free(void *ptr)
{
	k_heap_free(&wifi_heap, ptr);
}

k_thread_stack_t *__wrap_z_impl_k_thread_stack_alloc(size_t size, int flags)
{
	ARG_UNUSED(flags);
	return k_heap_aligned_alloc(&wifi_heap, Z_KERNEL_STACK_OBJ_ALIGN, K_KERNEL_STACK_LEN(size),
				    K_NO_WAIT);
}

int __wrap_z_impl_k_thread_stack_free(k_thread_stack_t *stack)
{
	k_heap_free(&wifi_heap, stack);
	return 0;
}

void wifi_module_enable(void)
{
	periph_rcc_enter();
	if (wifi_clock_refs == 0) {
		periph_ll_wifi_module_enable_clk_clear_rst();
	}
	wifi_clock_refs++;
	periph_rcc_exit();
}

void wifi_module_disable(void)
{
	periph_rcc_enter();
	wifi_clock_refs--;
	if (wifi_clock_refs == 0) {
		periph_ll_wifi_module_disable_clk_set_rst();
	}
	periph_rcc_exit();
}

/*
 * Its device is up (the host's init table walk): the host's interface takes it over, and the
 * radio arbiter learns that the SDR has to share the PHY from here on.
 */
int multiapp_module_up(void)
{
	esp_sdr_arb_wifi_attach();
	return multiapp_wifi_bind(DEVICE_DT_GET(DT_NODELABEL(wifi)));
}

static void flush_done(struct k_work *work)
{
	ARG_UNUSED(work);
}

/*
 * The inverse of multiapp_module_up(), called by the host before it unloads the module: the
 * driver's stop path (which leaves the blob, its tasks and the supplicant deinitialized), the
 * interrupts the blob installed, the host's interface, the arbiter's attach. -EBUSY and the like
 * leave the module loaded.
 */
int multiapp_module_down(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(wifi));
	static struct k_work flush;
	struct k_work_sync sync;
	struct sys_memory_stats heap_stats;
	int ret;

	ret = esp32_wifi_shutdown(dev);
	if (ret != 0 && ret != -EALREADY) {
		return ret;
	}
	/* The blob's tasks end through a work item of this module on the system queue. */
	k_work_init(&flush, flush_done);
	(void)k_work_submit(&flush);
	(void)k_work_flush(&flush, &sync);

	esp_wifi_adapter_release_isrs();
	(void)multiapp_wifi_unbind(dev);
	esp_sdr_arb_wifi_detach();

	periph_rcc_enter();
	if (wifi_clock_refs != 0) {
		printk("wifi: %d clock references left, dropped\n", wifi_clock_refs);
		wifi_clock_refs = 0;
		periph_ll_wifi_module_disable_clk_set_rst();
	}
	periph_rcc_exit();

	/* What is still allocated from the private heap is a leak: the next load zeroes the heap. */
	if (sys_heap_runtime_stats_get(&wifi_heap.heap, &heap_stats) == 0 &&
	    heap_stats.allocated_bytes != 0) {
		printk("wifi: %zu bytes of the module heap not freed\n", heap_stats.allocated_bytes);
	}
	return 0;
}
