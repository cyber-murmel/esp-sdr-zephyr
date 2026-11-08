/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP-WebSDR's burst serial protocol on the esp-sdr receiver: the browser
 * viewer (ESPARGOS/esp-web-sdr, radio.js) and other clients of upstream's
 * firmware talk to this app as they do to upstream's main/targets/esp32s3.
 * Newline-terminated ASCII commands, one text reply each; a capture reply is
 * a DATA header followed by the packed samples.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>

/*
 * RX40/RX16 only matter to clients without RXLIMITS, and only on the S3.
 * RATE_MAX is the highest rate index upstream parses: beyond it a capture is
 * "ERR command", below it an unsupported rate is "ERR rate".
 */
#if defined(CONFIG_SOC_SERIES_ESP32S3)
#define FAMILY "S3"
#define CAPS_RATES " RX40 RX16"
#define RATE_MAX ESP_SDR_RATE_16MSPS
#elif defined(CONFIG_SOC_SERIES_ESP32C6)
#define FAMILY "C6"
#define CAPS_RATES ""
#define RATE_MAX ESP_SDR_RATE_80MSPS
#elif defined(CONFIG_SOC_SERIES_ESP32C5)
#define FAMILY "C5"
#define CAPS_RATES ""
#define RATE_MAX ESP_SDR_RATE_4MSPS
#else
#error "esp-sdr runs on the ESP32-S3, ESP32-C6 and ESP32-C5"
#endif

/* Upstream's longest line is 128 bytes with its terminator. */
#define LINE_LEN 128
/* A whole DATA payload must leave the UART within this. */
#define SEND_TIMEOUT_MS 2000
/* USB-Serial-JTAG's bulk packet size. */
#define USB_PACKET 64

/*
 * USB-Serial-JTAG sends each FIFO fill as one bulk packet. A send that ends on
 * a full packet needs a zero-length packet, or the host holds the data back
 * until the next short packet: a flush of the empty FIFO sends one, as
 * ESP-IDF's driver does.
 */
#define WEBSDR_UART_NODE DT_CHOSEN(esp_sdr_websdr_uart)
#define USB_ZLP DT_NODE_HAS_COMPAT(WEBSDR_UART_NODE, espressif_esp32_usb_serial)

static const struct device *const uart = DEVICE_DT_GET(WEBSDR_UART_NODE);
#define PORT_NAME (uart->name)

RING_BUF_DECLARE(rx_ring, 256);
static K_SEM_DEFINE(rx_ready, 0, 1);

/*
 * One send at a time: the ISR drains it straight from the caller's buffer.
 * tx_active keeps an RX interrupt from finishing a send that has not begun.
 */
static const uint8_t *volatile tx_next;
static volatile size_t tx_left;
static volatile bool tx_active;
/* The last fill was a full USB packet: end the transfer with a ZLP. */
static volatile bool tx_zlp;
static K_SEM_DEFINE(tx_done, 0, 1);

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	for (;;) {
		uart_irq_update(dev);
		if (uart_irq_is_pending(dev) <= 0) {
			break;
		}
		if (uart_irq_rx_ready(dev) > 0) {
			uint8_t buf[64];
			int n;

			while ((n = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
				/* A client sends one command and waits for its reply. */
				(void)ring_buf_put(&rx_ring, buf, n);
			}
			k_sem_give(&rx_ready);
		}
		if (uart_irq_tx_ready(dev) > 0) {
			if (!tx_active) {
				/* Pending stays set while TX is enabled: never spin on it. */
				uart_irq_tx_disable(dev);
				continue;
			}
			if (tx_left > 0) {
				int n = uart_fifo_fill(dev, tx_next, tx_left);

				if (n > 0) {
					tx_next += n;
					tx_left -= n;
				}
				if (USB_ZLP && tx_left == 0 && n == USB_PACKET) {
					/* Once the host has taken this packet. */
					tx_zlp = true;
					continue;
				}
			} else if (tx_zlp) {
				tx_zlp = false;
				(void)uart_fifo_fill(dev, tx_next, 0);
			}
			if (tx_left == 0 && !tx_zlp) {
				tx_active = false;
				uart_irq_tx_disable(dev);
				k_sem_give(&tx_done);
			}
		}
	}
}

static int send(const void *data, size_t len)
{
	if (len == 0) {
		return 0;
	}
	k_sem_reset(&tx_done);
	tx_next = data;
	tx_left = len;
	tx_zlp = false;
	tx_active = true;
	uart_irq_tx_enable(uart);
	if (k_sem_take(&tx_done, K_MSEC(SEND_TIMEOUT_MS)) != 0) {
		uart_irq_tx_disable(uart);
		tx_active = false;
		tx_left = 0;
		return -ETIMEDOUT;
	}
	return 0;
}

