/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module's board functions, the same wherever the radio is: the launcher's next app
 * and home gesture (the knob, or a key held), the display's size, the reset reason, and the
 * sample format converters. modesdr.c (the S3), esdr_radio.c (the P4, the ESP32) and esdr_c5.c
 * (the C5: no knob, no display) put them in their module tables (esdr_board.h), and share the
 * helpers of their radio bindings.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_SHELL_BACKEND_DUMMY)
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#endif
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include <esp_system.h>

#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"

#include "esdr_board.h"

/*
 * ---- the launcher (modules/launcher.py) ----
 *
 * The app to start survives the soft reset launcher.start() makes, outside the heap. Holding
 * the knob (the LVGL encoder's switch, or APP_HOME_CODE on keys that drive LVGL as a keypad) for
 * KNOB_HOME_MS ends what runs as Ctrl-C does, flagged so that the launcher shows its menu
 * rather than the REPL. When no Ctrl-C can help (the REPL waits for input, keyboard interrupts
 * are off, what runs ignored a hold KNOB_AGAIN_MS ago), holding the knob restarts the board.
 * A hold counts for the launcher for KNOB_FRESH_MS: a hold whose Ctrl-C an app swallowed does
 * not turn a later Ctrl-C from the console into one.
 */

#define KNOB_HOME_MS  1500
#define KNOB_AGAIN_MS 5000
#define KNOB_FRESH_MS 15000
#define KNOB_NODE     DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_encoder_input)
#define KEYPAD_NODE   DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_keypad_input)

#if DT_NODE_EXISTS(KNOB_NODE)
#define HOME_CODE DT_PROP(KNOB_NODE, button_input_code)
#elif DT_NODE_EXISTS(KEYPAD_NODE)
#define HOME_CODE CONFIG_APP_HOME_CODE
#endif

static char next_app[128];
static atomic_t knob_home;
static int64_t knob_time;

#ifdef HOME_CODE
static void knob_held(struct k_work *work)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(work);
	if (mp_interrupt_char < 0 ||
	    (atomic_get(&knob_home) != 0 && now - knob_time < KNOB_AGAIN_MS)) {
		sys_reboot(SYS_REBOOT_COLD);
	}
	knob_time = now;
	(void)atomic_set(&knob_home, 1);
	mp_hal_signal_event();
	mp_sched_keyboard_interrupt();
}

static K_WORK_DELAYABLE_DEFINE(knob_work, knob_held);

static void knob_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY || evt->code != HOME_CODE) {
		return;
	}
	if (evt->value != 0) {
		(void)k_work_reschedule(&knob_work, K_MSEC(KNOB_HOME_MS));
	} else {
		(void)k_work_cancel_delayable(&knob_work);
	}
}
INPUT_CALLBACK_DEFINE(NULL, knob_input, NULL);
#endif

void esdr_board_init(void)
{
	(void)atomic_clear(&knob_home);
}

/* ---- the radio's bindings ---- */

void esdr_check(int ret)
{
	if (ret < 0) {
		mp_raise_OSError(-ret);
	}
}

void esdr_gen_from(struct esdr_gen *g, mp_obj_t t)
{
	mp_obj_t *items;
	size_t len;

	mp_obj_get_array(t, &len, &items);
	if (len != 5) {
		mp_raise_ValueError(MP_ERROR_TEXT("gen: (wave, cycles, amp, dc_i, dc_q)"));
	}
	mp_int_t wave = mp_obj_get_int(items[0]);

	/* gnuradio.analog's GR_CONST_WAVE .. GR_SAW_WAVE are 100 .. 105. */
	g->wave = (enum esdr_wave)(wave >= 100 ? wave - 100 : wave);
	g->cycles = (float)mp_obj_get_float(items[1]);
	g->amp = (float)mp_obj_get_float(items[2]);
	g->dc_i = (float)mp_obj_get_float(items[3]);
	g->dc_q = (float)mp_obj_get_float(items[4]);
}

