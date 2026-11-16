/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32-S31 receiver, ported from esp-sdr at 9b61e7465: main/targets/esp32s31/burst/receiver.c,
 * streaming/receiver.c and tuning.h, main/common/rx_recalibration.c, rx_pll_cal.h and
 * burst_gain_table.c. The PHY offsets and calling conventions belong to esp-phy-lib b88e4b76e090
 * (zephyr/module.yml), the one upstream qualified them on.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_private/esp_clk.h>
#include <modem/modem_widgets_reg.h>

#include <esp_sdr/esp_sdr_tx.h>

#if defined(CONFIG_SMP)
#include <esp_mp_stall.h>
#endif

#include "esp_sdr_priv.h"

/* phy_param: the PHY's calibration state word. */
#define PHY_PARAM_STATE    164U
#define PHY_STATE_TABLE    0x200U
#define PHY_STATE_DC_TABLE 0x280U

/* The gain memory: 80 low-table words, 80 high-table words. */
#define GAIN_WORDS 160U
#define GAIN_LOW   80U

/*
 * The dump's fixed gate and clocks. Values from upstream, meaning unknown beyond its names: the
 * MAC's dump source off, and the block's clock gates forced open.
 */
#define DUMP_SRC_GATE_REG   0x201008ccU
#define DUMP_SRC_GATE_M     0x00780000U
#define DUMP_SRC_SEL_REG    0x201070b8U
#define DUMP_SRC_SEL_M      0x7U
#define DUMP_CLK_EN_REG     0x20100800U
#define DUMP_CLK_EN_BIT     BIT(2)
#define DUMP_CLK_FORCE0_REG 0x20109c04U
#define DUMP_CLK_FORCE1_REG 0x20109c08U
#define DUMP_CLK_DIV_REG    0x20109c0cU
#define DUMP_CLK_DIV_M      0xff00f000U
#define DUMP_CLK_DIV_VAL    0x44004000U
#define DUMP_CLK_UPD_REG    0x20109c10U
#define DUMP_CLK_FORCE2_REG 0x20109c14U
#define DUMP_CLK_FORCE2_M   0xe400U

/*
 * Without this bit the receiver gives only the ADC's own noise, whatever the gain. librftest's
 * mac_init() sets it, among much the receive path does not need; the Wi-Fi driver's start must
 * too, as upstream's firmware never writes it.
 */
#define MAC_RX_REG 0x20104ddcU
#define MAC_RX_EN  BIT(4)

/*
 * The aperture: the engine runs in its circular mode even for a burst, its finite mode writes at
 * TCM offset 0, over the image. A burst stops before the first wrap. The first samples are the
 * engine settling; the guard words above the aperture stay sentinels as long as it wrapped there.
 */
#define DUMP_CAPACITY    26624U
#define DUMP_GUARD_WORDS 4U
BUILD_ASSERT(DUMP_FIRST_WORD + ESP_SDR_SAMPLES_MAX < DUMP_CAPACITY, "burst past the aperture");
BUILD_ASSERT((DUMP_CAPACITY + DUMP_GUARD_WORDS) * 4U <= 0x1A010U, "aperture past the bank");

/* A burst takes at most (16380 + 256) / 4 MS/s, 4.2 ms: this is 9.4 ms at 320 MHz. */
#define DUMP_CYCLES_MAX 3000000U

static unsigned int calibrated_mhz;
/* The PLL frequency a table rebuild measures DC at; the PHY's own channels otherwise. */
static unsigned int measurement_mhz;

/* The gain index the AGC falls back to on a quiet band. */
#define AGC_IDLE_INDEX 60U
static uint32_t gain_init, gain_threshold;
static bool gain_saved;
static bool gain_agc = true;
static unsigned int gain_code;

static uint32_t gain_words[GAIN_WORDS][3];
static bool gain_valid[GAIN_WORDS];
static int gain_mirrored = -1;
/* The low-table index a DC correction overwrote. */
static int gain_corrected = -1;

static uint32_t dump_reset_ctrl;
static bool dump_ready;

