/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The display and the touch screen over the console, whatever the REPL or an app is doing, for
 * tools/screen.py at the other end of the UART (CONFIG_APP_CONSOLE_REMOTE). A byte 0x1e starts a
 * command and a newline ends it; neither that nor the command reaches MicroPython. A Ctrl-C, or
 * no byte for CMD_GAP_MS, ends it unheard: such a byte was not for here after all.
 *
 *   s [step]       the screen as lines of text at 921600 baud, every step-th pixel of every
 *                  step-th row: 0x1e "FB width height" then a line per row, then 0x1e "END"
 *   t x y down     a finger: down 1 or 0, at x, y
 *   k code value   a key by its input code, as the key's driver would report it
 *   c              frames and underruns of the panel since the start: 0x1e "COUNT frames underruns"
 *
 * A row's line is "<y> <how> <base64> <sum>": how is '=' for the same pixels as the row before
 * (nothing follows), 'L' for little-endian RGB565 pixels, 'R' for runs (count, pixel) of 16-bit
 * little-endian words; sum is the low byte of the base64's bytes added up, in hex.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/console/uart_console.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <esp_sdr/esp_sdr_place.h>

#define START_BYTE 0x1eU
#define DUMP_BAUD  921600U
/* What the other side gets to change its speed. */
#define BAUD_MS    100
#define CMD_MAX    32
#define CMD_GAP_MS 100
/* Commands a slow thread may be behind by. */
#define CMD_QUEUE  8
#define CTRL_C     0x03U

#define STACK 2048
/* As MicroPython, which gets its half of the CPU while a screen goes out. */
#define PRIO  CONFIG_MAIN_THREAD_PRIORITY

#define POINTER_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_pointer_input)
#define TOUCH_DEV    DEVICE_DT_GET(DT_PHANDLE(POINTER_NODE, input))

BUILD_ASSERT(DT_HAS_CHOSEN(zephyr_display), "there is no screen to send");

#define WIDTH  DT_PROP(DT_CHOSEN(zephyr_display), width)
#define HEIGHT DT_PROP(DT_CHOSEN(zephyr_display), height)

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uart_console_in_debug_hook_t port_hook;
K_MSGQ_DEFINE(cmd_queue, CMD_MAX, CMD_QUEUE, 1);

/*
 * A line of the screen and the one sent before, in PSRAM where the linker script has a section
 * for it (the S31's internal RAM is full; this thread only copies them).
 */
static uint16_t row[WIDTH] ESP_SDR_DATA(BULK, row), last[WIDTH] ESP_SDR_DATA(BULK, last);

static atomic_t frames, underruns;

static enum display_event_result count_event(const struct device *dev, uint32_t evt,
					     const struct display_event_data *data, void *user)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);
	ARG_UNUSED(user);
	if (evt == DISPLAY_EVENT_VSYNC) {
		atomic_inc(&frames);
	} else {
		atomic_inc(&underruns);
	}
	return DISPLAY_EVENT_RESULT_CONTINUE;
}

static int input_filter(uint8_t c)
{
	static char cmd[CMD_MAX];
	static size_t cmd_len;
	static bool in_cmd;
	static uint32_t last_ms;
	const uint32_t now_ms = k_uptime_get_32();

	if (in_cmd && (c == CTRL_C || now_ms - last_ms > CMD_GAP_MS)) {
		in_cmd = false;
	}
	last_ms = now_ms;
	if (!in_cmd) {
		if (c == START_BYTE) {
			in_cmd = true;
			cmd_len = 0U;
			return 1;
		}
		return port_hook != NULL ? port_hook(c) : 0;
	}
	if (c == '\n' || c == '\r') {
		in_cmd = false;
		memset(&cmd[cmd_len], 0, CMD_MAX - cmd_len);
		if (k_msgq_put(&cmd_queue, cmd, K_NO_WAIT) != 0) {
			/* The oldest goes: the newest may be the finger's lift. */
			char oldest[CMD_MAX];

			(void)k_msgq_get(&cmd_queue, oldest, K_NO_WAIT);
			(void)k_msgq_put(&cmd_queue, cmd, K_NO_WAIT);
		}
	} else if (cmd_len < CMD_MAX - 1U) {
		cmd[cmd_len++] = (char)c;
	}
	return 1;
}

/* The port installs its console input here: this looks at the bytes first. */
void __real_uart_console_in_debug_hook_install(uart_console_in_debug_hook_t hook);

void __wrap_uart_console_in_debug_hook_install(uart_console_in_debug_hook_t hook)
{
	port_hook = hook;
	__real_uart_console_in_debug_hook_install(input_filter);
}

static void put(const char *s, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		uart_poll_out(uart, (unsigned char)s[k]);
	}
}

static void puts_uart(const char *s)
{
	put(s, strlen(s));
}

static struct {
	uint32_t bits;
	uint8_t count;
	uint8_t sum;
} b64;

static void b64_char(uint32_t v)
{
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char c = table[v & 0x3fU];

	b64.sum += (uint8_t)c;
	put(&c, 1U);
}

static void b64_byte(uint8_t v)
{
	b64.bits = (b64.bits << 8) | v;
	if (++b64.count == 3U) {
		b64_char(b64.bits >> 18);
		b64_char(b64.bits >> 12);
		b64_char(b64.bits >> 6);
		b64_char(b64.bits);
		b64.bits = 0U;
		b64.count = 0U;
	}
}

