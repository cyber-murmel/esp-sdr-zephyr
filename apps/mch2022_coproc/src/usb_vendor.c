/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The USB functions, all bulk pipes the bridge thread moves data on: the stock firmware's two CDC
 * ACM ports (its usb_descriptors.c, the endpoints too), its WebUSB consoles 4 and 5 with their class
 * requests (its webusb_task.c), and 6, new, with the requests of esp_sdr/mch2022_coproc.h. The CDC
 * ports are here and not Zephyr's class because that one sends a packet per buffer and goes through
 * a thread for each: it missed the frame nearly every time, 69 kB/s where the UART brings 200.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/class/usb_cdc.h>
#include <zephyr/usb/usbd.h>

#include "coproc.h"
#include "regmap.h"

LOG_MODULE_REGISTER(usb_vendor, LOG_LEVEL_INF);

#define BULK_MPS 64U

/* The stock firmware's class requests. */
#define WEBUSB_SET_STATE    0x22
#define WEBUSB_RESET        0x23
#define WEBUSB_BAUDRATE     0x24
#define WEBUSB_SET_MODE     0x25
#define WEBUSB_GET_MODE     0x26
#define WEBUSB_GET_FW_VER   0x27

/* The stock firmware's notification endpoint: never used, as there. */
#define CDC_NOTIFY_MPS      8U
#define CDC_NOTIFY_INTERVAL 16U

struct vendor_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor out, in;
	struct usb_desc_header nil;
} __packed;

struct cdc_desc {
	struct usb_association_descriptor iad;
	struct usb_if_descriptor if0;
	struct cdc_header_descriptor header;
	struct cdc_cm_descriptor cm;
	struct cdc_acm_descriptor acm;
	struct cdc_union_descriptor un;
	struct usb_ep_descriptor notify;
	struct usb_if_descriptor if1;
	struct usb_ep_descriptor out, in;
	struct usb_desc_header nil;
} __packed;

enum vendor_kind {
	KIND_CDC,
	KIND_WEBUSB,
	KIND_LINK,
};

struct vendor_if {
	struct usbd_class_data *c_data;
	enum vendor_kind kind;
	struct usb_ep_descriptor *ep_in_desc, *ep_out_desc;
	/* The string's interface descriptor; CDC: the interface numbers to tell its descriptors. */
	struct usb_if_descriptor *str_if;
	struct cdc_desc *cdc;
	struct cdc_acm_line_coding line;
	const struct usb_desc_header *const *fs;
	struct usbd_desc_node *str;
	struct net_buf_pool *in_pool, *out_pool;
	uint16_t in_size, out_size;
	/* Buffers out with the controller, at most. */
	uint8_t in_max, out_max;
	atomic_t in_flight, out_flight;
	atomic_t enabled;
	struct k_fifo out_done;
	/* The UART of a CDC port or WebUSB interface; -1 for the link. */
	int port;
};

static uint8_t ep_in(struct vendor_if *v)
{
	return v->ep_in_desc->bEndpointAddress;
}

static uint8_t ep_out(struct vendor_if *v)
{
	return v->ep_out_desc->bEndpointAddress;
}

int vendor_in_flight(struct vendor_if *v)
{
	return (int)atomic_get(&v->in_flight);
}

bool vendor_enabled(struct vendor_if *v)
{
	return atomic_get(&v->enabled) != 0;
}

struct net_buf *vendor_in_get(struct vendor_if *v, size_t *room)
{
	struct net_buf *buf;
	struct udc_buf_info *bi;

	if (!vendor_enabled(v) || atomic_get(&v->in_flight) >= v->in_max) {
		return NULL;
	}
	buf = net_buf_alloc(v->in_pool, K_NO_WAIT);
	if (buf == NULL) {
		return NULL;
	}
	bi = udc_get_buf_info(buf);
	memset(bi, 0, sizeof(*bi));
	bi->ep = ep_in(v);
	*room = net_buf_tailroom(buf);
	return buf;
}

void vendor_in_put(struct vendor_if *v, struct net_buf *buf, bool more)
{
	/* A whole number of packets does not end the host's read: unless more follows at once. */
	if (!more && buf->len % BULK_MPS == 0U) {
		udc_ep_buf_set_zlp(buf);
	}
	atomic_inc(&v->in_flight);
	if (usbd_ep_enqueue(v->c_data, buf) != 0) {
		atomic_dec(&v->in_flight);
		net_buf_unref(buf);
	}
}