/* The RF PLL's capacitor (block 0x62), for the low range the vendor calibration does not finish. */
#define RFPLL_BLOCK  0x62U
#define RFPLL_HOST   1U
#define RFPLL_CAP    1U
#define RFPLL_CAP_HI 2U
#define RFPLL_CAP_HI_BIT 0x40U
#define RFPLL_MANUAL 11U
#define RFPLL_MANUAL_BIT 0x40U
#define RFPLL_STATUS 12U
#define RFPLL_CAP_CODES 512U
/* Comparator: 0 in the lock window, 1 more capacitance, 2 less. */
#define PLL_IN_WINDOW 0U
#define PLL_MORE      1U
#define PLL_BROKEN    3U
#define PLL_RECOVER_BELOW_MHZ 2400U
#define PLL_SETTLE_US 50U

static unsigned int pll_read(unsigned int reg)
{
	unsigned int v;

	regi2c_enter_critical();
	v = rom_chip_i2c_readReg(RFPLL_BLOCK, RFPLL_HOST, reg);
	regi2c_exit_critical();
	return v;
}

static void pll_write(unsigned int reg, unsigned int v)
{
	regi2c_enter_critical();
	rom_chip_i2c_writeReg(RFPLL_BLOCK, RFPLL_HOST, reg, v);
	regi2c_exit_critical();
}

static unsigned int pll_status(void)
{
	return (pll_read(RFPLL_STATUS) >> 2) & 3U;
}

static unsigned int pll_probe(unsigned int cap, unsigned int reg2)
{
	pll_write(RFPLL_CAP, cap & 255U);
	pll_write(RFPLL_CAP_HI, (reg2 & ~RFPLL_CAP_HI_BIT) | ((cap >> 2) & RFPLL_CAP_HI_BIT));
	k_busy_wait(PLL_SETTLE_US);
	return pll_status();
}

/*
 * Below the Wi-Fi band the vendor calibration can stop short of the required code: find both
 * edges of the lock window with the PLL's own comparator and take the middle, for drift margin.
 * The frequency stays as programmed.
 */
static bool pll_recover(unsigned int mhz)
{
	unsigned int reg1, reg2, reg11, low, high, first;

	if (mhz >= PLL_RECOVER_BELOW_MHZ || pll_status() == PLL_IN_WINDOW) {
		return true;
	}
	reg1 = pll_read(RFPLL_CAP);
	reg2 = pll_read(RFPLL_CAP_HI);
	reg11 = pll_read(RFPLL_MANUAL);
	pll_write(RFPLL_MANUAL, reg11 | RFPLL_MANUAL_BIT);

	low = 0U;
	high = RFPLL_CAP_CODES;
	while (low < high) {
		unsigned int mid = low + (high - low) / 2U;
		unsigned int status = pll_probe(mid, reg2);

		if (status == PLL_BROKEN) {
			goto restore;
		}
		if (status == PLL_MORE) {
			low = mid + 1U;
		} else {
			high = mid;
		}
	}
	if (low == RFPLL_CAP_CODES || pll_probe(low, reg2) != PLL_IN_WINDOW) {
		goto restore;
	}
	first = low;
	high = RFPLL_CAP_CODES;
	while (low < high) {
		unsigned int mid = low + (high - low) / 2U;
		unsigned int status = pll_probe(mid, reg2);

		if (status == PLL_MORE || status == PLL_BROKEN) {
			goto restore;
		}
		if (status == PLL_IN_WINDOW) {
			low = mid + 1U;
		} else {
			high = mid;
		}
	}
	if (pll_probe(first + (low - first - 1U) / 2U, reg2) != PLL_IN_WINDOW) {
		goto restore;
	}
	for (unsigned int i = 0; i < 3U; i++) {
		k_busy_wait(PLL_SETTLE_US);
		if (pll_status() != PLL_IN_WINDOW) {
			goto restore;
		}
	}
	return true;

restore:
	pll_write(RFPLL_CAP, reg1);
	pll_write(RFPLL_CAP_HI, reg2);
	pll_write(RFPLL_MANUAL, reg11);
	return false;
}

/* The PHY's channel setup programs this; a table rebuild tunes to fixed channels through it. */
extern void __real_phy_set_channel_rfpll_freq(unsigned int mhz, unsigned int crystal,
					      unsigned int mode);
void __wrap_phy_set_channel_rfpll_freq(unsigned int mhz, unsigned int crystal, unsigned int mode)
{
	__real_phy_set_channel_rfpll_freq(measurement_mhz != 0U ? measurement_mhz : mhz, crystal,
					  mode);
	if (measurement_mhz != 0U) {
		(void)pll_recover(measurement_mhz);
	}
}

/*
 * Fresh receive DC at this LO and the gain tables rebuilt from it, keeping the startup IQ
 * correction: its loopback calibration emits a tone through the antenna.
 */
