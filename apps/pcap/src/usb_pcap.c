/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pcap stream's USB function (usb_pcap.h): one vendor interface with a bulk IN endpoint,
 * 512-byte packets at high speed (the ESP32-P4's OTG HS port). Transfers point at the app's own
 * buffers (net_buf_alloc_with_data), so no data is copied here. Control requests to the device
 * are claimed by bRequest and passed to the app; after ../../osmosdr/src/usb_sdr.c.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>

#include "esdr_proto.h"
#include "pcap_proto.h"
#include "usb_pcap.h"

LOG_MODULE_REGISTER(usb_pcap, LOG_LEVEL_INF);

/* Bulk IN transfers in flight. */
#define PCAP_XFERS 8

UDC_BUF_POOL_DEFINE(pcap_ep_pool, PCAP_XFERS, 0, sizeof(struct udc_buf_info), NULL);

/* Every request the radio answers (esdr_proto.h), and this app's own. */
static const struct usbd_cctx_vendor_req pcap_vregs = USBD_VENDOR_REQ(
	ESDR_REQ_GET_INFO, ESDR_REQ_SET_MODE, ESDR_REQ_SET_FREQ, ESDR_REQ_SET_SAMPLE_RATE,
	ESDR_REQ_SET_RX_GAIN, ESDR_REQ_SET_TX_GAIN, ESDR_REQ_SET_BANDWIDTH, ESDR_REQ_SET_FORMAT,
	ESDR_REQ_SET_FREQ_CORR, ESDR_REQ_GET_STATE, ESDR_REQ_GET_STATS, ESDR_REQ_SET_OPTIONS,
	ESDR_REQ_SET_DIGITAL_GAIN, ESDR_REQ_RESET, ESDR_REQ_SET_GAPLESS, ESDR_REQ_GET_GAPLESS_STATS,
	ESDR_REQ_SET_CHANNELIZER, ESDR_REQ_USB_TO_C6, ESDR_REQ_GET_TEMP, ESDR_REQ_GET_CAL,
	ESDR_REQ_SET_CAL, PCAP_REQ_START, PCAP_REQ_STOP, PCAP_REQ_SET_TIME, PCAP_REQ_SET_DECODERS,
	PCAP_REQ_GET_STATS, PCAP_REQ_ADD_AA, PCAP_REQ_SET_MODE);

struct pcap_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor in_ep;
	struct usb_ep_descriptor hs_in_ep;
	struct usb_desc_header nil_desc;
};

static struct pcap_desc pcap_desc = {
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
	.hs_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(512U),
		.bInterval = 0,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *const pcap_fs_desc[] = {
	(struct usb_desc_header *)&pcap_desc.if0,
	(struct usb_desc_header *)&pcap_desc.in_ep,
	(struct usb_desc_header *)&pcap_desc.nil_desc,
};

static const struct usb_desc_header *const pcap_hs_desc[] = {
	(struct usb_desc_header *)&pcap_desc.if0,
	(struct usb_desc_header *)&pcap_desc.hs_in_ep,
	(struct usb_desc_header *)&pcap_desc.nil_desc,
};

static struct usbd_class_data *pcap_c_data;
static atomic_t pcap_up;

bool usb_pcap_enabled(void)
{
	return atomic_get(&pcap_up) != 0;
}

int usb_pcap_submit(uint8_t *data, size_t len)
{
	struct net_buf *buf;
	struct udc_buf_info *bi;
	int err;

	if (!usb_pcap_enabled()) {
		return -ENOTCONN;
	}
	buf = net_buf_alloc_with_data(&pcap_ep_pool, data, len, K_NO_WAIT);
	if (buf == NULL) {
		return -ENOMEM;
	}
	bi = udc_get_buf_info(buf);
	memset(bi, 0, sizeof(*bi));
	bi->ep = pcap_desc.in_ep.bEndpointAddress;
	/* Every transfer ends the host's read: a whole number of packets with a zero-length one. */
	bi->zlp = (len % 512U) == 0U;
	err = usbd_ep_enqueue(pcap_c_data, buf);
	if (err != 0) {
		net_buf_unref(buf);
	}
	return err;
}

static int pcap_request(struct usbd_class_data *const c_data, struct net_buf *buf, int err)
{
	struct udc_buf_info *bi = udc_get_buf_info(buf);
	uint8_t *data = buf->__buf;
	size_t len = buf->len;
	uint8_t ep = bi->ep;

	ARG_UNUSED(c_data);
	net_buf_unref(buf);
	if (ep == pcap_desc.in_ep.bEndpointAddress) {
		usb_pcap_in_done(data, len, err);
	}
	return 0;
}

static struct net_buf *pcap_control_to_host(struct usbd_class_data *c_data,
					    const struct usb_setup_packet *const setup)
{
	struct net_buf *buf;
	int len;

	if (setup->RequestType.recipient != USB_REQTYPE_RECIPIENT_DEVICE) {
		return NULL;
	}
	buf = usbd_ep_ctrl_data_in_alloc(usbd_class_get_ctx(c_data), setup->wLength);
	if (buf == NULL) {
		return NULL;
	}
	len = usb_pcap_control_in(setup->bRequest, setup->wValue, buf->data,
				  MIN(setup->wLength, net_buf_tailroom(buf)));
	if (len < 0) {
		net_buf_unref(buf);
		return NULL;
	}
	net_buf_add(buf, len);
	return buf;
}

static int pcap_control_to_dev(struct usbd_class_data *c_data,
			       const struct usb_setup_packet *const setup,
			       const struct net_buf *const buf)
{
	ARG_UNUSED(c_data);

	if (setup->RequestType.recipient != USB_REQTYPE_RECIPIENT_DEVICE) {
		return -ENOTSUP;
	}
	if (setup->wLength == 0U) {
		return usb_pcap_control_out(setup->bRequest, setup->wValue, NULL, 0);
	}
	if (buf == NULL) {
		/* Setup stage: accept the data stage, handled when it arrives. */
		return 0;
	}
	return usb_pcap_control_out(setup->bRequest, setup->wValue, buf->data, buf->len);
}

static const void *pcap_get_desc(struct usbd_class_data *const c_data,
				 const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
	return speed == USBD_SPEED_HS ? pcap_hs_desc : pcap_fs_desc;
}

static void pcap_enable(struct usbd_class_data *const c_data)
{
	pcap_c_data = c_data;
	atomic_set(&pcap_up, 1);
	LOG_INF("pcap interface enabled (in 0x%02x)", pcap_desc.in_ep.bEndpointAddress);
	usb_pcap_link(true);
}

static void pcap_disable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	atomic_set(&pcap_up, 0);
	LOG_INF("pcap interface disabled");
	usb_pcap_link(false);
}

static int pcap_init(struct usbd_class_data *c_data)
{
	pcap_c_data = c_data;
	return 0;
}

static const struct usbd_class_api pcap_api = {
	.request = pcap_request,
	.control_to_host = pcap_control_to_host,
	.control_to_dev = pcap_control_to_dev,
	.get_desc = pcap_get_desc,
	.enable = pcap_enable,
	.disable = pcap_disable,
	.init = pcap_init,
};

USBD_DEFINE_CLASS(esp_sdr_pcap, &pcap_api, NULL, &pcap_vregs);
