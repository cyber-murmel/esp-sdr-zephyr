/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_usb.h. Modelled on apps/osmosdr/src/usb_sdr.c.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>

#include <esp_sdr/esp_sdr.h>

#include "app_usb.h"
#include "ubt_ctrl.h"
#include "ubt_usb.h"

LOG_MODULE_REGISTER(ubt_usb, LOG_LEVEL_INF);

#define BULK_IN_EP  0x82U
#define BULK_OUT_EP 0x05U
#define PKT_SIZE    sizeof(usb_pkt_rx)

/* Packets in flight on the bulk endpoint. */
#define INFLIGHT 4U

/* The largest data stage of a request to the device (upstream's abVendorReqData). */
#define REQ_DATA_MAX 258U

UDC_BUF_POOL_DEFINE(ubt_ep_pool, INFLIGHT, 0, sizeof(struct udc_buf_info), NULL);

/* Every request the tool library sends (upstream ubertooth_interface.h, 0 to 73 without the unused 64). */
static const struct usbd_cctx_vendor_req ubt_vregs = USBD_VENDOR_REQ(
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
	26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
	49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 65, 66, 67, 68, 69, 70, 71, 72,
	73);

#ifdef USBD_EP_ADDR_FIXED
#define IN_EP_FLAGGED  (BULK_IN_EP | USBD_EP_ADDR_FIXED)
#define OUT_EP_FLAGGED (BULK_OUT_EP | USBD_EP_ADDR_FIXED)
#else
/* Without the fixed address flag the first free IN endpoint is 0x81: park an interrupt IN there. */
#define IN_EP_FLAGGED  BULK_IN_EP
#define OUT_EP_FLAGGED BULK_OUT_EP
#endif

struct ubt_desc {
	struct usb_if_descriptor if0;
#ifndef USBD_EP_ADDR_FIXED
	struct usb_ep_descriptor dummy_ep;
#endif
	struct usb_ep_descriptor in_ep;
	struct usb_ep_descriptor out_ep;
	struct usb_desc_header nil_desc;
};

static struct ubt_desc ubt_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
#ifdef USBD_EP_ADDR_FIXED
		.bNumEndpoints = 2,
#else
		.bNumEndpoints = 3,
#endif
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
#ifndef USBD_EP_ADDR_FIXED
	.dummy_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_INTERRUPT,
		.wMaxPacketSize = sys_cpu_to_le16(8U),
		.bInterval = 255,
	},
#endif
	.in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = IN_EP_FLAGGED,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64U),
		.bInterval = 0,
	},
	.out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = OUT_EP_FLAGGED,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64U),
		.bInterval = 0,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *const ubt_fs_desc[] = {
	(struct usb_desc_header *)&ubt_desc.if0,
#ifndef USBD_EP_ADDR_FIXED
	(struct usb_desc_header *)&ubt_desc.dummy_ep,
#endif
	(struct usb_desc_header *)&ubt_desc.in_ep,
	(struct usb_desc_header *)&ubt_desc.out_ep,
	(struct usb_desc_header *)&ubt_desc.nil_desc,
};

static struct ubt_dev *dev;
static struct usbd_class_data *ubt_c_data;
static atomic_t up;
/* Bit i: stage[i] is on the bulk endpoint. */
static atomic_t busy;
/* USB DMA buffers; internal RAM below the ring banks is full, so they live in the high region. */
static struct {
	uint8_t stage[INFLIGHT][64];
	uint8_t req_data[(REQ_DATA_MAX + 3U) & ~3U];
} usb_high __aligned(4) ESP_SDR_HIGH_RAM;
#define stage    usb_high.stage
#define req_data usb_high.req_data
static struct k_work tx_work;
static struct k_work_delayable keepalive_work;
static uint32_t last_tx_clkn;

bool ubt_usb_enabled(void)
{
	return atomic_get(&up) != 0;
}

void ubt_usb_kick(void)
{
	k_work_submit(&tx_work);
}

static void arm_fixed_flags(void)
{
#ifdef USBD_EP_ADDR_FIXED
	ubt_desc.in_ep.bEndpointAddress = IN_EP_FLAGGED;
	ubt_desc.out_ep.bEndpointAddress = OUT_EP_FLAGGED;
#endif
}