static void rx_recalibrate(unsigned int mhz)
{
	volatile uint32_t *state = (volatile uint32_t *)(phy_param + PHY_PARAM_STATE);

	measurement_mhz = mhz;
	REG_CLR_BIT(AGC_FORCE_REG, AGC_FORCE_EN);
	*state &= ~PHY_STATE_DC_TABLE;
	phy_set_rx_gain_table(mhz, 0);
	measurement_mhz = 0U;
}

/*
 * The AGC reads its gain words from the high table. A forced index only reads as itself when the
 * low-table word is mirrored there, so keep what the calibration wrote, to put it back.
 */
extern void __real_phy_write_gain_mem(uint32_t a, uint32_t b, uint32_t c, uint32_t index);
void __wrap_phy_write_gain_mem(uint32_t a, uint32_t b, uint32_t c, uint32_t index)
{
	if (index < GAIN_WORDS) {
		gain_words[index][0] = a;
		gain_words[index][1] = b;
		gain_words[index][2] = c;
		gain_valid[index] = true;
	}
	__real_phy_write_gain_mem(a, b, c, index);
}

static void gain_mirror(int index)
{
	if (gain_corrected >= 0) {
		unsigned int low = (unsigned int)gain_corrected;

		__real_phy_write_gain_mem(gain_words[low][0], gain_words[low][1],
					  gain_words[low][2], low);
		gain_corrected = -1;
	}
	if (gain_mirrored >= 0) {
		unsigned int high = (unsigned int)gain_mirrored + GAIN_LOW;

		if (gain_valid[high]) {
			__real_phy_write_gain_mem(gain_words[high][0], gain_words[high][1],
						  gain_words[high][2], high);
		}
		gain_mirrored = -1;
	}
	if (index >= 0 && index < (int)GAIN_LOW && gain_valid[index]) {
		__real_phy_write_gain_mem(gain_words[index][0], gain_words[index][1],
					  gain_words[index][2], (uint32_t)index + GAIN_LOW);
		gain_mirrored = index;
	}
}

static void front_end_rx(void)
{
	phy_pbus_workmode();
	phy_pbus_xpd_tx_off();
	phy_pbus_xpd_rx_on(1);
	phy_set_rxclk_en(1);
}

/* Work mode releases a forced gain: force it again. */
void esp_sdr_s31_rx_on(void)
{
	front_end_rx();
	phy_rfrx_sat_rst(gain_agc ? 1U : 0U);
	phy_force_rx_gain(gain_agc ? 0U : 1U, gain_agc ? 0U : gain_code);
}

/* Transmitting: a capture or a stop takes the radio back to receive. */
static bool tx_tone_on;
/* The paced stream's chain (esp_sdr_tx_chain_on()): the tone and the stream exclude each other. */
static bool tx_chain_on;

/* What the S31 tunes, and so transmits at. */
#define TX_MIN_MHZ 2200U
#define TX_MAX_MHZ 2900U

int esp_sdr_tx_tone_start(int offset_khz, unsigned int att)
{
	if (att > ESP_SDR_TX_TONE_ATT_MAX) {
		return -EINVAL;
	}
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	if (tx_chain_on) {
		k_mutex_unlock(&esp_sdr_lock);
		return -EBUSY;
	}
	phy_txcal_debuge_mode_();
	phy_start_tx_tone_step_new(1, offset_khz, (int)att, 0, 0, 0);
	tx_tone_on = true;
	k_mutex_unlock(&esp_sdr_lock);
	return 0;
}

void esp_sdr_tx_tone_stop(void)
{
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	if (tx_tone_on) {
		phy_start_tx_tone_step_new(0, 0, 0, 0, 0, 0);
		phy_txcal_work_mode();
		tx_tone_on = false;
	}
	k_mutex_unlock(&esp_sdr_lock);
}

/*
 * The paced stream's chain (esp_sdr_paced.c), lock held: the tone path turns the transmit chain
 * on at the LO as tuned, then phy_stop_tx_tone_new() stops the tone and hands the DAC to the TX DC
 * value. No retune: the tone path keeps the LO. The power step does nothing here; the level
 * follows the samples alone (doc/hardware-quirks.md).
 */
