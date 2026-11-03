/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Who each UART talks to. As in the stock firmware's uart_task.c the ESP32's UART goes to its CDC
 * ACM port or, once the host says so, to its WebUSB interface; the FPGA's likewise, or back to
 * itself for the ESP32's self test. New: the ESP32's UART to interface 6 (COPROC_REQ_LINK_OPEN).
 * Whoever owns a UART also sets its line.
 *
 * One thread moves all the data, woken by the USB functions' completions and the UARTs' DMA, and
 * every millisecond for what the UARTs bring.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "coproc.h"

LOG_MODULE_REGISTER(bridge, LOG_LEVEL_INF);

#define BRIDGE_STACK 1536
#define BRIDGE_PRIO  5
/* A host that reads nothing for this long does not get what it missed on a CDC ACM port. */
#define STALE_MS     200
#define ERRORS_MS    100
#define LINE_WAIT_MS 10
/* The stock firmware's, for the ESP32's self test. */
#define LOOPBACK_BAUD 1000000U
/* What apps/osmosdr_serial runs at. */
#define LINK_BAUD     2000000U

enum route {
	ROUTE_CDC,
	ROUTE_WEBUSB,
	ROUTE_LINK,
	ROUTE_LOOPBACK,
};

static struct bridge_port {
	enum route route;
	/* The line each owner asked for. */
	struct uart_config cdc_line;
	uint32_t webusb_baud, link_baud;
	/* The line in force. */
	struct uart_config line;
	uint16_t webusb_status;
	/* A buffer from the host, whose it is, how far it went. */
	struct net_buf *out;
	struct vendor_if *out_owner;
	size_t out_off;
	/* The last sign of a host reading its CDC ACM port. */
	int64_t last_read_ms;
} ports[PORT_COUNT];

static K_SEM_DEFINE(wake, 0, 1);
static bool usb_up;
static bool link_open;
static bool loopback;
static uint8_t test_mode;
static uint32_t test_count;

void bridge_wake(void)
{
	k_sem_give(&wake);
}

static enum route route_of(int port)
{
	struct bridge_port *p = &ports[port];

	if (port == PORT_ESP32 && link_open) {
		return ROUTE_LINK;
	}
	if (port == PORT_FPGA && loopback) {
		return ROUTE_LOOPBACK;
	}
	return (p->webusb_status & 1U) != 0U ? ROUTE_WEBUSB : ROUTE_CDC;
}

bool bridge_cdc_owns_esp32(void)
{
	return route_of(PORT_ESP32) == ROUTE_CDC;
}

/* The line the UART's owner wants; 8N1 for all but the CDC ACM port's. */
static void wanted_line(int port, struct uart_config *cfg)
{
	struct bridge_port *p = &ports[port];

	*cfg = p->cdc_line;
	if (route_of(port) != ROUTE_CDC) {
		cfg->parity = UART_CFG_PARITY_NONE;
		cfg->stop_bits = UART_CFG_STOP_BITS_1;
		cfg->data_bits = UART_CFG_DATA_BITS_8;
	}
	/* No such wires. */
	cfg->flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
	switch (route_of(port)) {
	case ROUTE_WEBUSB:
		cfg->baudrate = p->webusb_baud;
		break;
	case ROUTE_LINK:
		cfg->baudrate = p->link_baud;
		break;
	case ROUTE_LOOPBACK:
		cfg->baudrate = LOOPBACK_BAUD;
		break;
	default:
		break;
	}
}

/* Once what is on its way has gone out at the old rate. */
static void line_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	for (int k = 0; k < PORT_COUNT; k++) {
		struct bridge_port *p = &ports[k];
		struct uart_config want;
		int ret;

		wanted_line(k, &want);
		if (memcmp(&want, &p->line, sizeof(want)) == 0) {
			continue;
		}
		for (int t = 0; t < LINE_WAIT_MS && !uart_tx_drained(k); t++) {
			k_msleep(1);
		}
		ret = uart_set_line(k, &want);
		if (ret != 0) {
			/* Mark and space parity, 1.5 stop bits: the line stays. */
			LOG_WRN("UART %d: line refused (%d)", k, ret);
		}
		p->line = want;
		if (k == PORT_ESP32) {
			uint32_t b = want.baudrate / 100U;

			regs_set(COPROC_REG_UART0_BAUD, (uint8_t)b);
			regs_set(COPROC_REG_UART0_BAUD + 1, (uint8_t)(b >> 8));
			regs_set(COPROC_REG_UART0_BAUD + 2, (uint8_t)(b >> 16));
		}
	}
}
static K_WORK_DEFINE(line_work, line_fn);

static void routes_changed(void)
{
	for (int k = 0; k < PORT_COUNT; k++) {
		ports[k].route = route_of(k);
	}
	(void)k_work_submit(&line_work);
	bridge_wake();
}