/* Queue @p len bytes of stage[@p slot] on bulk IN. */
static int submit(unsigned int slot, size_t len)
{
	struct net_buf *buf;
	struct udc_buf_info *bi;
	int err;

	buf = net_buf_alloc_with_data(&ubt_ep_pool, stage[slot], len, K_NO_WAIT);
	if (buf == NULL) {
		return -ENOMEM;
	}
	bi = udc_get_buf_info(buf);
	memset(bi, 0, sizeof(*bi));
	bi->ep = ubt_desc.in_ep.bEndpointAddress;
	err = usbd_ep_enqueue(ubt_c_data, buf);
	if (err != 0) {
		net_buf_unref(buf);
	}
	return err;
}

/*
 * Packets the bulk endpoint may hold. The LE modes are read with POLL by the tools; as on the real
 * device (two endpoint buffers) the first packets are lost to a bulk endpoint nobody reads, so
 * keep that to two there.
 */
static unsigned int inflight_limit(void)
{
	return dev->mode == MODE_BT_FOLLOW_LE || dev->mode == MODE_BT_PROMISC_LE ? 2U : INFLIGHT;
}

static int free_slot(void)
{
	unsigned int used = 0;
	int free = -1;

	for (unsigned int i = 0; i < INFLIGHT; i++) {
		if (!atomic_test_bit(&busy, i)) {
			free = free < 0 ? (int)i : free;
		} else {
			used++;
		}
	}
	return used < inflight_limit() ? free : -1;
}

static void tx_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	while (ubt_usb_enabled()) {
		int slot = free_slot();
		usb_pkt_rx pkt;

		if (slot < 0) {
			return;
		}
		if (!ubt_queue_take(&dev->queue, &pkt)) {
			return;
		}
		memcpy(stage[slot], &pkt, PKT_SIZE);
		atomic_set_bit(&busy, (unsigned int)slot);
		if (submit((unsigned int)slot, PKT_SIZE) != 0) {
			atomic_clear_bit(&busy, (unsigned int)slot);
			return;
		}
		last_tx_clkn = ubt_clock_clkn(&dev->clock, dev->plat->cycles());
	}
}

/* A 1 byte KEEP_ALIVE after UBT_KEEP_ALIVE_TICKS without a packet, while a mode runs (upstream dequeue_send()). */
static void keepalive_fn(struct k_work *work)
{
	uint32_t clkn = ubt_clock_clkn(&dev->clock, dev->plat->cycles());
	int slot;

	k_work_reschedule(&keepalive_work, K_SECONDS(5));
	if (!ubt_usb_enabled() || dev->mode == MODE_IDLE ||
	    clkn - last_tx_clkn < UBT_KEEP_ALIVE_TICKS || ubt_queue_count(&dev->queue) != 0U) {
		return;
	}
	slot = free_slot();
	if (slot < 0) {
		return;
	}
	stage[slot][0] = KEEP_ALIVE;
	atomic_set_bit(&busy, (unsigned int)slot);
	if (submit((unsigned int)slot, 1U) != 0) {
		atomic_clear_bit(&busy, (unsigned int)slot);
		return;
	}
	last_tx_clkn = clkn;
}

static int ubt_request(struct usbd_class_data *const c_data, struct net_buf *buf, int err)
{
	const uint8_t *data = buf->__buf;
	unsigned int slot = (unsigned int)((data - &stage[0][0]) / sizeof(stage[0]));

	ARG_UNUSED(c_data);
	net_buf_unref(buf);
	if (slot < INFLIGHT) {
		atomic_clear_bit(&busy, slot);
	}
	if (err != 0 && err != -ECONNABORTED) {
		LOG_WRN("bulk IN failed (%d)", err);
	}
	k_work_submit(&tx_work);
	return 0;
}

static struct net_buf *ubt_control_to_host(struct usbd_class_data *c_data,
					   const struct usb_setup_packet *const setup)
{
	struct ubt_req r = {
		.request = setup->bRequest,
		.in = true,
		.value = setup->wValue,
		.index = setup->wIndex,
		.length = setup->wLength,
	};
	struct net_buf *buf;
	int len;

	if (setup->RequestType.recipient != USB_REQTYPE_RECIPIENT_DEVICE) {
		return NULL;
	}
	if (setup->wLength == 0U) {
		/* Nothing goes back; the stack answers with the status stage. */
		(void)ubt_ctrl(dev, &r, req_data, 0);
		return NULL;
	}
	buf = usbd_ep_ctrl_data_in_alloc(usbd_class_get_ctx(c_data), setup->wLength);
	if (buf == NULL) {
		return NULL;
	}
	len = ubt_ctrl(dev, &r, buf->data, MIN(setup->wLength, net_buf_tailroom(buf)));
	if (len < 0) {
		LOG_DBG("request %u to host fails (%d)", setup->bRequest, len);
		net_buf_unref(buf);
		return NULL;
	}
	net_buf_add(buf, (size_t)len);
	return buf;
}