int esp_sdr_tx_chain_on(unsigned int power)
{
	ARG_UNUSED(power);
	if (tx_tone_on) {
		return -EBUSY;
	}
	if (esp_sdr_freq_mhz < TX_MIN_MHZ || esp_sdr_freq_mhz > TX_MAX_MHZ) {
		return -ERANGE;
	}
	phy_txcal_debuge_mode_();
	phy_start_tx_tone_step_new(1, 0, ESP_SDR_TX_TONE_ATT_MAX, 0, 0, 0);
	phy_stop_tx_tone_new();
	tx_chain_on = true;
	return 0;
}

void esp_sdr_tx_chain_off(void)
{
	phy_start_tx_tone_step_new(0, 0, 0, 0, 0, 0);
	phy_txcal_work_mode();
	esp_sdr_s31_rx_on();
	tx_chain_on = false;
}

void esp_sdr_s31_rx_mute(bool mute)
{
	if (mute) {
		REG_CLR_BIT(MAC_RX_REG, MAC_RX_EN);
	} else {
		REG_SET_BIT(MAC_RX_REG, MAC_RX_EN);
	}
}

int esp_sdr_s31_gain_get(void)
{
	return gain_agc ? -1 : (int)gain_code;
}

void esp_sdr_s31_gain(int index)
{
	gain_agc = index < 0;
	gain_code = gain_agc ? 0U : (unsigned int)index;

	phy_pbus_debugmode();
	phy_pbus_xpd_rx_on(0);
	phy_set_rxclk_en(1);
	phy_set_txclk_en(1);
	gain_mirror(index);
	phy_set_txclk_en(0);
	front_end_rx();
	phy_rfrx_sat_rst(gain_agc ? 1U : 0U);
	if (gain_saved) {
		REG_WRITE(AGC_INIT_REG, gain_agc ? gain_init : ((gain_init & ~AGC_INIT_INDEX) |
								FIELD_PREP(AGC_INIT_INDEX, gain_code)));
		REG_WRITE(AGC_THRESH_REG, gain_agc ? gain_threshold :
					  ((gain_threshold & ~AGC_THRESH_INDEX) |
					   FIELD_PREP(AGC_THRESH_INDEX, gain_code)));
	}
	phy_force_rx_gain(gain_agc ? 0U : 1U, gain_code);
}

/* The group's owner must be the engine, and interrupts off, while the clocks and lanes change. */
static ALWAYS_INLINE void dump_regs(void)
{
	REG_CLR_BIT(DUMP_SRC_GATE_REG, DUMP_SRC_GATE_M);
	REG_WRITE(DUMP_SRC_SEL_REG, (REG_READ(DUMP_SRC_SEL_REG) & ~DUMP_SRC_SEL_M) | 1U);
	REG_SET_BIT(DUMP_CLK_EN_REG, DUMP_CLK_EN_BIT);
	REG_WRITE(DUMP_CLK_FORCE0_REG, UINT32_MAX);
	REG_CLR_BIT(DUMP_MAC_REG, BIT(31));
	REG_SET_BIT(HP_SYSTEM_TCM_RAM_PWR_CTRL0_REG, HP_SYSTEM_REG_HP_SYSTEM_TCM_CLK_FORCE_ON);
	REG_SET_BIT(DUMP_CLK_FORCE1_REG, BIT(31));
	REG_SET_BIT(DUMP_CLK_FORCE2_REG, DUMP_CLK_FORCE2_M);
	REG_WRITE(DUMP_CLK_DIV_REG,
		  (REG_READ(DUMP_CLK_DIV_REG) & ~DUMP_CLK_DIV_M) | DUMP_CLK_DIV_VAL);
	REG_SET_BIT(DUMP_CLK_UPD_REG, BIT(31));
	REG_CLR_BIT(DUMP_CLK_UPD_REG, BIT(31));
	/* Bits this keeps read 0x03c00000 here: cleared, the writer stalls. */
	dump_reset_ctrl = REG_READ(DUMP_CTRL_REG) & DUMP_CTRL_KEEP;
	REG_WRITE(DUMP_MODE_REG, (REG_READ(DUMP_MODE_REG) & ~(DUMP_MODE_SOURCE | DUMP_MODE_RATE)) |
					 FIELD_PREP(DUMP_MODE_SOURCE, DUMP_SOURCE_IQ));
	REG_WRITE(DUMP_CONFIG_REG, (REG_READ(DUMP_CONFIG_REG) & ~DUMP_CONFIG_LANES) | DUMP_CONFIG_IQ);
}

