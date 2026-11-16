/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gateware's DSP chain (gateware/esdr_fpga/dsp.py), configured in dB: window, averaging
 * table, the offset that puts a full-scale tone at 0 dB (as esdr.psd() does) and the scales
 * from dB to waterfall index and trace row. The arithmetic matches gateware/esdr_fpga/model.py.
 */

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>
#include <esp_sdr/esp_sdr_dsp.h>

/* dB per L unit: L is 256 log2 of power. */
#define DB_PER_L (10.0f * 0.30103f / 256.0f)

/* The gateware's waterfall indices: 0 .. ESDR_FPGA_PAL_BG - 1. */
#define WF_TOP (ESDR_FPGA_PAL_BG - 1)

static int16_t sat16(float v)
{
	return (int16_t)CLAMP(lroundf(v), INT16_MIN, INT16_MAX);
}

int16_t esdr_fpga_db_to_l(float db)
{
	return sat16(db / DB_PER_L);
}

float esdr_fpga_l_to_db(int16_t l)
{
	return l * DB_PER_L;
}

int esdr_fpga_dsp_config(const struct device *dev, const struct esdr_fpga_dsp *cfg)
{
	const uint32_t n = 1U << cfg->log2n;
	uint16_t buf[512];
	float sum = 0.0f;
	int ret;

	if (cfg->log2n < 4 || cfg->log2n > 9 || cfg->cols == 0 || cfg->cols > 320 ||
	    cfg->wf_max_db <= cfg->wf_min_db || cfg->tr_top_db <= cfg->tr_bottom_db ||
	    cfg->alpha <= 0.0f || cfg->alpha > 1.0f) {
		return -EINVAL;
	}

	/* The first half of a periodic window (the gateware mirrors it), Q15. */
	for (uint32_t k = 0; k < n; k++) {
		float w = esp_sdr_window(cfg->window, k, n);

		sum += w;
		if (k < n / 2) {
			buf[k] = (uint16_t)MIN(lroundf(32768.0f * w), 32767);
		}
	}
	ret = esdr_fpga_reg_write(dev, ESDR_FPGA_WINDOW, buf, n / 2);
	if (ret) {
		return ret;
	}

	/* Average: s += F(L - s), F(d) = 256 log2((1 - a) + a 2^(d / 256)), d = 8 (i - 256). */
	for (int i = 0; i < 512; i++) {
		float d = 8.0f * (i - 256);
		float f = (1.0f - cfg->alpha) + cfg->alpha * exp2f(d / 256.0f);

		buf[i] = (uint16_t)sat16(256.0f * log2f(f));
	}
	ret = esdr_fpga_reg_write(dev, ESDR_FPGA_AVG_LUT, buf, 512);
	if (ret) {
		return ret;
	}

	/*
	 * |X|^2 of a full-scale tone (amplitude fs) is (fs sum w)^2: that is 0 dB. The
	 * waterfall maps wf_min_db..wf_max_db onto its indices, the trace tr_top_db..tr_bottom_db
	 * onto its rows; the gains are 8.8 and below 128.
	 */
	float fs = cfg->full_scale > 0.0f ? cfg->full_scale : 32768.0f;
	float l_offset = -512.0f * log2f(fs * sum);
	float wf_span = esdr_fpga_db_to_l(cfg->wf_max_db) - esdr_fpga_db_to_l(cfg->wf_min_db);
	float tr_span = esdr_fpga_db_to_l(cfg->tr_top_db) - esdr_fpga_db_to_l(cfg->tr_bottom_db);
	uint16_t avg = cfg->alpha < 1.0f;
	const uint16_t regs[] = {
		/* DSP_CTRL is written last, by esdr_fpga_dsp_run(). */
		[ESDR_FPGA_REG_FFT_CFG - ESDR_FPGA_REG_FFT_CFG] = cfg->log2n | cfg->frames_per_line << 8,
		[ESDR_FPGA_REG_L_OFFSET - ESDR_FPGA_REG_FFT_CFG] = (uint16_t)sat16(l_offset),
		[ESDR_FPGA_REG_AVG_C_HI - ESDR_FPGA_REG_FFT_CFG] =
			(uint16_t)sat16(256.0f * log2f(cfg->alpha)),
		[ESDR_FPGA_REG_AVG_C_LO - ESDR_FPGA_REG_FFT_CFG] =
			avg ? (uint16_t)sat16(256.0f * log2f(1.0f - cfg->alpha)) : (uint16_t)INT16_MIN,
		[ESDR_FPGA_REG_COLS - ESDR_FPGA_REG_FFT_CFG] = cfg->cols,
		[ESDR_FPGA_REG_ZOOM_START - ESDR_FPGA_REG_FFT_CFG] = cfg->zoom_start,
		[ESDR_FPGA_REG_ZOOM_STEP - ESDR_FPGA_REG_FFT_CFG] =
			cfg->zoom_step ? cfg->zoom_step : (uint16_t)((n << 8) / cfg->cols),
		[ESDR_FPGA_REG_WF_LO - ESDR_FPGA_REG_FFT_CFG] =
			(uint16_t)esdr_fpga_db_to_l(cfg->wf_min_db),
		[ESDR_FPGA_REG_WF_GAIN - ESDR_FPGA_REG_FFT_CFG] =
			(uint16_t)MIN(lroundf(WF_TOP * 256.0f / wf_span), 32767),
		[ESDR_FPGA_REG_TR_TOP - ESDR_FPGA_REG_FFT_CFG] =
			(uint16_t)esdr_fpga_db_to_l(cfg->tr_top_db),
		[ESDR_FPGA_REG_TR_GAIN - ESDR_FPGA_REG_FFT_CFG] =
			(uint16_t)MIN(lroundf((cfg->tr_rows - 1) * 256.0f / tr_span), 32767),
	};

	BUILD_ASSERT(ESDR_FPGA_REG_TR_GAIN - ESDR_FPGA_REG_FFT_CFG == 11,
		     "the DSP registers are consecutive");
	ret = esdr_fpga_reg_write(dev, ESDR_FPGA_REG_FFT_CFG, regs, ARRAY_SIZE(regs));
	if (ret) {
		return ret;
	}
	return esdr_fpga_dsp_run(dev, false, cfg);
}

int esdr_fpga_dsp_run(const struct device *dev, bool on, const struct esdr_fpga_dsp *cfg)
{
	const uint16_t reset = 1;
	uint16_t ctrl = (on ? BIT(0) : 0) | (cfg->alpha < 1.0f ? BIT(1) : 0) |
			(cfg->overlap ? BIT(2) : 0);
	int ret;

	ret = esdr_fpga_reg_write(dev, ESDR_FPGA_REG_AVG_RESET, &reset, 1);
	if (ret == 0) {
		ret = esdr_fpga_reg_write(dev, ESDR_FPGA_REG_DSP_CTRL, &ctrl, 1);
	}
	return ret;
}

int esdr_fpga_samples(const struct device *dev, uint8_t format, bool resync, const void *data,
		      size_t len)
{
	const uint8_t head[2] = {
		ESDR_FPGA_OP_SAMPLES, format | (resync ? ESDR_FPGA_SAMPLES_RESYNC : 0),
	};

	return esdr_fpga_write(dev, head, sizeof(head), data, len);
}

int esdr_fpga_dsp_columns(const struct device *dev, int16_t *l, size_t n)
{
	return esdr_fpga_reg_read(dev, ESDR_FPGA_COLV, (uint16_t *)l, n);
}
