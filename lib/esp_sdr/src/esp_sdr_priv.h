/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Library internals shared by esp_sdr.c, esp_sdr_rx.c, esp_sdr_tx.c and the DAC
 * streaming backend: registers, ROM entry points, the shared state and the
 * DAC session.
 */

#ifndef ESP_SDR_PRIV_H_
#define ESP_SDR_PRIV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>

#include <soc/sensitive_reg.h>
#include <soc/soc.h>

/* Undocumented Wi-Fi MAC dump engine; layout from upstream. */
#define DUMP_CTRL_REG     0x60033d5cU
#define DUMP_CTRL_RUN     BIT(31)
#define DUMP_CTRL_TRIGGER BIT(19)
#define DUMP_CTRL_DONE    BIT(18)
#define DUMP_CTRL_16MSPS  BIT(16)
#define DUMP_CTRL_40MSPS  BIT(15)
#define DUMP_CTRL_COUNT   GENMASK(13, 0)
#define DUMP_CONFIG_REG   0x60033d90U
/* Four 6-bit lane selectors on sources 0..3: receive I/Q. */
#define DUMP_CONFIG_IQ    0x000c2040U
#define DUMP_TIMEOUT_US   20000U

/*
 * The dump engine's DAC-side mirror, 8 bytes above DUMP_CTRL_REG. Same RUN/
 * TRIGGER/DONE/rate bit positions as the ADC side; librftest.a's dactrig()
 * fills the bank with a ramp before arming this, which esp_sdr_tx_play() skips
 * so the caller's own samples survive.
 */
#define DAC_TRIG_REG 0x60033d64U
/* Measured: the DAC side runs at 40 MS/s, bit 15 selects 80 MS/s; bits 16 and 27:20 do
 * nothing to the rate (dactrig() writes 27:20).
 */
#define DAC_CTRL_80MSPS BIT(15)

/* The dump engine writes one of four 64 KiB banks, one-hot in MAC_DUMP_USAGE. */
#define DUMP_BANK0_ADDR 0x3fcb0000U
#define DUMP_BANK_SIZE  0x10000U
#define DUMP_BANKS      3U /* bank 3 also holds ROM data */

#define SENTINEL 0xa5a0055aU

/* Housekeeping counters (esp_sdr_debug_get()). */
extern struct esp_sdr_debug sdr_dbg;

/* BBTOP analog baseband: I/Q low-pass capacitor codes in registers 4 and 5. */
#define BBTOP_BLOCK    0x67U
#define BBTOP_HOST     0U
#define BBTOP_LPF_REG  4U
#define BBTOP_LPF_DCAP GENMASK(5, 0)

/* Wi-Fi AGC: forced index 31:24, force 23, largest calibrated index 14:8. */
#define AGC_FORCE_REG      0x6001c02cU
#define AGC_GAIN_MAX       GENMASK(14, 8)
/* Larger maxima on the S3 point at uncalibrated table slots. */
#define AGC_GAIN_MAX_VALID 82U
/* Index upstream passes while the hardware AGC is in charge. */
#define AGC_DEFAULT_INDEX  40U

/* Default calibration channel for direct PLL tuning. */
#define CAL_CHANNEL_MHZ 2412U

/* ROM and PHY library entry points. */
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned int en);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_pbus_xpd_tx_on(unsigned int en);
extern void rom_set_rxclk_en(unsigned int en);
extern void set_chanfreq(unsigned int mhz, unsigned int bw);
extern void set_rf_freq_offset(unsigned int mode, unsigned int mhz, int khz);
extern void stop_tx_tone(unsigned int en);
#if defined(CONFIG_ESP_SDR_RFTEST)
/* librftest.a: force, index, bt (0 selects the Wi-Fi AGC). */
extern void force_rx_gain(unsigned int force, unsigned int index, unsigned int bt);
/*
 * librftest.a: gain memory index, baseband gain code, digital gain. Writes
 * gain memory slot 0 (the one force_txon_mode(1, 0, 0) selects) and turns
 * the vendor's TX power tracking off.
 */
extern void force_tx_gain(unsigned int gain, unsigned int bb, int dig);
/* librftest.a: en sets the front end's force-TX-on bit (0x60006000 bit 1). */
extern void force_txon_mode(unsigned int en, unsigned int mode, unsigned int ofs);
#endif

/*
 * The vendor PHY serializes its analog I2C traffic with this lock (hal
 * regi2c_ctrl.c); every rom_chip_i2c_* sequence of ours takes it too.
 */
void regi2c_enter_critical(void);
void regi2c_exit_critical(void);

/* Reserved by the module linker snippet. */
extern uint32_t __esp_sdr_bank_start[];
/* Second bank, only linked with CONFIG_ESP_SDR_BANK1 (esp_sdr_bank_tx.ld). */
extern uint32_t __esp_sdr_bank1_start[];

/* Radio state, guarded by sdr_lock (esp_sdr.c). */
extern struct k_mutex sdr_lock;
extern bool sdr_ready;
extern uint32_t sdr_freq_mhz;
extern int32_t sdr_fofs_khz;

/* Tune the LO to sdr_freq_mhz (plus offset). */
void sdr_tune(void);
/* Back to the resting RX-ready state after a tuning change; lock held. */
int sdr_retune(void);
/* MAC_DUMP_USAGE one-hot bits of the capture bank. */
int sdr_bank_usage(uint32_t *usage);
/* Settling after a retune (PLL lock), and the TX/RX turnaround (esp_sdr_set_turnaround()). */
#define SDR_RETUNE_SETTLE_US 3000U
extern uint32_t sdr_turn_settle_us;
extern bool sdr_turn_retune;

/*
 * Front end to RX (the resting state) after a retune, back to RX after a
 * transmission, or to TX, at the current gains; lock held.
 */
void sdr_rx_prepare(void);
void sdr_rx_resume(void);
void sdr_tx_prepare(void);

/* Take the receiver lock and switch the front end to TX at the current step. */
int sdr_dac_begin(enum esp_sdr_rate rate);

/*
 * Two DAC banks: 0 is the capture bank (SRAM bank 2), 1 the transmit bank
 * (SRAM bank 1, reserved by esp_sdr_bank_tx.ld). The engine owns the bank it
 * plays, the CPUs the other one: fill that one with plain stores, then
 * sdr_dac_start() it once the previous burst is done.
 */
#define ESP_SDR_DAC_BANKS 2

/* Bank idx and its size in words. */
uint32_t *sdr_dac_buf(int idx, size_t *words);

/* Hand bank idx to the engine and play its first count words; returns at once. */
void sdr_dac_start(int idx, size_t count);

/* Wait for the burst started last; false on timeout. */
bool sdr_dac_wait(void);

/*
 * Loop mode: with the trigger bit held the engine replays the first count
 * words of the selected bank back to back, gapless, at exactly the DAC rate
 * (stores to other banks do not slow it), until sdr_dac_halt(). It reads
 * whichever bank is selected at the moment, so sdr_dac_select() switches
 * mid-pass at the current offset. Call with interrupts locked to timestamp
 * the start: sample s plays s DAC periods after the call.
 */
void sdr_dac_loop(int idx, size_t count);

/* Hand bank idx to the engine (the CPUs keep every other one). */
void sdr_dac_select(int idx);

/* Stop the engine at once. */
void sdr_dac_halt(void);

/* Back to RX and release the lock. */
void sdr_dac_end(void);

#endif /* ESP_SDR_PRIV_H_ */