/*
 * While the engine owns the upper group, nothing may touch it, the ROM data in it included: this
 * core runs with interrupts off and, with two, the other waits in its stall interrupt, which spins
 * in IRAM. The pause comes first, interrupts still open: the other core may hold it and have to
 * park this one before it lets go.
 */
static ALWAYS_INLINE unsigned int group_take(void)
{
#if defined(CONFIG_SMP)
	soc_mp_pause_others();
#endif
	return irq_lock();
}

static ALWAYS_INLINE void group_give(unsigned int key)
{
	irq_unlock(key);
#if defined(CONFIG_SMP)
	soc_mp_resume_others();
#endif
}

/*
 * The engine's clocks and lanes, once. Nothing here may touch ROM data, the PHY or the scheduler:
 * their memory is the engine's meanwhile.
 */
static IRAM_ATTR __noinline void dump_setup(void)
{
	unsigned int key = group_take();

	REG_WRITE(DUMP_USAGE_REG, DUMP_USAGE_OWN);
	__asm__ volatile("fence" ::: "memory");
	dump_regs();
	REG_WRITE(DUMP_CTRL_REG,
		  dump_reset_ctrl | DUMP_CAPACITY | DUMP_CTRL_CIRCULAR | DUMP_CTRL_DONE);
	REG_WRITE(DUMP_CTRL_REG, dump_reset_ctrl | DUMP_CAPACITY | DUMP_CTRL_CIRCULAR);
	REG_WRITE(DUMP_USAGE_REG, 0U);
	__asm__ volatile("fence" ::: "memory");
	group_give(key);
}

#if defined(CONFIG_ESP_SDR_DIAG16)
/* The ring of the circular mode while the bus runs: nothing reads it, the group goes back. */
#define DIAG_RING_WORDS    16383U
#define DIAG_EXCHANGE      0x32f2U
#define DIAG_FIX_SEL_IQ    (MODEM_WIDGETS_DIAG_BUS_FIX_LOW_EN | (22U << 5))
#define DIAG_PROBE_REG     0x20107c04U
#define DIAG_PROBE_CLEAR   (0xffU | BIT(30))
#define DIAG_LATCH_SPINS   64U

/* The engine running with its bank handed back: the bus still carries its word. */
IRAM_ATTR void esp_sdr_s31_diag_start(void)
{
	unsigned int key = group_take();
	uint32_t ctrl;

	REG_WRITE(DUMP_USAGE_REG, DUMP_USAGE_OWN);
	__asm__ volatile("fence" ::: "memory");
	dump_regs();
	ctrl = dump_reset_ctrl | DIAG_RING_WORDS | DUMP_CTRL_CIRCULAR;
	REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_DONE);
	REG_WRITE(DUMP_CTRL_REG, ctrl);
	REG_SET_BIT(HP_SYSTEM_CLK_EN_REG, HP_SYSTEM_REG_CLK_EN);
	REG_SET_BIT(HP_SYSTEM_PROBEA_CTRL_REG, HP_SYSTEM_REG_PROBE_GLOBAL_EN);
	REG_SET_BIT(MODEM_WIDGETS_CLK_CONF_REG, MODEM_WIDGETS_CLK_EN);
	REG_WRITE(MODEM_WIDGETS_MODEM_DIAG_EXCHANGE_REG, DIAG_EXCHANGE);
	REG_WRITE(MODEM_WIDGETS_MODEM_DIAG_FIX_SEL_REG, DIAG_FIX_SEL_IQ);
	REG_WRITE(DIAG_PROBE_REG, REG_READ(DIAG_PROBE_REG) & ~DIAG_PROBE_CLEAR);
	REG_WRITE(HP_SYSTEM_MODEM_DIAG_EN_REG, UINT32_MAX);
	REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_RUN);
	for (volatile unsigned int j = 0; j < DIAG_LATCH_SPINS; j++) {
		__asm__ volatile("nop");
	}
	REG_WRITE(DUMP_USAGE_REG, 0U);
	__asm__ volatile("fence" ::: "memory");
	group_give(key);
}

void esp_sdr_s31_diag_stop(void)
{
	REG_WRITE(HP_SYSTEM_MODEM_DIAG_EN_REG, 0U);
	REG_CLR_BIT(DUMP_CTRL_REG, DUMP_CTRL_RUN);
}
#endif /* CONFIG_ESP_SDR_DIAG16 */