static int port_init(void)
{
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	uart_irq_callback_set(uart, uart_isr);
	uart_irq_rx_enable(uart);
	return 0;
}

static uint8_t next_byte(void)
{
	uint8_t c;

	while (ring_buf_get(&rx_ring, &c, 1) == 0) {
		k_sem_take(&rx_ready, K_FOREVER);
	}
	return c;
}


static void reply(const char *s)
{
	(void)send(s, strlen(s));
}

/* Whole-word unsigned decimal, as upstream's "%u %c" sscanf accepts. */
static bool parse_u32(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (*s < '0' || *s > '9') {
		return false;
	}
	v = strtoul(s, &end, 10);
	if (*end != '\0' || v > UINT32_MAX) {
		return false;
	}
	*out = v;
	return true;
}

static bool parse_u32_pair(const char *s, uint32_t *a, uint32_t *b)
{
	char first[16];
	const char *space = strchr(s, ' ');

	if (space == NULL || space - s >= (ptrdiff_t)sizeof(first)) {
		return false;
	}
	memcpy(first, s, space - s);
	first[space - s] = '\0';
	return parse_u32(first, a) && parse_u32(space + 1, b);
}

/*
 * IQ8 in upstream's field order: the low 10-bit field first, then the high
 * one, each truncated to its upper eight bits. esp_sdr_rx_pack_iq8() puts
 * the high field (I, see esp_sdr_rx_i()) first, which the viewer would show
 * mirrored; IQ10 already matches (esp_sdr_rx_pack_iq10()). In place.
 */
static size_t pack_iq8_upstream(uint32_t *words, size_t count)
{
	uint8_t *out = (uint8_t *)words;

	for (size_t j = 0; j < count; j++) {
		uint32_t w = words[j];

		out[2 * j] = (uint8_t)(w >> 2);
		out[2 * j + 1] = (uint8_t)(w >> 12);
	}
	return 2 * count;
}

static void capture(uint32_t count, uint32_t rate, unsigned int bits)
{
	struct esp_sdr_rx_burst b;
	char header[48];
	size_t len;
	uint32_t crc;
	int ret;

	if (esp_sdr_rx_rate_hz((enum esp_sdr_rate)rate) == 0) {
		reply("ERR rate\n");
		return;
	}
	ret = esp_sdr_rx_capture((enum esp_sdr_rate)rate, count, &b);
	if (ret == -ETIMEDOUT) {
		reply("ERR capture_timeout\n");
		return;
	} else if (ret != 0) {
		reply("ERR capture\n");
		return;
	}
	len = bits == 8 ? pack_iq8_upstream(b.words, b.count)
			: esp_sdr_rx_pack_iq10(b.words, b.count, (uint8_t *)b.words);
	crc = crc32_ieee((const uint8_t *)b.words, len);
	snprintk(header, sizeof(header), "DATA %u %08" PRIx32 " %" PRIu32 "\n",
		 (unsigned int)b.count, crc, b.elapsed_us);
	if (send(header, strlen(header)) == 0) {
		(void)send(b.words, len);
	}
}

static void reply_limits(void)
{
	char buf[160];
	uint32_t bw_min, bw_max;
	int n;

	esp_sdr_rx_bandwidth_range(&bw_min, &bw_max);
	n = snprintk(buf, sizeof(buf),
		     "LIMITS {\"gain\":[0,%d,1],\"bandwidth\":[%" PRIu32 ",%" PRIu32
		     ",1,0],\"rates\":[",
		     esp_sdr_rx_gain_max(), bw_min, bw_max);
	/* The viewer's rate index is enum esp_sdr_rate. */
	for (int r = ESP_SDR_RATE_80MSPS, first = 1; r <= ESP_SDR_RATE_16MSPS; r++) {
		uint32_t hz = esp_sdr_rx_rate_hz((enum esp_sdr_rate)r);

		if (hz != 0) {
			n += snprintk(buf + n, sizeof(buf) - n, "%s%" PRIu32, first ? "" : ",", hz);
			first = 0;
		}
	}
	snprintk(buf + n, sizeof(buf) - n, "],\"bits\":[8,10]}\n");
	reply(buf);
}

static void reply_gain(void)
{
	char buf[64];
	int g = esp_sdr_rx_get_gain();
	bool manual = g != ESP_SDR_RX_GAIN_AUTO;

	snprintk(buf, sizeof(buf), "GAIN %s %d 0 %d %d\n", manual ? "MANUAL" : "HARDWARE", g,
		 esp_sdr_rx_gain_max(), manual);
	reply(buf);
}