static void out_arm(struct vendor_if *v)
{
	while (vendor_enabled(v) && atomic_get(&v->out_flight) < v->out_max) {
		struct net_buf *buf = net_buf_alloc(v->out_pool, K_NO_WAIT);
		struct udc_buf_info *bi;

		if (buf == NULL) {
			return;
		}
		bi = udc_get_buf_info(buf);
		memset(bi, 0, sizeof(*bi));
		bi->ep = ep_out(v);
		atomic_inc(&v->out_flight);
		if (usbd_ep_enqueue(v->c_data, buf) != 0) {
			atomic_dec(&v->out_flight);
			net_buf_unref(buf);
			return;
		}
	}
}

struct net_buf *vendor_out_get(struct vendor_if *v)
{
	return k_fifo_get(&v->out_done, K_NO_WAIT);
}

void vendor_out_done(struct vendor_if *v, struct net_buf *buf)
{
	net_buf_unref(buf);
	out_arm(v);
}

static int vendor_request(struct usbd_class_data *const c_data, struct net_buf *buf, int err)
{
	struct vendor_if *v = usbd_class_get_private(c_data);
	struct udc_buf_info *bi = udc_get_buf_info(buf);

	if (bi->ep == ep_in(v)) {
		atomic_dec(&v->in_flight);
		net_buf_unref(buf);
	} else {
		atomic_dec(&v->out_flight);
		if (err == 0 && buf->len > 0U) {
			k_fifo_put(&v->out_done, buf);
		} else {
			net_buf_unref(buf);
			out_arm(v);
		}
	}
	bridge_wake();
	return 0;
}

static void vendor_enable(struct usbd_class_data *const c_data)
{
	struct vendor_if *v = usbd_class_get_private(c_data);

	atomic_set(&v->enabled, 1);
	out_arm(v);
	bridge_wake();
}

static void vendor_disable(struct usbd_class_data *const c_data)
{
	struct vendor_if *v = usbd_class_get_private(c_data);
	struct net_buf *buf;

	atomic_set(&v->enabled, 0);
	while ((buf = k_fifo_get(&v->out_done, K_NO_WAIT)) != NULL) {
		net_buf_unref(buf);
	}
}

static void vendor_update(struct usbd_class_data *const c_data, uint8_t iface, uint8_t alternate)
{
	ARG_UNUSED(c_data);
	ARG_UNUSED(iface);
	ARG_UNUSED(alternate);
}

static const void *vendor_get_desc(struct usbd_class_data *const c_data, const enum usbd_speed speed)
{
	struct vendor_if *v = usbd_class_get_private(c_data);

	ARG_UNUSED(speed);
	return v->fs;
}

static int vendor_init(struct usbd_class_data *const c_data)
{
	struct vendor_if *v = usbd_class_get_private(c_data);
	struct usbd_context *ctx = usbd_class_get_ctx(c_data);

	v->c_data = c_data;
	if (v->str_if->iInterface == 0U && usbd_add_descriptor(ctx, v->str) == 0) {
		v->str_if->iInterface = usbd_str_desc_get_idx(v->str);
	}
	if (v->cdc != NULL) {
		v->cdc->un.bControlInterface = v->cdc->if0.bInterfaceNumber;
		v->cdc->un.bSubordinateInterface0 = v->cdc->if1.bInterfaceNumber;
		v->cdc->cm.bDataInterface = v->cdc->if1.bInterfaceNumber;
	}
	return 0;
}

/* ---- the CDC ports ---- */