void esp_sdr_s31_tune(unsigned int mhz, int khz, unsigned int cbw)
{
	bool channel = (mhz >= 2412U && mhz <= 2472U && (mhz - 2412U) % 5U == 0U) || mhz == 2484U;
	volatile uint32_t *state = (volatile uint32_t *)(phy_param + PHY_PARAM_STATE);

	REG_SET_BIT(MAC_RX_REG, MAC_RX_EN);

	/* Manual overrides off before the PHY sets the channel up. */
	phy_set_txclk_en(1);
	gain_mirror(-1);
	phy_set_txclk_en(0);
	if (gain_saved) {
		REG_WRITE(AGC_INIT_REG, gain_init);
		REG_WRITE(AGC_THRESH_REG, gain_threshold);
	}
	if (calibrated_mhz != mhz) {
		rx_recalibrate(mhz);
		calibrated_mhz = mhz;
	}
	/* Out of band requests stay out of the channel calibration and its index. */
	phy_chip_set_chan(channel ? mhz : CAL_CHANNEL_MHZ, cbw);
	phy_set_freq(mhz, khz);
	(void)pll_recover(mhz);
	phy_loopback_mode_en(0);
	phy_bb_bss_cbw40_dig(0);
	phy_bb_cbw_chan_cfg(0);
	phy_wifi_fbw_sel(0);
	front_end_rx();
	phy_pbus_debugmode();
	if (!dump_ready) {
		dump_setup();
		dump_ready = true;
		front_end_rx();
	}
	*state &= ~PHY_STATE_TABLE;
	phy_set_rx_gain_table(mhz, 0);
	gain_init = REG_READ(AGC_INIT_REG);
	gain_threshold = REG_READ(AGC_THRESH_REG);
	/*
	 * Where the AGC idles with nothing on the air. The PHY's choice, the table's top, is noise at
	 * -22 dBFS with the DC to match; this is about -34, near the other chips' AGC.
	 */
	gain_threshold = (gain_threshold & ~AGC_THRESH_INDEX) |
			 FIELD_PREP(AGC_THRESH_INDEX,
				    MIN(FIELD_GET(AGC_THRESH_INDEX, gain_threshold), AGC_IDLE_INDEX));
	gain_saved = true;
	if (gain_agc) {
		REG_WRITE(AGC_THRESH_REG, gain_threshold);
	}
}

/*
 * The gate is open for the whole burst, some 4 ms at 4 MS/s: the upper group, ROM data in it, is
 * the engine's. Hence interrupts off, code and stack below the group, and no calls.
 */
static IRAM_ATTR __noinline bool dump_run(size_t words, unsigned int rate_field, uint32_t *cycles,
					 uint32_t *end)
{
	uint32_t ctrl = dump_reset_ctrl | DUMP_CAPACITY | DUMP_CTRL_CIRCULAR;
	uint32_t at = 0U;
	unsigned int key = group_take();
	uint32_t start;

	REG_WRITE(DUMP_USAGE_REG, DUMP_USAGE_OWN);
	__asm__ volatile("fence" ::: "memory");
	REG_WRITE(DUMP_MODE_REG, (REG_READ(DUMP_MODE_REG) & ~(DUMP_MODE_SOURCE | DUMP_MODE_RATE)) |
					 FIELD_PREP(DUMP_MODE_RATE, rate_field) |
					 FIELD_PREP(DUMP_MODE_SOURCE, DUMP_SOURCE_IQ));
	REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_DONE);
	REG_WRITE(DUMP_CTRL_REG, ctrl);
	start = (uint32_t)esp_cpu_get_cycle_count();
	REG_SET_BIT(DUMP_CTRL_REG, DUMP_CTRL_RUN);
	REG_SET_BIT(DUMP_CTRL_REG, DUMP_CTRL_TRIGGER);
	REG_CLR_BIT(DUMP_CTRL_REG, DUMP_CTRL_TRIGGER);
	while (at < words && (uint32_t)esp_cpu_get_cycle_count() - start < DUMP_CYCLES_MAX) {
		at = REG_READ(DUMP_MODE_REG) & DUMP_MODE_INDEX;
	}
	*cycles = (uint32_t)esp_cpu_get_cycle_count() - start;
	*end = at;
	REG_CLR_BIT(DUMP_CTRL_REG, DUMP_CTRL_RUN);
	REG_WRITE(DUMP_USAGE_REG, 0U);
	__asm__ volatile("fence" ::: "memory");
	group_give(key);
	return at >= words && at < DUMP_CAPACITY;
}