static void handle(const char *line)
{
	uint32_t a, b;
	char buf[48];

	if (strncmp(line, "SYNC ", 5) == 0 && line[5] >= '0' && line[5] <= '9' &&
	    strspn(line + 5, "0123456789") == strlen(line + 5) && strlen(line + 5) <= 20) {
		/* Resynchronization marker: echo it. */
		char echo[LINE_LEN + 1];

		snprintk(echo, sizeof(echo), "%s\n", line);
		reply(echo);
	} else if (strcmp(line, "INFO") == 0) {
		snprintk(buf, sizeof(buf), FAMILY "SDR 6 burst %u\n", ESP_SDR_SAMPLES_MAX);
		reply(buf);
	} else if (strcmp(line, "CAPS") == 0) {
		reply("CAPS RXLIMITS SERIALLEASE TUNEEXT" CAPS_RATES " GAIN HWAGC IQ8\n");
	} else if (strcmp(line, "LIMITS?") == 0) {
		reply_limits();
	} else if (strcmp(line, "RANGE?") == 0) {
		snprintk(buf, sizeof(buf), "RANGE %u %u 1\n", ESP_SDR_FREQ_MIN_MHZ,
			 ESP_SDR_FREQ_MAX_MHZ);
		reply(buf);
	} else if (strncmp(line, "FREQ ", 5) == 0 && parse_u32(line + 5, &a)) {
		int ret = esp_sdr_set_freq(a);

		/*
		 * Upstream answers OK wherever it lands; here the PLL
		 * calibration's VCO window check says when it may not lock.
		 */
		reply(ret == 0 ? "OK\n" : ret == -ERANGE ? "ERR pll_range\n" : "ERR command\n");
	} else if (strncmp(line, "BANDWIDTH ", 10) == 0 && parse_u32(line + 10, &a)) {
		reply(esp_sdr_rx_set_bandwidth(a) == 0 ? "OK\n" : "ERR command\n");
#if !defined(CONFIG_SOC_SERIES_ESP32C6)
	} else if (strcmp(line, "LPF AUTO") == 0) {
		/* Upstream's C6 has no LPF command. */
		reply(esp_sdr_rx_set_lpf(ESP_SDR_RX_LPF_AUTO) == 0 ? "OK\n" : "ERR command\n");
#endif
	} else if (strcmp(line, "GAIN?") == 0) {
		reply_gain();
	} else if (strcmp(line, "GAIN HARDWARE") == 0) {
		reply(esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO) == 0 ? "OK\n" : "ERR command\n");
	} else if (strncmp(line, "GAIN MANUAL ", 12) == 0 && parse_u32(line + 12, &a) &&
		   a <= (uint32_t)esp_sdr_rx_gain_max()) {
		int ret = esp_sdr_rx_set_gain(a);

		reply(ret == 0 ? "OK\n" : ret == -ENOTSUP ? "ERR gain\n" : "ERR command\n");
	} else if ((strncmp(line, "CAP16 ", 6) == 0 || strncmp(line, "CAP20 ", 6) == 0) &&
		   parse_u32_pair(line + 6, &a, &b) && a >= ESP_SDR_SAMPLES_MIN &&
		   a <= ESP_SDR_SAMPLES_MAX && b <= RATE_MAX) {
		capture(a, b, line[4] == '6' ? 8 : 10);
	} else if (strcmp(line, "RELEASE") == 0) {
		/* One serial port: nobody else can hold the radio. */
		reply("OK\n");
	} else {
		reply("ERR command\n");
	}
}

int main(void)
{
	char line[LINE_LEN];
	size_t len = 0;
	bool overflow = false;
	int ret;

	/* On the console, where there is one besides the protocol's port. */
	ret = port_init();
	if (ret != 0) {
		printk("websdr: %s not ready\n", PORT_NAME);
		return ret;
	}
	ret = esp_sdr_init();
	if (ret != 0) {
		printk("websdr: esp_sdr_init() %d\n", ret);
		return ret;
	}
	printk("websdr: ready on %s\n", PORT_NAME);

	for (;;) {
		uint8_t c = next_byte();

		if (c == '\r') {
			continue;
		}
		if (c != '\n') {
			if (len < sizeof(line) - 1) {
				line[len++] = c;
			} else {
				overflow = true;
			}
			continue;
		}
		line[len] = '\0';
		if (overflow) {
			reply("ERR command_length\n");
		} else if (len > 0) {
			handle(line);
		}
		len = 0;
		overflow = false;
	}
	return 0;
}