void bridge_cdc_line(int port, const struct uart_config *cfg)
{
	ports[port].cdc_line = *cfg;
	(void)k_work_submit(&line_work);
}

void bridge_webusb_status(int port, uint16_t status)
{
	ports[port].webusb_status = status;
	routes_changed();
}

void bridge_webusb_baud(int port, uint32_t baud)
{
	ports[port].webusb_baud = baud;
	(void)k_work_submit(&line_work);
}

void bridge_loopback(bool on)
{
	loopback = on;
	routes_changed();
}

void bridge_link_open(bool open)
{
	link_open = open;
	regs_set(COPROC_REG_LINK_STATE, open ? 1U : 0U);
	routes_changed();
}

bool bridge_link_is_open(void)
{
	return link_open;
}

int bridge_link_baud(int port, uint32_t baud)
{
	if (port >= PORT_COUNT || baud == 0U) {
		return -EINVAL;
	}
	ports[port].link_baud = baud;
	(void)k_work_submit(&line_work);
	return 0;
}

uint32_t bridge_baud(int port)
{
	return port < PORT_COUNT ? ports[port].line.baudrate : 0U;
}

void bridge_set_test(uint8_t mode)
{
	test_mode = mode;
	test_count = 0;
	bridge_wake();
}

uint8_t bridge_get_test(void)
{
	return test_mode;
}

void bridge_usb_up(bool up)
{
	usb_up = up;
	bridge_wake();
}

/* ---- the USB functions ---- */

/* The UART's bytes out on @p v's bulk IN, while it has buffers; how many. */
static size_t uart_to_vendor(int port, struct vendor_if *v)
{
	size_t room, total = 0;
	struct net_buf *buf;

	while (true) {
		const uint8_t *data;
		size_t n = uart_rx_peek(port, &data);

		if (n == 0U) {
			return total;
		}
		buf = vendor_in_get(v, &room);
		if (buf == NULL) {
			return total;
		}
		n = MIN(n, room);
		net_buf_add_mem(buf, data, n);
		uart_rx_consume(port, n);
		/* What lies past the ring's end, if the buffer has room for it. */
		if (net_buf_tailroom(buf) > 0U) {
			size_t more = uart_rx_peek(port, &data);

			more = MIN(more, net_buf_tailroom(buf));
			if (more > 0U) {
				net_buf_add_mem(buf, data, more);
				uart_rx_consume(port, more);
			}
		}
		total += buf->len;
		vendor_in_put(v, buf, uart_rx_peek(port, &data) > 0U);
	}
}

/* @p v's bulk OUT into the UART, as the ring takes it (the host waits meanwhile); how many. */
static size_t vendor_to_uart(int port, struct vendor_if *v)
{
	struct bridge_port *p = &ports[port];
	size_t total = 0;

	while (true) {
		size_t n;

		if (p->out == NULL) {
			p->out = vendor_out_get(v);
			p->out_owner = v;
			p->out_off = 0;
			if (p->out == NULL) {
				return total;
			}
		}
		n = uart_tx_write(port, p->out->data + p->out_off, p->out->len - p->out_off);
		p->out_off += n;
		total += n;
		if (p->out_off < p->out->len) {
			return total;
		}
		/* A route may have changed since it came: it still goes to the UART. */
		vendor_out_done(p->out_owner, p->out);
		p->out = NULL;
	}
}

/*
 * Nobody reading a CDC ACM port (it is closed: the host's driver takes nothing then). The next
 * reader gets no old news, as with the stock firmware's small buffers.
 */
static void stale_check(int port, size_t moved)
{
	struct bridge_port *p = &ports[port];
	const uint8_t *data;
	size_t n = uart_rx_peek(port, &data);
	int64_t now = k_uptime_get();

	if (moved > 0U || n == 0U) {
		p->last_read_ms = now;
	} else if (now - p->last_read_ms > STALE_MS) {
		LOG_WRN("UART %d: %u bytes stale, %d IN buffers out", port, (unsigned int)n,
			vendor_in_flight(vendor_cdc[port]));
		while (n > 0U) {
			coproc_stats.stale_dropped[port] += n;
			uart_rx_consume(port, n);
			n = uart_rx_peek(port, &data);
		}
		p->last_read_ms = now;
	}
}

static void loopback_service(int port)
{
	const uint8_t *data;
	size_t n = uart_rx_peek(port, &data);
	static uint8_t chunk[128];

	/* What the stock firmware's self test expects back. */
	n = MIN(n, MIN(sizeof(chunk), uart_tx_room(port)));
	for (size_t k = 0; k < n; k++) {
		chunk[k] = data[k] ^ 0xA5U;
	}
	if (n > 0U) {
		uart_rx_consume(port, n);
		(void)uart_tx_write(port, chunk, n);
	}
}

static void drain_out(struct vendor_if *v)
{
	struct net_buf *buf;

	while ((buf = vendor_out_get(v)) != NULL) {
		vendor_out_done(v, buf);
	}
}

