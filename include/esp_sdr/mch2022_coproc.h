/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief What apps/mch2022_coproc adds to the stock firmware of the MCH2022 badge's RP2040.
 *
 * The stock USB interfaces, WebUSB requests and I2C registers stay as they are
 * (https://github.com/badgeteam/mch2022-firmware-rp2040). New: USB interface 6 with its vendor
 * requests, and registers the stock firmware leaves free. Little endian. Tools read the numbers
 * from this file: keep every constant on its own line.
 *
 * A host ends every write to interface 6's bulk OUT with a short packet, a zero-length one after a
 * whole number of packets: without one the last bytes wait in the firmware for more to come.
 */

#ifndef ESP_SDR_MCH2022_COPROC_H_
#define ESP_SDR_MCH2022_COPROC_H_

#include <stdint.h>

/** USB interfaces: CDC ACM 0 and 1 (ESP32), 2 and 3 (FPGA), then the vendor ones. */
#define COPROC_IF_WEBUSB_ESP32 4
#define COPROC_IF_WEBUSB_FPGA  5
#define COPROC_IF_LINK         6

/** The UARTs, in the requests' wValue. */
#define COPROC_UART_ESP32 0
#define COPROC_UART_FPGA  1

/** Vendor requests to interface 6 (bmRequestType 0x41 OUT, 0xC1 IN; wIndex 6). */
enum coproc_req {
	/** IN struct coproc_info. */
	COPROC_REQ_GET_INFO = 0x01,
	/** IN struct coproc_stats. */
	COPROC_REQ_GET_STATS = 0x02,
	/** OUT, no data: the counters to zero. */
	COPROC_REQ_RESET_STATS = 0x03,
	/** OUT uint32_t baud rate of the UART wValue, while interface 6 owns it. */
	COPROC_REQ_SET_BAUD = 0x10,
	/** IN uint32_t baud rate of the UART wValue. */
	COPROC_REQ_GET_BAUD = 0x11,
	/** OUT, no data: reset the ESP32; wValue 0 into its application, 1 into download mode. */
	COPROC_REQ_ESP32_RESET = 0x20,
	/** OUT, no data: the FPGA's CRESET; wValue 0 held, 1 released, 2 a pulse. */
	COPROC_REQ_FPGA_RESET = 0x21,
	/** OUT, no data: reboot into the RP2040 ROM's USB bootloader. */
	COPROC_REQ_BOOTSEL = 0x22,
	/** OUT, no data: reboot into the badge's bootloader (register 0 then reads 0xFF). */
	COPROC_REQ_BOOTLOADER = 0x23,
	/** OUT, no data: UART0 to and from interface 6, instead of its CDC ACM port. */
	COPROC_REQ_LINK_OPEN = 0x30,
	/** OUT, no data: UART0 back. */
	COPROC_REQ_LINK_CLOSE = 0x31,
	/**
	 * OUT, no data: the FPGA's fast line into interface 6 at wValue times 100 kbaud, 0 off. Its UART
	 * pins change hands meanwhile: the RP2040 reads one and holds the other as a ready line.
	 */
	COPROC_REQ_FPGA_FASTLINK = 0x40,
	/** OUT, no data: how DTR and RTS reset the ESP32, wValue enum coproc_reset_mode. */
	COPROC_REQ_RESET_MODE = 0x50,
	/** IN: the next bytes of the firmware's log, as many as there are (at most wLength). */
	COPROC_REQ_GET_LOG = 0x60,
	/** OUT, no data: interface 6's bulk endpoints to wValue enum coproc_test_mode. */
	COPROC_REQ_TEST_MODE = 0x70,
};

/** How DTR and RTS on the first CDC ACM port reset the ESP32. */
enum coproc_reset_mode {
	/** As the stock firmware: sequences, each line ORed with its last state. */
	COPROC_RESET_STOCK = 0,
	/** As the usual circuit: RTS alone holds EN low, DTR alone holds IO0 low. */
	COPROC_RESET_CIRCUIT = 1,
};

/** Interface 6's bulk endpoints in a test. */
enum coproc_test_mode {
	/** Off: UART0 if the link is open, else nothing. */
	COPROC_TEST_OFF = 0,
	/** IN sends a counter of little endian 32-bit words, OUT is dropped. */
	COPROC_TEST_SOURCE = 1,
	/** What comes in on OUT goes back on IN. */
	COPROC_TEST_LOOPBACK = 2,
	/** A fatal error: the crash handling's test (register 18 reads 1 after the reboot). */
	COPROC_TEST_CRASH = 3,
};

#define COPROC_INFO_MAGIC 0x31325043U /* "CP21" */
#define COPROC_API_MAJOR  1
#define COPROC_API_MINOR  0

/** struct coproc_info.features and register COPROC_REG_FEATURES (bit 7 and below). */
#define COPROC_F_LINK     0x0001U
#define COPROC_F_FASTLINK 0x0002U

/** COPROC_REQ_GET_INFO */
struct coproc_info {
	uint32_t magic;
	uint8_t api_major, api_minor;
	uint16_t features;
	/** ROSC status, the watchdog's reason at boot: how this run started. */
	uint32_t rosc_status, reset_reason;
	uint32_t uptime_ms;
	/** enum coproc_reset_mode, enum coproc_test_mode, whether the link is open. */
	uint8_t reset_mode, test_mode, link_open, reserved;
} __attribute__((packed));

/** COPROC_REQ_GET_STATS. Per UART: ESP32, FPGA. */
struct coproc_stats {
	/** Bytes received and sent on the UART. */
	uint64_t uart_rx[2], uart_tx[2];
	/** Bytes lost: the ring overran; the UART's own overrun, framing, parity and break errors. */
	uint32_t ring_overrun[2], uart_overrun[2], uart_framing[2];
	/** Bytes dropped: the first CDC ACM ports' host was not reading. */
	uint32_t stale_dropped[2];
	/** Interface 6: bytes in on OUT, out on IN. */
	uint64_t link_out, link_in;
	/** Resets of the ESP32, and those a set reset lock made application resets. */
	uint32_t esp32_resets, resets_locked;
	/** The FPGA's fast line: bytes, bytes lost when its ring overran, times it was held back. */
	uint64_t fast_rx;
	uint32_t fast_overrun, fast_stalls;
	/** The most bytes its ring has held: near the host's credit, USB or the host set the pace. */
	uint32_t fast_peak;
	/** Polls of the bridge (one a millisecond) that found a byte with a bad stop bit. */
	uint32_t fast_framing;
} __attribute__((packed));

/**
 * The registers beyond the stock map (I2C 0x17). Register 0 stays 0x0C: the stock launcher
 * replaces anything below that and reads registers this firmware does not have above it. These
 * tell this firmware from the stock one, where they read 0.
 */
#define COPROC_REG_ID        100
#define COPROC_REG_API_MAJOR 101
#define COPROC_REG_API_MINOR 102
#define COPROC_REG_FEATURES  103
/** Read and write: enum coproc_reset_mode. */
#define COPROC_REG_RESET_MODE 0xF0
/** Read: bit 0, interface 6 owns UART0. */
#define COPROC_REG_LINK_STATE 0xF1
/** Read: UART0's baud rate over 100, low byte first (3 bytes). */
#define COPROC_REG_UART0_BAUD 0xF2
/** Write COPROC_BOOTSEL_MAGIC: reboot into the ROM's USB bootloader. */
#define COPROC_REG_BOOTSEL 0xF5

/** Register COPROC_REG_ID reads this. */
#define COPROC_ID         0x5AU
#define COPROC_BOOTSEL_MAGIC 0xB0U

#endif /* ESP_SDR_MCH2022_COPROC_H_ */
