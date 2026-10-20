/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A turned panel's writes (the WHY2025 badge: the ESP32-P4 DSI driver's swap-xy) land
 * transposed in the framebuffer: source pixel (col, row) of a write at (x, y) at framebuffer row
 * x + col, column y + row. Writes blocks of distinct pixels, reads the framebuffer back
 * (display_read() reads it as it is, untransposed) and compares; then times full-width writes
 * as LVGL makes them.
 */

#include <string.h>

#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/ztest.h>

#define W_MAX 720U
#define H_MAX 48U

static const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static uint16_t src[W_MAX * H_MAX] __aligned(64);
static uint16_t back[W_MAX * H_MAX] __aligned(64);
static struct display_capabilities caps;

static void fill(uint16_t w, uint16_t h, bool random)
{
	for (uint32_t r = 0; r < h; r++) {
		for (uint32_t c = 0; c < w; c++) {
			src[r * w + c] = random ? (uint16_t)sys_rand32_get()
						: (uint16_t)((r << 10) ^ (c * 7U) ^ 0x5a5aU);
		}
	}
}

/* Mismatching pixels of a w x h write at (x, y), read back from the framebuffer. */
static uint32_t check(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	struct display_buffer_descriptor d = {
		.buf_size = sizeof(src), .width = w, .height = h, .pitch = w,
	};
	struct display_buffer_descriptor rd = {
		.buf_size = sizeof(back), .width = h, .height = w, .pitch = h,
	};
	uint32_t bad = 0;

	zassert_ok(display_write(dev, x, y, &d, src), "write %ux%u at %u,%u", w, h, x, y);
	/* With two framebuffers the written one goes on screen at the next frame (17 ms), and
	 * display_read() reads what is on screen.
	 */
	k_msleep(50);
	/* The framebuffer's columns y.. of rows x..: untransposed coordinates. */
	zassert_ok(display_read(dev, y, x, &rd, back), "read");
	for (uint32_t r = 0; r < h; r++) {
		for (uint32_t c = 0; c < w; c++) {
			bad += back[c * h + r] != src[r * w + c];
		}
	}
	return bad;
}

ZTEST(display_transpose, test_a_sizes)
{
	static const uint16_t sizes[][4] = {
		/* x, y, w, h */
		{0, 0, 720, 43},   {0, 677, 720, 43}, {100, 200, 40, 24}, {3, 5, 1, 1},
		{719, 719, 1, 1},  {10, 11, 17, 33}, {200, 300, 64, 32}, {7, 9, 300, 65},
		{0, 0, 720, 48},   {511, 13, 209, 31}, {1, 2, 720 - 1, 47}, {0, 0, 720, 33},
		{5, 50, 300, 97},
	};

	uint32_t fails = 0;

	for (size_t k = 0; k < ARRAY_SIZE(sizes); k++) {
		uint16_t x = sizes[k][0], y = sizes[k][1], w = sizes[k][2], h = sizes[k][3];
		uint32_t bad;

		if (x + w > caps.x_resolution || y + h > caps.y_resolution) {
			continue;
		}
		fill(w, h, true);
		bad = check(x, y, w, h);
		TC_PRINT("%ux%u at %u,%u: %u of %u pixels wrong\n", w, h, x, y, bad, w * h);
		fails += bad != 0U;
	}
	zassert_equal(fails, 0, "%u blocks wrong", fails);
}

/*
 * Earlier frames stay: with two framebuffers each frame starts from a copy of what the frame on
 * screen changed. Three blocks in three frames, then each must still be there.
 */
ZTEST(display_transpose, test_b_history)
{
	static const uint16_t blk[3][4] = {{10, 20, 64, 40}, {400, 600, 200, 33}, {650, 5, 70, 48}};
	static uint16_t keep[3][64 * 48 + 200 * 33];
	struct display_buffer_descriptor d = {.buf_size = sizeof(src)};
	struct display_buffer_descriptor rd = {.buf_size = sizeof(back)};

	for (int k = 0; k < 3; k++) {
		d.width = blk[k][2];
		d.height = blk[k][3];
		d.pitch = blk[k][2];
		fill(blk[k][2], blk[k][3], true);
		memcpy(keep[k], src, (size_t)blk[k][2] * blk[k][3] * 2U);
		zassert_ok(display_write(dev, blk[k][0], blk[k][1], &d, src));
		k_msleep(50);
	}
	for (int k = 0; k < 3; k++) {
		uint16_t x = blk[k][0], y = blk[k][1], w = blk[k][2], h = blk[k][3];
		uint32_t bad = 0;

		rd.width = h;
		rd.height = w;
		rd.pitch = h;
		zassert_ok(display_read(dev, y, x, &rd, back));
		for (uint32_t r = 0; r < h; r++) {
			for (uint32_t c = 0; c < w; c++) {
				bad += back[c * h + r] != keep[k][r * w + c];
			}
		}
		TC_PRINT("block %d (%ux%u at %u,%u) after later frames: %u wrong\n", k, w, h, x, y,
			 bad);
		zassert_equal(bad, 0, "block %d lost", k);
	}
}

ZTEST(display_transpose, test_c_speed)
{
	struct display_buffer_descriptor d = {
		.buf_size = sizeof(src), .width = W_MAX, .height = 43, .pitch = W_MAX,
	};
	uint32_t t0, us;

	fill(W_MAX, 43, true);
	t0 = k_cycle_get_32();
	for (int k = 0; k < 17; k++) {
		/* A screen in 6 % bands, as LVGL draws it. */
		d.frame_incomplete = k < 16;
		zassert_ok(display_write(dev, 0, (uint16_t)(k * 42), &d, src));
	}
	us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
	TC_PRINT("full screen in 17 writes of 720x43: %u us (%u ns a pixel)\n", us,
		 us * 1000U / (17U * W_MAX * 43U));

	/* A frame of one pixel once the screen has settled: what starting a frame costs (with two
	 * framebuffers, the copy of the one on screen).
	 */
	d.width = 1;
	d.height = 1;
	d.pitch = 1;
	d.frame_incomplete = false;
	k_msleep(100);
	t0 = k_cycle_get_32();
	zassert_ok(display_write(dev, 0, 0, &d, src));
	TC_PRINT("a one-pixel frame: %u us\n", k_cyc_to_us_floor32(k_cycle_get_32() - t0));
}

static void *setup(void)
{
	zassert_true(device_is_ready(dev), "display not ready");
	display_get_capabilities(dev, &caps);
	zassert_equal(caps.current_pixel_format, PIXEL_FORMAT_RGB_565, "RGB565 expected");
	(void)display_blanking_off(dev);
	return NULL;
}

ZTEST_SUITE(display_transpose, NULL, setup, NULL, NULL, NULL);