/* fc32_to_sc16(src, dst, scale): float32 pairs times scale to clipped int16 pairs, as many as
 * both hold; returns that count of samples. */
static mp_obj_t esdr_fc32_to_sc16(mp_obj_t src_in, mp_obj_t dst_in, mp_obj_t scale_in)
{
	mp_buffer_info_t src, dst;
	float scale = (float)mp_obj_get_float(scale_in);

	mp_get_buffer_raise(src_in, &src, MP_BUFFER_READ);
	mp_get_buffer_raise(dst_in, &dst, MP_BUFFER_WRITE);
	size_t n = MIN(src.len / 4U, dst.len / 2U) & ~1U;
	const float *s = src.buf;
	int16_t *o = dst.buf;

	if ((((uintptr_t)s) & 3U) != 0U || (((uintptr_t)o) & 1U) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("unaligned buffer"));
	}
	for (size_t k = 0; k < n; k++) {
		float v = s[k] * scale;

		o[k] = (int16_t)(v > 32767.0f ? 32767 : (v < -32767.0f ? -32767 : (int)v));
	}
	return mp_obj_new_int_from_uint(n / 2U);
}
MP_DEFINE_CONST_FUN_OBJ_3(esdr_fc32_to_sc16_obj, esdr_fc32_to_sc16);

/* sc16_to_fc32(src, dst, scale): as many pairs as both hold; returns that count. */
static mp_obj_t esdr_sc16_to_fc32(mp_obj_t src_in, mp_obj_t dst_in, mp_obj_t scale_in)
{
	mp_buffer_info_t src, dst;
	float scale = (float)mp_obj_get_float(scale_in);

	mp_get_buffer_raise(src_in, &src, MP_BUFFER_READ);
	mp_get_buffer_raise(dst_in, &dst, MP_BUFFER_WRITE);
	size_t n = MIN(src.len / 2U, dst.len / 4U);
	const int16_t *s = src.buf;
	float *o = dst.buf;

	if ((((uintptr_t)s) & 1U) != 0U || (((uintptr_t)o) & 3U) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("unaligned buffer"));
	}
	for (size_t k = 0; k < n; k++) {
		o[k] = (float)s[k] * scale;
	}
	return mp_obj_new_int_from_uint(n / 2U);
}
MP_DEFINE_CONST_FUN_OBJ_3(esdr_sc16_to_fc32_obj, esdr_sc16_to_fc32);

/* display_size() -> (width, height): the panel LVGL draws on (chosen zephyr,display), (0, 0)
 * without one. Known before LVGL starts, for the layouts. */
#if DT_HAS_CHOSEN(zephyr_display)
#define DISPLAY_W DT_PROP(DT_CHOSEN(zephyr_display), width)
#define DISPLAY_H DT_PROP(DT_CHOSEN(zephyr_display), height)
#else
#define DISPLAY_W 0
#define DISPLAY_H 0
#endif

static mp_obj_t esdr_display_size(void)
{
	mp_obj_t size[2] = {MP_OBJ_NEW_SMALL_INT(DISPLAY_W), MP_OBJ_NEW_SMALL_INT(DISPLAY_H)};

	return mp_obj_new_tuple(2, size);
}
MP_DEFINE_CONST_FUN_OBJ_0(esdr_display_size_obj, esdr_display_size);

/* reset_reason() -> str: why the chip last started (esp_reset_reason()). Catches what crash()
 * cannot: power-on, brownout, the hardware watchdogs, a USB or JTAG reset. */
static mp_obj_t esdr_reset_reason(void)
{
	static const char *const names[] = {
		"unknown", "power-on", "external pin", "software", "panic", "interrupt watchdog",
		"task watchdog", "other watchdog", "deep sleep", "brownout", "sdio", "usb", "jtag",
		"efuse", "power glitch", "cpu lockup",
	};
	unsigned int r = (unsigned int)esp_reset_reason();

	return mp_obj_new_str_from_cstr(r < ARRAY_SIZE(names) ? names[r] : "?");
}
MP_DEFINE_CONST_FUN_OBJ_0(esdr_reset_reason_obj, esdr_reset_reason);