int esp_sdr_s31_capture(enum esp_sdr_rate rate, size_t count, uint32_t *elapsed_us)
{
	/* Measured by upstream from capture lengths: 80, 40, 20, 10, 8, 4 MS/s. */
	static const uint8_t rate_field[] = {0, 1, 3, 5, 7, 9};
	volatile uint32_t *bank = __esp_sdr_bank_start;
	size_t last = DUMP_FIRST_WORD + count - 1U;
	uint32_t cycles, end;

	if ((unsigned int)rate >= ARRAY_SIZE(rate_field)) {
		return -EINVAL;
	}
	if (!dump_ready) {
		return -EAGAIN;
	}
	/* The engine fills the aperture in order: first and last sentinels gone, it all arrived. */
	bank[DUMP_FIRST_WORD] = SENTINEL;
	bank[last] = SENTINEL;
	for (unsigned int j = 0; j < DUMP_GUARD_WORDS; j++) {
		bank[DUMP_CAPACITY + j] = SENTINEL;
	}
	esp_sdr_s31_rx_on();
	if (!dump_run(DUMP_FIRST_WORD + count, rate_field[rate], &cycles, &end)) {
		return -ETIMEDOUT;
	}
	for (unsigned int j = 0; j < DUMP_GUARD_WORDS; j++) {
		if (bank[DUMP_CAPACITY + j] != SENTINEL) {
			return -EOVERFLOW;
		}
	}
	if (bank[DUMP_FIRST_WORD] == SENTINEL || bank[last] == SENTINEL) {
		return -ETIMEDOUT;
	}
	*elapsed_us = (uint32_t)((uint64_t)cycles * USEC_PER_SEC / (uint32_t)esp_clk_cpu_freq());
	return 0;
}

/*
 * The DC servo (upstream streaming/dc.c, in integers: the library stays off the FPU). Means are in
 * 1/256 of an 8-bit step. The two baseband DC DACs live in the gain word; which way each moves I
 * and Q is measured every time, the mapping is no contract.
 */
#define DC_GOAL          16384 /* A cost of 0.25: half an 8-bit step. */
#define DC_NUDGE         16
#define DC_CODE_MAX      511
#define DC_STEP_MAX      32
#define DC_RANGE         96
#define DC_STEPS         5
#define DC_TRIALS        3
#define DC_SLOPE_MAX     (4 * 256)
#define DC_DET_MIN       131 /* 0.002 in step^2 per code^2. */
#define DC_APPLY_MS      10
/* An unused entry, latched before the real one, so the front end reloads its DC DACs. */
#define GAIN_SHADOW      79U

extern void phy_pbus_force_test(unsigned int group, unsigned int mode, unsigned int value);

static bool dc_codes(unsigned int index, int codes[2])
{
	if (index >= GAIN_SHADOW || !gain_valid[index] || gain_mirrored != (int)index) {
		return false;
	}
	codes[0] = (int)(((gain_words[index][1] & 255U) << 1) | (gain_words[index][0] >> 31));
	codes[1] = (int)((gain_words[index][0] >> 13) & 511U);
	return true;
}

static void dc_apply(unsigned int index, const int codes[2])
{
	unsigned int i = (unsigned int)codes[0];
	unsigned int q = (unsigned int)codes[1];
	/* Baseband DC only: RF DC, the gains and the I/Q calibration stay. */
	uint32_t a = (gain_words[index][0] & ~((511U << 13) | BIT(31))) | (q << 13) |
		     ((i & 1U) << 31);
	uint32_t b = (gain_words[index][1] & ~255U) | (i >> 1);
	const unsigned int slots[] = {index, index + GAIN_LOW, GAIN_SHADOW,
				      GAIN_SHADOW + GAIN_LOW};
	uint32_t reg;

	for (unsigned int j = 0; j < ARRAY_SIZE(slots); j++) {
		__real_phy_write_gain_mem(a, b, gain_words[index][2], slots[j]);
	}
	gain_corrected = (int)index;
	phy_pbus_force_test(2, 2, i);
	phy_pbus_force_test(3, 2, q);
	reg = REG_READ(AGC_FORCE_REG) & ~AGC_FORCE_INDEX;
	REG_WRITE(AGC_FORCE_REG, reg | FIELD_PREP(AGC_FORCE_INDEX, GAIN_SHADOW));
	REG_WRITE(AGC_FORCE_REG, reg | FIELD_PREP(AGC_FORCE_INDEX, index));
	k_msleep(DC_APPLY_MS);
}