static void b64_word(uint16_t v)
{
	b64_byte((uint8_t)v);
	b64_byte((uint8_t)(v >> 8));
}

static void b64_end(void)
{
	char pad = '=';

	if (b64.count == 1U) {
		b64.bits <<= 16;
		b64_char(b64.bits >> 18);
		b64_char(b64.bits >> 12);
		b64.sum += (uint8_t)pad * 2U;
		put("==", 2U);
	} else if (b64.count == 2U) {
		b64.bits <<= 8;
		b64_char(b64.bits >> 18);
		b64_char(b64.bits >> 12);
		b64_char(b64.bits >> 6);
		b64.sum += (uint8_t)pad;
		put(&pad, 1U);
	}
	b64.bits = 0U;
	b64.count = 0U;
}

/* @return The speed it had, 0 if it has no other. */
static uint32_t set_baud(uint32_t baud)
{
	struct uart_config cfg;
	uint32_t before;

	if (uart_config_get(uart, &cfg) != 0) {
		return 0U;
	}
	before = cfg.baudrate;
	cfg.baudrate = baud;
	return uart_configure(uart, &cfg) == 0 ? before : 0U;
}

static void send_row(uint32_t y, const uint16_t *pixels, size_t n)
{
	char text[24];
	size_t runs = 1U;

	for (size_t k = 1U; k < n; k++) {
		runs += pixels[k] != pixels[k - 1U];
	}
	/* A run is two words, a pixel one. */
	(void)snprintf(text, sizeof(text), "%u %c ", y, runs * 2U < n ? 'R' : 'L');
	puts_uart(text);
	b64.sum = 0U;
	if (runs * 2U < n) {
		uint16_t count = 1U;

		for (size_t k = 1U; k <= n; k++) {
			if (k < n && pixels[k] == pixels[k - 1U]) {
				count++;
			} else {
				b64_word(count);
				b64_word(pixels[k - 1U]);
				count = 1U;
			}
		}
	} else {
		for (size_t k = 0U; k < n; k++) {
			b64_word(pixels[k]);
		}
	}
	b64_end();
	(void)snprintf(text, sizeof(text), " %02x\n", b64.sum);
	puts_uart(text);
}

static void send_screen(uint32_t step)
{
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(row),
		.width = WIDTH,
		.height = 1U,
		.pitch = WIDTH,
	};
	const size_t n = WIDTH / step;
	char text[32];
	bool have_last = false;
	uint32_t baud;

	k_msleep(BAUD_MS);
	baud = set_baud(DUMP_BAUD);
	if (baud == 0U) {
		return;
	}
	k_msleep(BAUD_MS);
	(void)snprintf(text, sizeof(text), "\x1e" "FB %u %u\n", (unsigned int)n,
		       HEIGHT / step);
	puts_uart(text);
	for (uint32_t y = 0U; y < HEIGHT; y += step) {
		if (y / step >= HEIGHT / step) {
			break;
		}
		if (display_read(disp, 0U, (uint16_t)y, &desc, row) != 0) {
			break;
		}
		for (size_t k = 0U; k < n; k++) {
			row[k] = row[k * step];
		}
		if (have_last && memcmp(row, last, n * sizeof(row[0])) == 0) {
			(void)snprintf(text, sizeof(text), "%u = 0\n", y / step);
			puts_uart(text);
			continue;
		}
		send_row(y / step, row, n);
		memcpy(last, row, n * sizeof(row[0]));
		have_last = true;
	}
	puts_uart("\x1e" "END\n");
	k_msleep(BAUD_MS);
	(void)set_baud(baud);
}

static void remote_thread(void *a, void *b, void *c)
{
	uint32_t handle;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Only a panel with events has these to tell. */
	(void)display_register_event_cb(disp, count_event, NULL,
					DISPLAY_EVENT_VSYNC | DISPLAY_EVENT_FIFO_UNDERFLOW, true,
					&handle);
	for (;;) {
		char copy[CMD_MAX];
		int v[3] = {0, 0, 0};
		char *p;
		char kind;

		(void)k_msgq_get(&cmd_queue, copy, K_FOREVER);
		kind = copy[0];
		p = copy + 1;
		for (size_t k = 0U; k < ARRAY_SIZE(v) && *p != '\0'; k++) {
			v[k] = (int)strtol(p, &p, 10);
		}
		switch (kind) {
		case 's':
			send_screen(CLAMP(v[0], 1, 16));
			break;
#if DT_NODE_EXISTS(POINTER_NODE)
		case 't':
			(void)input_report_abs(TOUCH_DEV, INPUT_ABS_X, v[0], false, K_FOREVER);
			(void)input_report_abs(TOUCH_DEV, INPUT_ABS_Y, v[1], false, K_FOREVER);
			(void)input_report_key(TOUCH_DEV, INPUT_BTN_TOUCH, v[2], true, K_FOREVER);
			break;
#endif
		case 'k':
			(void)input_report_key(NULL, (uint16_t)v[0], v[1], true, K_FOREVER);
			break;
		case 'c':
			(void)snprintf(copy, sizeof(copy), "\x1e" "COUNT %ld %ld\n",
				       (long)atomic_get(&frames), (long)atomic_get(&underruns));
			puts_uart(copy);
			break;
		default:
			break;
		}
	}
}

K_THREAD_DEFINE(console_remote, STACK, remote_thread, NULL, NULL, NULL, PRIO, 0, 0);
