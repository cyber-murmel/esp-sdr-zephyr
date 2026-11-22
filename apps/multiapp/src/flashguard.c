/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A flash operation from a thread whose stack is in PSRAM is refused. The flash driver turns the cache off for the
 * operation, and the thread's own stack goes with it: the board hangs silently (doc/dev/network-services.md, phase 1).
 * MicroPython's thread has its stack in PSRAM (ext/mp/mp_repl_s31.c) and must send what can reach the flash to its
 * worker; this turns a slip into an error return and a log line. Wraps (CMakeLists.txt) of the HAL's esp_flash calls
 * that the flash driver makes.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_flash.h>

LOG_MODULE_REGISTER(flashguard, LOG_LEVEL_INF);

#define PSRAM_LO 0x50000000UL
#define PSRAM_HI 0x58000000UL

static bool refused_here(const char *what)
{
	const struct k_thread *t = k_current_get();
	uintptr_t base;

	/* Early boot and interrupts have no thread of their own to judge */
	if (t == NULL || k_is_in_isr()) {
		return false;
	}
	base = t->stack_info.start;
	if (base < PSRAM_LO || base >= PSRAM_HI) {
		return false;
	}
	LOG_ERR("%s from thread %s, whose stack is in PSRAM: refused (it would stall the cache)", what,
		k_thread_name_get(k_current_get()) != NULL ? k_thread_name_get(k_current_get()) : "?");
	return true;
}

esp_err_t __real_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);
esp_err_t __real_esp_flash_read_encrypted(esp_flash_t *chip, uint32_t address, void *out_buffer, uint32_t length);
esp_err_t __real_esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address, const void *buffer, uint32_t length);

esp_err_t __wrap_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length)
{
	return refused_here("flash read") ? ESP_ERR_NOT_SUPPORTED : __real_esp_flash_read(chip, buffer, address, length);
}

esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
	return refused_here("flash write") ? ESP_ERR_NOT_SUPPORTED
					   : __real_esp_flash_write(chip, buffer, address, length);
}

esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len)
{
	return refused_here("flash erase") ? ESP_ERR_NOT_SUPPORTED : __real_esp_flash_erase_region(chip, start, len);
}

esp_err_t __wrap_esp_flash_read_encrypted(esp_flash_t *chip, uint32_t address, void *out_buffer, uint32_t length)
{
	return refused_here("flash read") ? ESP_ERR_NOT_SUPPORTED
					  : __real_esp_flash_read_encrypted(chip, address, out_buffer, length);
}

esp_err_t __wrap_esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address, const void *buffer, uint32_t length)
{
	return refused_here("flash write") ? ESP_ERR_NOT_SUPPORTED
					   : __real_esp_flash_write_encrypted(chip, address, buffer, length);
}

#if !defined(CONFIG_SSH_SERVER)
#include <errno.h>
#include <multiapp/multiapp.h>

/* The shell instance of the host's commands is the SSH server's (console_mux.c): without it there is none */
int multiapp_shell_exec(const char *cmd, multiapp_shell_out_fn out, void *ctx)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(out);
	ARG_UNUSED(ctx);
	return -ENOSYS;
}
#endif
