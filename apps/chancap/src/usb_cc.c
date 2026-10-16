/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Vendor USB function of the capture stream (usb_cc.h): one interface, bulk IN. Transfers
 * point at the app's own blocks (net_buf_alloc_with_data), so nothing is copied here. After
 * apps/osmosdr/src/usb_sdr.c, without its OUT endpoint and control requests: the shell sets
 * the radio.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>

#include "usb_cc.h"

LOG_MODULE_REGISTER(usb_cc, LOG_LEVEL_INF);

/* Bulk transfers in flight at most. */
#define CC_XFERS 8

UDC_BUF_POOL_DEFINE(cc_ep_pool, CC_XFERS, 0, sizeof(struct udc_buf_info), NULL);

struct cc_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor in_ep;
	struct usb_desc_header nil_desc;
};

static struct cc_desc cc_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64U),
		.bInterval = 0,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *const cc_fs_desc[] = {
	(struct usb_desc_header *)&cc_desc.if0,
	(struct usb_desc_header *)&cc_desc.in_ep,
	(struct usb_desc_header *)&cc_desc.nil_desc,
};

static struct usbd_class_data *cc_c_data;
static atomic_t cc_up;

bool usb_cc_enabled(void)
{
	return atomic_get(&cc_up) != 0;
}

int usb_cc_submit_in(uint8_t *data, size_t len)
{
	struct net_buf *buf;
	struct udc_buf_info *bi;
	int err;

	if (!usb_cc_enabled()) {
		return -ENOTCONN;
	}
	buf = net_buf_alloc_with_data(&cc_ep_pool, data, len, K_NO_WAIT);
	if (buf == NULL) {
		return -ENOMEM;
	}
	bi = udc_get_buf_info(buf);
	memset(bi, 0, sizeof(*bi));
	bi->ep = cc_desc.in_ep.bEndpointAddress;
	/* A whole number of 64-byte packets: end it with a zero-length packet. */
	bi->zlp = (len % 64U) == 0U;
	err = usbd_ep_enqueue(cc_c_data, buf);
	if (err != 0) {
		net_buf_unref(buf);
	}
	return err;
}

void usb_cc_cancel(void)
{
	if (cc_c_data == NULL) {
		return;
	}
	(void)usbd_ep_dequeue(usbd_class_get_ctx(cc_c_data), cc_desc.in_ep.bEndpointAddress);
}

static int cc_request(struct usbd_class_data *const c_data, struct net_buf *buf, int err)
{
	struct udc_buf_info *bi = udc_get_buf_info(buf);
	uint8_t *data = buf->__buf;
	size_t len = buf->len;
	uint8_t ep = bi->ep;

	ARG_UNUSED(c_data);
	net_buf_unref(buf);
	if (ep == cc_desc.in_ep.bEndpointAddress) {
		usb_cc_in_done(data, len, err);
	}
	return 0;
}

static const void *cc_get_desc(struct usbd_class_data *const c_data, const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
	ARG_UNUSED(speed);
	return cc_fs_desc;
}

static void cc_enable(struct usbd_class_data *const c_data)
{
	cc_c_data = c_data;
	atomic_set(&cc_up, 1);
	LOG_INF("interface enabled (in 0x%02x)", cc_desc.in_ep.bEndpointAddress);
	usb_cc_link(true);
}

static void cc_disable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	atomic_set(&cc_up, 0);
	LOG_INF("interface disabled");
	usb_cc_link(false);
}

static int cc_init(struct usbd_class_data *c_data)
{
	cc_c_data = c_data;
	return 0;
}

static const struct usbd_class_api cc_api = {
	.request = cc_request,
	.get_desc = cc_get_desc,
	.enable = cc_enable,
	.disable = cc_disable,
	.init = cc_init,
};

USBD_DEFINE_CLASS(esp_sdr_cc, &cc_api, NULL, NULL);
