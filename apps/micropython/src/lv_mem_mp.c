/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LVGL's allocator on the MicroPython heap (LV_STDLIB_MICROPYTHON), in place of LVGL's
 * src/stdlib/micropython/lv_mem_core_micropython.c, whose m_malloc() raises MemoryError: the
 * longjmp would cross LVGL's C frames and leave its event list and guards behind. These return
 * NULL, as LVGL expects, also while the GC is locked (finalisers).
 */

#include <lvgl.h>

#include "py/gc.h"
#include "py/misc.h"

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
	LV_UNUSED(mem);
	LV_UNUSED(bytes);
	return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
	LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
	return m_malloc_maybe(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
	return m_realloc_maybe(p, new_size, true);
}

void lv_free_core(void *p)
{
	m_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
	LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
	return LV_RESULT_OK;
}
