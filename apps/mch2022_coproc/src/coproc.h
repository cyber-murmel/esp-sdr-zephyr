/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the parts of apps/mch2022_coproc share.
 */

#ifndef COPROC_H_
#define COPROC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/net_buf.h>

#include <esp_sdr/mch2022_coproc.h>

#define PORT_ESP32 COPROC_UART_ESP32
#define PORT_FPGA  COPROC_UART_FPGA
#define PORT_COUNT 2

extern struct coproc_stats coproc_stats;

/* ---- the UARTs (uart_dma.c): rings on DMA ---- */

int uart_dma_init(void);
/* The received bytes that lie together; 0 if none. Overruns are counted and skipped here. */
size_t uart_rx_peek(int port, const uint8_t **data);
void uart_rx_consume(int port, size_t n);
/* As many of @p n bytes as fit; they go out by DMA. Any thread. */
size_t uart_tx_write(int port, const uint8_t *data, size_t n);
size_t uart_tx_room(int port);
/* Nothing queued, and the UART has sent its last bit. */
bool uart_tx_drained(int port);
int uart_set_line(int port, const struct uart_config *cfg);
/* The UART's errors into the counters. */
void uart_poll_errors(int port);

/* ---- routing (bridge.c) ---- */

int bridge_start(void);
/* Something for the bridge thread to do. */
void bridge_wake(void);
/* A CDC ACM port's line coding changed. */
void bridge_cdc_line(int port, const struct uart_config *cfg);
/* Whether DTR and RTS on the ESP32's CDC ACM port count. */
bool bridge_cdc_owns_esp32(void);
/* The WebUSB interfaces' requests: their status (bit 0: they own the UART), their baud rate. */
void bridge_webusb_status(int port, uint16_t status);
void bridge_webusb_baud(int port, uint32_t baud);
/* The FPGA's UART looped back, as the ESP32's self test asks. */
void bridge_loopback(bool on);
/* Interface 6. */
void bridge_link_open(bool open);
bool bridge_link_is_open(void);
int bridge_link_baud(int port, uint32_t baud);
uint32_t bridge_baud(int port);
void bridge_set_test(uint8_t mode);
uint8_t bridge_get_test(void);
/* USB configured or not. */
void bridge_usb_up(bool up);

/* ---- USB functions (usb_vendor.c): bulk pipes ---- */

struct vendor_if;
extern struct vendor_if *const vendor_cdc[PORT_COUNT];
extern struct vendor_if *const vendor_webusb[PORT_COUNT];
extern struct vendor_if *const vendor_link;

/* The classes' names, in the order of their interfaces. */
#define VENDOR_CLASSES 5
extern const char *const vendor_class_names[VENDOR_CLASSES];

bool vendor_enabled(struct vendor_if *v);
int vendor_in_flight(struct vendor_if *v);
/* A buffer to fill, or NULL if all are out; its room in *room. */
struct net_buf *vendor_in_get(struct vendor_if *v, size_t *room);
/* It goes out; @p more: more follows at once, so a whole number of packets needs no end mark. */
void vendor_in_put(struct vendor_if *v, struct net_buf *buf, bool more);
/* The next buffer the host sent, or NULL; back with vendor_out_done(). */
struct net_buf *vendor_out_get(struct vendor_if *v);
void vendor_out_done(struct vendor_if *v, struct net_buf *buf);

/* ---- USB (usb_dev.c) ---- */

int usb_dev_init(void);
/* For the BOS descriptors' and classes' control data. */
struct usbd_context;
extern struct usbd_context *const coproc_usbd;

/* ---- the ESP32's and FPGA's lines (esp32_ctl.c) ---- */

int esp32_ctl_init(void);
void esp32_reset(bool download);
/* A new state of DTR and RTS on the ESP32's CDC ACM port. */
void esp32_line_state(bool dtr, bool rts);
void esp32_set_reset_mode(uint8_t mode);
uint8_t esp32_get_reset_mode(void);
/* CRESET: the FPGA held in reset or let go. */
void fpga_run(bool run);
bool fpga_cdone(void);

/* ---- the I2C registers (i2c_regs.c) ---- */

int regs_init(void);
/* The target on the bus. */
int regs_start(void);
uint8_t regs_get(uint8_t reg);
void regs_set(uint8_t reg, uint8_t val);
/* A new reading of an input register (regmap_update_input()). */
void regs_input(uint8_t reg, uint8_t value, uint8_t ignore);
/* The WebUSB interface's mode (register 17), the USB state (register 15). */
void regs_webusb_mode(uint8_t mode);
uint8_t regs_webusb_mode_get(void);
void regs_usb_state(bool mounted, bool suspended, bool remote_wakeup);

/* ---- buttons, ADC, backlight (periph.c, qspi_select.c) ---- */

int periph_init(void);
void backlight_set(uint8_t value);
/* SAO and test point pins: registers 1 and 3. */
void gpio_regs_apply(uint8_t dir, uint8_t out);
/* SELECT, which is the flash's chip select: blocks every interrupt some 30 us. */
bool select_pressed(void);

/* ---- PIO (pio_out.c) ---- */

int pio_out_init(void);
/* 0: off, 1: RGB, 2: RGBW. */
void ws2812_mode(uint8_t mode);
void ws2812_put(uint32_t data);
void ir_send(uint16_t address, uint16_t data);

/* ---- system (sysctl.c, fwlog.c) ---- */

/* How this run began. */
bool sysctl_crashed(void);
uint32_t sysctl_reset_reason(void);
uint32_t sysctl_rosc_status(void);
/* Into the badge's bootloader (0xBE to register 16), the ROM's USB bootloader, soon. */
void sysctl_reboot_bootloader(void);
void sysctl_reboot_bootsel(void);
/* Log bytes not yet read, at most @p max. */
size_t fwlog_read(uint8_t *out, size_t max);

#endif /* COPROC_H_ */