static int64_t dc_cost(const int32_t m[2])
{
	return (int64_t)m[0] * m[0] + (int64_t)m[1] * m[1];
}

int esp_sdr_s31_dc_calibrate(esp_sdr_s31_dc_measure_t measure, void *user,
			     struct esp_sdr_rx_dc *out)
{
	unsigned int index = gain_code;
	int base[2], best[2];
	int32_t mi[2], mq[2];
	int64_t best_cost;
	int32_t a, b, c, d;
	int64_t det;
	int di, dq;

	*out = (struct esp_sdr_rx_dc){0};
	if (gain_agc || !dc_codes(index, base)) {
		return -ENOTSUP;
	}
	/* The shadow slot is an entry of a table that reaches it: nothing would restore that. */
	if (FIELD_GET(AGC_GAIN_MAX, REG_READ(AGC_FORCE_REG)) >= GAIN_SHADOW) {
		return -ENOTSUP;
	}
	if (!measure(out->before, user)) {
		return -EIO;
	}
	/* A correction from an earlier call is in force, but base holds the PHY's codes. */
	if (gain_corrected == (int)index && dc_cost(out->before) >= DC_GOAL) {
		dc_apply(index, base);
		if (!measure(out->before, user)) {
			return -EIO;
		}
	}
	out->after[0] = out->before[0];
	out->after[1] = out->before[1];
	best_cost = dc_cost(out->before);
	if (best_cost < DC_GOAL) {
		return 0;
	}
	best[0] = base[0];
	best[1] = base[1];

	/* The 2x2 response of I and Q to the two DACs. */
	di = base[0] > DC_CODE_MAX - DC_NUDGE ? -DC_NUDGE : DC_NUDGE;
	dq = base[1] > DC_CODE_MAX - DC_NUDGE ? -DC_NUDGE : DC_NUDGE;
	dc_apply(index, (int[]){base[0] + di, base[1]});
	if (!measure(mi, user)) {
		dc_apply(index, base);
		return -EIO;
	}
	dc_apply(index, (int[]){base[0], base[1] + dq});
	if (!measure(mq, user)) {
		dc_apply(index, base);
		return -EIO;
	}
	a = (mi[0] - out->before[0]) / di;
	b = (mq[0] - out->before[0]) / dq;
	c = (mi[1] - out->before[1]) / di;
	d = (mq[1] - out->before[1]) / dq;
	det = (int64_t)a * d - (int64_t)b * c;
	if ((det < 0 ? -det : det) < DC_DET_MIN || abs(a) > DC_SLOPE_MAX ||
	    abs(b) > DC_SLOPE_MAX || abs(c) > DC_SLOPE_MAX || abs(d) > DC_SLOPE_MAX) {
		dc_apply(index, base);
		return -ERANGE;
	}

	for (unsigned int step = 0; step < DC_STEPS && best_cost >= DC_GOAL; step++) {
		int32_t want[2] = {
			(int32_t)((-(int64_t)d * out->after[0] + (int64_t)b * out->after[1]) / det),
			(int32_t)(((int64_t)c * out->after[0] - (int64_t)a * out->after[1]) / det),
		};
		bool improved = false;

		for (unsigned int trial = 0; trial < DC_TRIALS; trial++) {
			int next[2];
			int32_t seen[2];

			for (unsigned int axis = 0; axis < 2U; axis++) {
				int32_t move = CLAMP(want[axis], -DC_STEP_MAX, DC_STEP_MAX) >> trial;

				next[axis] = CLAMP(best[axis] + move, 0, DC_CODE_MAX);
				next[axis] = CLAMP(next[axis], base[axis] - DC_RANGE,
						   base[axis] + DC_RANGE);
			}
			if (next[0] == best[0] && next[1] == best[1]) {
				break;
			}
			dc_apply(index, next);
			out->steps++;
			if (measure(seen, user) && dc_cost(seen) < best_cost) {
				best_cost = dc_cost(seen);
				best[0] = next[0];
				best[1] = next[1];
				out->after[0] = seen[0];
				out->after[1] = seen[1];
				improved = true;
				break;
			}
		}
		if (!improved) {
			break;
		}
	}
	/* The last trial may have been worse: latch the best seen. */
	dc_apply(index, best);
	return 0;
}