static void cdc_line_changed(struct vendor_if *v)
{
	static const uint8_t stop[] = {UART_CFG_STOP_BITS_1, UART_CFG_STOP_BITS_1_5,
				       UART_CFG_STOP_BITS_2};
	static const uint8_t parity[] = {UART_CFG_PARITY_NONE, UART_CFG_PARITY_ODD,
					 UART_CFG_PARITY_EVEN, UART_CFG_PARITY_MARK,
					 UART_CFG_PARITY_SPACE};
	struct uart_config cfg = {
		.baudrate = sys_le32_to_cpu(v->line.dwDTERate),
		.parity = v->line.bParityType < ARRAY_SIZE(parity) ? parity[v->line.bParityType]
								   : UART_CFG_PARITY_NONE,
		.stop_bits = v->line.bCharFormat < ARRAY_SIZE(stop) ? stop[v->line.bCharFormat]
								    : UART_CFG_STOP_BITS_1,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

	/* 16 bits and whatever else: 8, as the stock firmware. */
	switch (v->line.bDataBits) {
	case 5:
		cfg.data_bits = UART_CFG_DATA_BITS_5;
		break;
	case 6:
		cfg.data_bits = UART_CFG_DATA_BITS_6;
		break;
	case 7:
		cfg.data_bits = UART_CFG_DATA_BITS_7;
		break;
	default:
		cfg.data_bits = UART_CFG_DATA_BITS_8;
		break;
	}
	bridge_cdc_line(v->port, &cfg);
}

static int cdc_to_dev(struct vendor_if *v, const struct usb_setup_packet *setup,
		      const struct net_buf *buf)
{
	switch (setup->bRequest) {
	case SET_LINE_CODING:
		if (buf == NULL || buf->len != sizeof(v->line)) {
			return -EINVAL;
		}
		memcpy(&v->line, buf->data, sizeof(v->line));
		cdc_line_changed(v);
		return 0;
	case SET_CONTROL_LINE_STATE:
		/* In the request's own handling, in order: a reset is a quick sequence of these. */
		if (v->port == PORT_ESP32) {
			esp32_line_state((setup->wValue & SET_CONTROL_LINE_STATE_DTR) != 0U,
					 (setup->wValue & SET_CONTROL_LINE_STATE_RTS) != 0U);
		}
		return 0;
	default:
		return -ENOTSUP;
	}
}

static struct net_buf *cdc_to_host(struct vendor_if *v, const struct usb_setup_packet *setup)
{
	struct net_buf *buf;
	uint16_t len = MIN(setup->wLength, sizeof(v->line));

	if (setup->bRequest != GET_LINE_CODING) {
		return NULL;
	}
	buf = usbd_ep_ctrl_data_in_alloc(usbd_class_get_ctx(v->c_data), len);
	if (buf != NULL) {
		net_buf_add_mem(buf, &v->line, len);
	}
	return buf;
}

/* ---- the stock class requests ---- */

static int webusb_to_dev(struct vendor_if *v, const struct usb_setup_packet *setup)
{
	const int port = v->port;

	switch (setup->bRequest) {
	case WEBUSB_SET_STATE:
		bridge_webusb_status(port, setup->wValue);
		return 0;
	case WEBUSB_RESET:
		if (port != PORT_ESP32) {
			return -ENOTSUP;
		}
		/* Past the reset lock: this interface is for the launcher's own tools. */
		esp32_reset(setup->wValue != 0U);
		return 0;
	case WEBUSB_BAUDRATE:
		bridge_webusb_baud(port, (uint32_t)setup->wValue * 100U);
		return 0;
	case WEBUSB_SET_MODE:
		if (port != PORT_ESP32) {
			return -ENOTSUP;
		}
		regs_webusb_mode((uint8_t)setup->wValue);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static struct net_buf *webusb_to_host(struct vendor_if *v, const struct usb_setup_packet *setup)
{
	struct usbd_context *ctx = usbd_class_get_ctx(v->c_data);
	uint8_t val;
	struct net_buf *buf;

	switch (setup->bRequest) {
	case WEBUSB_GET_MODE:
		if (v->port != PORT_ESP32) {
			return NULL;
		}
		val = regs_webusb_mode_get();
		break;
	case WEBUSB_GET_FW_VER:
		val = REGMAP_FW_VERSION;
		break;
	default:
		return NULL;
	}
	buf = usbd_ep_ctrl_data_in_alloc(ctx, MIN(setup->wLength, 1U));
	if (buf != NULL && setup->wLength > 0U) {
		net_buf_add_u8(buf, val);
	}
	return buf;
}

/* ---- interface 6 ---- */

static int link_to_dev(const struct usb_setup_packet *setup, const struct net_buf *buf)
{
	switch (setup->bRequest) {
	case COPROC_REQ_RESET_STATS:
		memset(&coproc_stats, 0, sizeof(coproc_stats));
		return 0;
	case COPROC_REQ_SET_BAUD:
		if (buf == NULL || buf->len != 4U) {
			return -EINVAL;
		}
		return bridge_link_baud(setup->wValue, sys_get_le32(buf->data));
	case COPROC_REQ_ESP32_RESET:
		esp32_reset(setup->wValue != 0U);
		return 0;
	case COPROC_REQ_FPGA_RESET:
		fpga_run(setup->wValue == 1U);
		if (setup->wValue == 2U) {
			k_busy_wait(10);
			fpga_run(true);
		}
		return 0;
	case COPROC_REQ_BOOTSEL:
		sysctl_reboot_bootsel();
		return 0;
	case COPROC_REQ_BOOTLOADER:
		sysctl_reboot_bootloader();
		return 0;
	case COPROC_REQ_LINK_OPEN:
		bridge_link_open(true);
		return 0;
	case COPROC_REQ_LINK_CLOSE:
		bridge_link_open(false);
		return 0;
	case COPROC_REQ_FPGA_FASTLINK:
		if (!bridge_link_is_open() && setup->wValue != 0U) {
			return -EINVAL;
		}
		return bridge_fastlink((uint32_t)setup->wValue * 100000U);
	case COPROC_REQ_RESET_MODE:
		if (setup->wValue > COPROC_RESET_CIRCUIT) {
			return -EINVAL;
		}
		esp32_set_reset_mode((uint8_t)setup->wValue);
		return 0;
	case COPROC_REQ_TEST_MODE:
		if (setup->wValue > COPROC_TEST_CRASH) {
			return -EINVAL;
		}
		bridge_set_test((uint8_t)setup->wValue);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static struct net_buf *link_to_host(struct vendor_if *v, const struct usb_setup_packet *setup)
{
	struct usbd_context *ctx = usbd_class_get_ctx(v->c_data);
	struct net_buf *buf;
	const uint16_t max = setup->wLength;

	switch (setup->bRequest) {
	case COPROC_REQ_GET_INFO: {
		struct coproc_info info = {
			.magic = sys_cpu_to_le32(COPROC_INFO_MAGIC),
			.api_major = COPROC_API_MAJOR,
			.api_minor = COPROC_API_MINOR,
			.features = sys_cpu_to_le16(COPROC_F_LINK | COPROC_F_FASTLINK),
			.rosc_status = sys_cpu_to_le32(sysctl_rosc_status()),
			.reset_reason = sys_cpu_to_le32(sysctl_reset_reason()),
			.uptime_ms = sys_cpu_to_le32(k_uptime_get_32()),
			.reset_mode = esp32_get_reset_mode(),
			.test_mode = bridge_get_test(),
			.link_open = bridge_link_is_open() ? 1U : 0U,
		};

		buf = usbd_ep_ctrl_data_in_alloc(ctx, MIN(max, sizeof(info)));
		if (buf != NULL) {
			net_buf_add_mem(buf, &info, MIN(max, sizeof(info)));
		}
		return buf;
	}
	case COPROC_REQ_GET_STATS:
		buf = usbd_ep_ctrl_data_in_alloc(ctx, MIN(max, sizeof(coproc_stats)));
		if (buf != NULL) {
			net_buf_add_mem(buf, &coproc_stats, MIN(max, sizeof(coproc_stats)));
		}
		return buf;
	case COPROC_REQ_GET_BAUD: {
		uint8_t le[4];

		sys_put_le32(bridge_baud(setup->wValue), le);
		buf = usbd_ep_ctrl_data_in_alloc(ctx, MIN(max, sizeof(le)));
		if (buf != NULL) {
			net_buf_add_mem(buf, le, MIN(max, sizeof(le)));
		}
		return buf;
	}
	case COPROC_REQ_GET_LOG: {
		uint8_t chunk[256];
		size_t n = fwlog_read(chunk, MIN(max, sizeof(chunk)));

		buf = usbd_ep_ctrl_data_in_alloc(ctx, MAX(n, 1U));
		if (buf != NULL) {
			net_buf_add_mem(buf, chunk, n);
		}
		return buf;
	}
	default:
		return NULL;
	}
}

static int vendor_control_to_dev(struct usbd_class_data *const c_data,
				 const struct usb_setup_packet *const setup,
				 const struct net_buf *const buf)
{
	struct vendor_if *v = usbd_class_get_private(c_data);

	/* First with no buffer: take the data stage. Then again, with its data. */
	if (setup->wLength != 0U && buf == NULL) {
		return 0;
	}
	switch (v->kind) {
	case KIND_CDC:
		return setup->RequestType.type == USB_REQTYPE_TYPE_CLASS
			       ? cdc_to_dev(v, setup, buf) : -ENOTSUP;
	case KIND_WEBUSB:
		return setup->RequestType.type == USB_REQTYPE_TYPE_CLASS
			       ? webusb_to_dev(v, setup) : -ENOTSUP;
	default:
		return setup->RequestType.type == USB_REQTYPE_TYPE_VENDOR
			       ? link_to_dev(setup, buf) : -ENOTSUP;
	}
}


static struct net_buf *vendor_control_to_host(struct usbd_class_data *const c_data,
					      const struct usb_setup_packet *const setup)
{
	struct vendor_if *v = usbd_class_get_private(c_data);

	switch (v->kind) {
	case KIND_CDC:
		return setup->RequestType.type == USB_REQTYPE_TYPE_CLASS ? cdc_to_host(v, setup)
									 : NULL;
	case KIND_WEBUSB:
		return setup->RequestType.type == USB_REQTYPE_TYPE_CLASS ? webusb_to_host(v, setup)
									 : NULL;
	default:
		return setup->RequestType.type == USB_REQTYPE_TYPE_VENDOR ? link_to_host(v, setup)
									  : NULL;
	}
}

static const struct usbd_class_api vendor_api = {
	.update = vendor_update,
	.control_to_dev = vendor_control_to_dev,
	.control_to_host = vendor_control_to_host,
	.request = vendor_request,
	.enable = vendor_enable,
	.disable = vendor_disable,
	.init = vendor_init,
	.get_desc = vendor_get_desc,
};

#define BULK_EP(addr)                                                                              \
	{                                                                                          \
		.bLength = sizeof(struct usb_ep_descriptor),                                       \
		.bDescriptorType = USB_DESC_ENDPOINT,                                              \
		.bEndpointAddress = (addr) | USBD_EP_ADDR_FIXED,                                   \
		.bmAttributes = USB_EP_TYPE_BULK,                                                  \
		.wMaxPacketSize = sys_cpu_to_le16(BULK_MPS),                                       \
	}

#define VENDOR_POOLS(n, in_count, in_bytes, out_count, out_bytes)                                  \
	UDC_BUF_POOL_DEFINE(n##_in_pool, in_count, in_bytes, sizeof(struct udc_buf_info), NULL);   \
	UDC_BUF_POOL_DEFINE(n##_out_pool, out_count, out_bytes, sizeof(struct udc_buf_info), NULL)

/* A vendor interface: @p n its name, @p ep its endpoint number, @p port_ its UART or -1. */
#define VENDOR_DEFINE(n, kind_, ep, port_, text, in_count, in_bytes, out_count, out_bytes)         \
	VENDOR_POOLS(n, in_count, in_bytes, out_count, out_bytes);                                 \
	USBD_DESC_STRING_DEFINE(n##_str, text, USBD_DUT_STRING_INTERFACE);                         \
	static struct vendor_desc n##_desc = {                                                     \
		.if0 = {                                                                           \
			.bLength = sizeof(struct usb_if_descriptor),                               \
			.bDescriptorType = USB_DESC_INTERFACE,                                     \
			.bNumEndpoints = 2,                                                        \
			.bInterfaceClass = USB_BCC_VENDOR,                                         \
		},                                                                                 \
		.out = BULK_EP(ep),                                                                \
		.in = BULK_EP(USB_EP_DIR_IN | (ep)),                                               \
	};                                                                                         \
	static const struct usb_desc_header *const n##_descs[] = {                                 \
		(struct usb_desc_header *)&n##_desc.if0,                                           \
		(struct usb_desc_header *)&n##_desc.out,                                           \
		(struct usb_desc_header *)&n##_desc.in,                                            \
		(struct usb_desc_header *)&n##_desc.nil,                                           \
	};                                                                                         \
	static struct vendor_if n##_if = {                                                         \
		.kind = kind_,                                                                     \
		.ep_in_desc = &n##_desc.in,                                                        \
		.ep_out_desc = &n##_desc.out,                                                      \
		.str_if = &n##_desc.if0,                                                           \
		.fs = n##_descs,                                                                   \
		.str = &n##_str,                                                                   \
		.in_pool = &n##_in_pool,                                                           \
		.out_pool = &n##_out_pool,                                                         \
		.in_size = in_bytes,                                                               \
		.out_size = out_bytes,                                                             \
		.in_max = in_count,                                                                \
		.out_max = out_count,                                                              \
		.out_done = Z_FIFO_INITIALIZER(n##_if.out_done),                                   \
		.port = port_,                                                                     \
	};                                                                                         \
	USBD_DEFINE_CLASS(n, &vendor_api, &n##_if, NULL)

/* A CDC ACM port, descriptor by descriptor the stock firmware's. */
#define CDC_DEFINE(n, ep_notify, ep_data, port_, text, in_count, in_bytes, out_count, out_bytes)   \
	VENDOR_POOLS(n, in_count, in_bytes, out_count, out_bytes);                                 \
	USBD_DESC_STRING_DEFINE(n##_str, text, USBD_DUT_STRING_INTERFACE);                         \
	static struct cdc_desc n##_desc = {                                                        \
		.iad = {                                                                           \
			.bLength = sizeof(struct usb_association_descriptor),                      \
			.bDescriptorType = USB_DESC_INTERFACE_ASSOC,                               \
			.bInterfaceCount = 2,                                                      \
			.bFunctionClass = USB_BCC_CDC_CONTROL,                                     \
			.bFunctionSubClass = ACM_SUBCLASS,                                         \
		},                                                                                 \
		.if0 = {                                                                           \
			.bLength = sizeof(struct usb_if_descriptor),                               \
			.bDescriptorType = USB_DESC_INTERFACE,                                     \
			.bNumEndpoints = 1,                                                        \
			.bInterfaceClass = USB_BCC_CDC_CONTROL,                                    \
			.bInterfaceSubClass = ACM_SUBCLASS,                                        \
		},                                                                                 \
		.header = {                                                                        \
			.bFunctionLength = sizeof(struct cdc_header_descriptor),                   \
			.bDescriptorType = USB_DESC_CS_INTERFACE,                                  \
			.bDescriptorSubtype = HEADER_FUNC_DESC,                                    \
			.bcdCDC = sys_cpu_to_le16(0x0120),                                         \
		},                                                                                 \
		.cm = {                                                                            \
			.bFunctionLength = sizeof(struct cdc_cm_descriptor),                       \
			.bDescriptorType = USB_DESC_CS_INTERFACE,                                  \
			.bDescriptorSubtype = CALL_MANAGEMENT_FUNC_DESC,                           \
		},                                                                                 \
		.acm = {                                                                           \
			.bFunctionLength = sizeof(struct cdc_acm_descriptor),                      \
			.bDescriptorType = USB_DESC_CS_INTERFACE,                                  \
			.bDescriptorSubtype = ACM_FUNC_DESC,                                       \
			/* Line coding and the control lines. */                                   \
			.bmCapabilities = BIT(1),                                                  \
		},                                                                                 \
		.un = {                                                                            \
			.bFunctionLength = sizeof(struct cdc_union_descriptor),                    \
			.bDescriptorType = USB_DESC_CS_INTERFACE,                                  \
			.bDescriptorSubtype = UNION_FUNC_DESC,                                     \
		},                                                                                 \
		.notify = {                                                                        \
			.bLength = sizeof(struct usb_ep_descriptor),                               \
			.bDescriptorType = USB_DESC_ENDPOINT,                                      \
			.bEndpointAddress = USB_EP_DIR_IN | (ep_notify) | USBD_EP_ADDR_FIXED,      \
			.bmAttributes = USB_EP_TYPE_INTERRUPT,                                     \
			.wMaxPacketSize = sys_cpu_to_le16(CDC_NOTIFY_MPS),                         \
			.bInterval = CDC_NOTIFY_INTERVAL,                                          \
		},                                                                                 \
		.if1 = {                                                                           \
			.bLength = sizeof(struct usb_if_descriptor),                               \
			.bDescriptorType = USB_DESC_INTERFACE,                                     \
			.bNumEndpoints = 2,                                                        \
			.bInterfaceClass = USB_BCC_CDC_DATA,                                       \
		},                                                                                 \
		.out = BULK_EP(ep_data),                                                           \
		.in = BULK_EP(USB_EP_DIR_IN | (ep_data)),                                          \
	};                                                                                         \
	static const struct usb_desc_header *const n##_descs[] = {                                 \
		(struct usb_desc_header *)&n##_desc.iad,                                           \
		(struct usb_desc_header *)&n##_desc.if0,                                           \
		(struct usb_desc_header *)&n##_desc.header,                                        \
		(struct usb_desc_header *)&n##_desc.cm,                                            \
		(struct usb_desc_header *)&n##_desc.acm,                                           \
		(struct usb_desc_header *)&n##_desc.un,                                            \
		(struct usb_desc_header *)&n##_desc.notify,                                        \
		(struct usb_desc_header *)&n##_desc.if1,                                           \
		(struct usb_desc_header *)&n##_desc.out,                                           \
		(struct usb_desc_header *)&n##_desc.in,                                            \
		(struct usb_desc_header *)&n##_desc.nil,                                           \
	};                                                                                         \
	static struct vendor_if n##_if = {                                                         \
		.kind = KIND_CDC,                                                                  \
		.ep_in_desc = &n##_desc.in,                                                        \
		.ep_out_desc = &n##_desc.out,                                                      \
		.str_if = &n##_desc.if0,                                                           \
		.cdc = &n##_desc,                                                                  \
		.line = {                                                                          \
			.dwDTERate = sys_cpu_to_le32(115200),                                      \
			.bDataBits = 8,                                                            \
		},                                                                                 \
		.fs = n##_descs,                                                                   \
		.str = &n##_str,                                                                   \
		.in_pool = &n##_in_pool,                                                           \
		.out_pool = &n##_out_pool,                                                         \
		.in_size = in_bytes,                                                               \
		.out_size = out_bytes,                                                             \
		.in_max = in_count,                                                                \
		.out_max = out_count,                                                              \
		.out_done = Z_FIFO_INITIALIZER(n##_if.out_done),                                   \
		.port = port_,                                                                     \
	};                                                                                         \
	USBD_DEFINE_CLASS(n, &vendor_api, &n##_if, NULL)

/*
 * The controller ends an OUT transfer at a short packet or a full buffer, and Linux's cdc-acm and
 * the WebUSB tools send no zero-length packet after a write of whole packets: their OUT buffers
 * are one packet each, or the end of such a write would wait in one for more to come. Interface 6's
 * host ends every write with a short packet (esp_sdr/mch2022_coproc.h), and gets larger ones.
 */
CDC_DEFINE(cdc_esp32, 1, 2, PORT_ESP32, "ESP32 console", 4, 1024, 8, BULK_MPS);
CDC_DEFINE(cdc_fpga, 3, 4, PORT_FPGA, "FPGA console", 4, 1024, 8, BULK_MPS);
VENDOR_DEFINE(webusb_esp32, KIND_WEBUSB, 5, PORT_ESP32, "WebUSB ESP32 console", 2, 512, 8, BULK_MPS);
VENDOR_DEFINE(webusb_fpga, KIND_WEBUSB, 6, PORT_FPGA, "WebUSB FPGA console", 2, 512, 8, BULK_MPS);
VENDOR_DEFINE(esdr_link, KIND_LINK, 7, -1, "ESDR link", 4, 2048, 2, 1024);

struct vendor_if *const vendor_cdc[PORT_COUNT] = {&cdc_esp32_if, &cdc_fpga_if};
struct vendor_if *const vendor_webusb[PORT_COUNT] = {&webusb_esp32_if, &webusb_fpga_if};
struct vendor_if *const vendor_link = &esdr_link_if;

const char *const vendor_class_names[VENDOR_CLASSES] = {
	"cdc_esp32", "cdc_fpga", "webusb_esp32", "webusb_fpga", "esdr_link",
};