/* next_app([path]) -> str: with a path, the app main.py starts after the next soft reset;
 * without, that app ("" for none), forgotten. */
static mp_obj_t esdr_next_app(size_t n_args, const mp_obj_t *args)
{
	char out[sizeof(next_app)];
	const char *path;
	size_t len;

	if (n_args == 0) {
		/* Forgotten first: whatever the string does, the next soft reset starts the menu. */
		strcpy(out, next_app);
		next_app[0] = '\0';
		return mp_obj_new_str_from_cstr(out);
	}
	if (!mp_obj_is_str(args[0])) {
		mp_raise_TypeError(MP_ERROR_TEXT("path must be a str"));
	}
	path = mp_obj_str_get_data(args[0], &len);
	if (len >= sizeof(next_app) || memchr(path, '\0', len) != NULL) {
		mp_raise_ValueError(MP_ERROR_TEXT("path too long or with a NUL"));
	}
	memcpy(next_app, path, len);
	next_app[len] = '\0';
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_next_app_obj, 0, 1, esdr_next_app);

/* home() -> bool: the knob was held since the last call, recently enough that the Ctrl-C
 * which ended the app was the knob's: "back to the launcher". */
static mp_obj_t esdr_home(void)
{
	bool held = atomic_clear(&knob_home) != 0;

	return mp_obj_new_bool(held && k_uptime_get() - knob_time < KNOB_FRESH_MS);
}
MP_DEFINE_CONST_FUN_OBJ_0(esdr_home_obj, esdr_home);

/*
 * shift() -> bool: a Shift key is held (a keyboard that reports INPUT_KEY_LEFTSHIFT or
 * INPUT_KEY_RIGHTSHIFT); LVGL's keypad does not carry modifiers, the GUI's sliders take
 * bigger steps with it.
 */
static atomic_t shift_keys;

#ifdef CONFIG_INPUT
static void shift_input(struct input_event *evt, void *user_data)
{
	atomic_val_t bit;

	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == INPUT_KEY_LEFTSHIFT) {
		bit = BIT(0);
	} else if (evt->code == INPUT_KEY_RIGHTSHIFT) {
		bit = BIT(1);
	} else {
		return;
	}
	if (evt->value != 0) {
		(void)atomic_or(&shift_keys, bit);
	} else {
		(void)atomic_and(&shift_keys, ~bit);
	}
}
INPUT_CALLBACK_DEFINE(NULL, shift_input, NULL);
#endif

static mp_obj_t esdr_shift(void)
{
	return mp_obj_new_bool(atomic_get(&shift_keys) != 0);
}
MP_DEFINE_CONST_FUN_OBJ_0(esdr_shift_obj, esdr_shift);

#if defined(CONFIG_SHELL_BACKEND_DUMMY)
/* shell(cmd) -> str: run a shell command on the dummy backend, return what it printed. The
 * port's zephyr.shell_exec() needs the serial backend, which the REPL's console cannot share. */
static mp_obj_t esdr_shell(mp_obj_t cmd_in)
{
	const char *cmd = mp_obj_str_get_str(cmd_in);
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t len = 0;
	const char *out;
	int ret;

	if (sh == NULL) {
		mp_raise_OSError(ENODEV);
	}
	shell_backend_dummy_clear_output(sh);
	ret = shell_execute_cmd(sh, cmd);
	out = shell_backend_dummy_get_output(sh, &len);
	if (ret < 0 && len == 0) {
		mp_raise_OSError(-ret);
	}
	return mp_obj_new_str(out, len);
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_shell_obj, esdr_shell);
#endif