static int ubt_control_to_dev(struct usbd_class_data *c_data,
			      const struct usb_setup_packet *const setup,
			      const struct net_buf *const buf)
{
	struct ubt_req r = {
		.request = setup->bRequest,
		.in = false,
		.value = setup->wValue,
		.index = setup->wIndex,
		.length = setup->wLength,
	};
	size_t len = 0;
	int ret;

	ARG_UNUSED(c_data);
	if (setup->RequestType.recipient != USB_REQTYPE_RECIPIENT_DEVICE) {
		return -ENOTSUP;
	}
	if (setup->wLength != 0U) {
		if (buf == NULL) {
			/* Setup stage: take the data stage; the request runs when it arrived. */
			return setup->wLength <= REQ_DATA_MAX ? 0 : -EINVAL;
		}
		len = MIN(buf->len, REQ_DATA_MAX);
		memcpy(req_data, buf->data, len);
	}
	ret = ubt_ctrl(dev, &r, req_data, len);
	if (ret < 0) {
		LOG_DBG("request %u to device fails (%d)", setup->bRequest, ret);
		return ret;
	}
	return 0;
}

static const void *ubt_get_desc(struct usbd_class_data *const c_data, const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
	ARG_UNUSED(speed);
	return ubt_fs_desc;
}

static void ubt_enable(struct usbd_class_data *const c_data)
{
	ubt_c_data = c_data;
	atomic_clear(&busy);
	atomic_set(&up, 1);
	LOG_INF("interface enabled (in 0x%02x, out 0x%02x)", ubt_desc.in_ep.bEndpointAddress,
		ubt_desc.out_ep.bEndpointAddress);
	k_work_reschedule(&keepalive_work, K_SECONDS(5));
	ubt_usb_kick();
}

static void ubt_disable(struct usbd_class_data *const c_data)
{
	atomic_set(&up, 0);
	k_work_cancel_delayable(&keepalive_work);
	(void)usbd_ep_dequeue(usbd_class_get_ctx(c_data), ubt_desc.in_ep.bEndpointAddress);
	LOG_INF("interface disabled");
}

static int ubt_init(struct usbd_class_data *c_data)
{
	ubt_c_data = c_data;
	return 0;
}

static void ubt_shutdown(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	/* The assignment cleared the flags: a new initialization needs them again. */
	arm_fixed_flags();
}

static const struct usbd_class_api ubt_api = {
	.request = ubt_request,
	.control_to_host = ubt_control_to_host,
	.control_to_dev = ubt_control_to_dev,
	.get_desc = ubt_get_desc,
	.enable = ubt_enable,
	.disable = ubt_disable,
	.init = ubt_init,
	.shutdown = ubt_shutdown,
};

USBD_DEFINE_CLASS(ubertooth, &ubt_api, NULL, &ubt_vregs);

void ubt_usb_init(struct ubt_dev *d)
{
	dev = d;
	k_work_init(&tx_work, tx_work_fn);
	k_work_init_delayable(&keepalive_work, keepalive_fn);
}

/*
 * libubertooth claims interface 0: the vendor class goes first, the shell and DFU behind it (the
 * sample registers the classes in name order). The device version is the API version the tools check.
 */
void app_usb_classes(struct usbd_context *ctx)
{
	(void)usbd_unregister_class(ctx, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (IS_ENABLED(CONFIG_APP_USB_DFU)) {
		(void)usbd_unregister_class(ctx, "dfu_runtime", USBD_SPEED_FS, 1);
	}
	(void)usbd_register_class(ctx, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (IS_ENABLED(CONFIG_APP_USB_DFU)) {
		(void)usbd_register_class(ctx, "dfu_runtime", USBD_SPEED_FS, 1);
	}
	(void)usbd_device_set_bcd_device(ctx, CONFIG_APP_UBERTOOTH_API_VERSION);
}