/* Interface 6 under test: the words count on, across buffers. */
static void test_source(void)
{
	struct vendor_if *v = vendor_link;
	struct net_buf *buf;
	size_t room;

	drain_out(v);
	while ((buf = vendor_in_get(v, &room)) != NULL) {
		size_t words = room / 4U;
		uint8_t *to = net_buf_add(buf, words * 4U);

		for (size_t k = 0; k < words; k++, to += 4) {
			sys_put_le32(test_count++, to);
		}
		coproc_stats.link_in += buf->len;
		vendor_in_put(v, buf, true);
	}
}

static void test_loopback(void)
{
	static struct net_buf *out;
	static size_t off;
	struct vendor_if *v = vendor_link;

	while (true) {
		struct net_buf *in;
		size_t room, n;

		if (out == NULL) {
			out = vendor_out_get(v);
			off = 0;
			if (out == NULL) {
				return;
			}
		}
		in = vendor_in_get(v, &room);
		if (in == NULL) {
			return;
		}
		n = MIN(room, out->len - off);
		net_buf_add_mem(in, out->data + off, n);
		off += n;
		coproc_stats.link_out += n;
		coproc_stats.link_in += n;
		vendor_in_put(v, in, off < out->len);
		if (off >= out->len) {
			vendor_out_done(v, out);
			out = NULL;
		}
	}
}

static void service_port(int port)
{
	struct bridge_port *p = &ports[port];
	struct vendor_if *web = vendor_webusb[port];
	struct vendor_if *cdc = vendor_cdc[port];
	const uint8_t *data;

	/* As the stock firmware, what comes on a WebUSB interface always goes to its UART. */
	if (p->route != ROUTE_LINK && vendor_enabled(web)) {
		(void)vendor_to_uart(port, web);
	}
	/* What comes on a CDC ACM port that does not own the UART goes nowhere. */
	if (p->route != ROUTE_CDC) {
		drain_out(cdc);
	}
	switch (p->route) {
	case ROUTE_CDC:
		(void)vendor_to_uart(port, cdc);
		stale_check(port, uart_to_vendor(port, cdc));
		break;
	case ROUTE_WEBUSB:
		(void)uart_to_vendor(port, web);
		break;
	case ROUTE_LINK:
		if (test_mode == COPROC_TEST_OFF) {
			coproc_stats.link_out += vendor_to_uart(port, vendor_link);
			coproc_stats.link_in += uart_to_vendor(port, vendor_link);
		} else {
			/* The UART has no way out meanwhile. */
			size_t n = uart_rx_peek(port, &data);

			uart_rx_consume(port, n);
		}
		break;
	case ROUTE_LOOPBACK:
		loopback_service(port);
		break;
	}
}

static void bridge_fn(void *a, void *b, void *c)
{
	int64_t next_errors = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		(void)k_sem_take(&wake, K_MSEC(1));
		if (k_uptime_get() >= next_errors) {
			next_errors = k_uptime_get() + ERRORS_MS;
			for (int k = 0; k < PORT_COUNT; k++) {
				uart_poll_errors(k);
			}
		}
		if (!usb_up) {
			/* Nobody to tell: no old news when somebody comes. */
			for (int k = 0; k < PORT_COUNT; k++) {
				const uint8_t *data;
				size_t n = uart_rx_peek(k, &data);

				uart_rx_consume(k, n);
			}
			continue;
		}
		if (test_mode == COPROC_TEST_SOURCE && vendor_enabled(vendor_link)) {
			test_source();
		} else if (test_mode == COPROC_TEST_LOOPBACK && vendor_enabled(vendor_link)) {
			test_loopback();
		}
		for (int k = 0; k < PORT_COUNT; k++) {
			service_port(k);
		}
	}
}

K_THREAD_STACK_DEFINE(bridge_stack, BRIDGE_STACK);
static struct k_thread bridge_thread;

int bridge_start(void)
{
	for (int k = 0; k < PORT_COUNT; k++) {
		struct bridge_port *p = &ports[k];

		p->cdc_line = (struct uart_config){
			.baudrate = 115200,
			.parity = UART_CFG_PARITY_NONE,
			.stop_bits = UART_CFG_STOP_BITS_1,
			.data_bits = UART_CFG_DATA_BITS_8,
			.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
		};
		p->line = p->cdc_line;
		p->webusb_baud = 115200;
		p->link_baud = LINK_BAUD;
	}
	regs_set(COPROC_REG_UART0_BAUD, (uint8_t)(115200U / 100U));
	regs_set(COPROC_REG_UART0_BAUD + 1, (uint8_t)((115200U / 100U) >> 8));
	k_thread_create(&bridge_thread, bridge_stack, K_THREAD_STACK_SIZEOF(bridge_stack),
			bridge_fn, NULL, NULL, NULL, BRIDGE_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&bridge_thread, "bridge");
	return 0;
}
