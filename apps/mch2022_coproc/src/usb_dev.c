/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The USB device: the stock firmware's identity, so that everything finds the badge where it did
 * (usb_descriptors.c there), and its interfaces in their order, then the new one. The binary
 * object store as there: WebUSB's landing page, and WinUSB for the vendor interfaces.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/usb/usbd.h>

#include "coproc.h"

LOG_MODULE_REGISTER(usb_dev, LOG_LEVEL_INF);

#define VENDOR_CODE_WEBUSB 0x01U
#define VENDOR_CODE_MSOS2  0x02U
#define LANDING_PAGE       0x01U
#define MSOS2_DESC_INDEX   0x07U
/* As the stock firmware's descriptor set. */
#define MSOS2_WINDOWS      0x06030000UL

USBD_DEVICE_DEFINE(coproc_usbd_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x16D0, 0x0F9A);
struct usbd_context *const coproc_usbd = &coproc_usbd_ctx;

USBD_DESC_LANG_DEFINE(desc_lang);
USBD_DESC_MANUFACTURER_DEFINE(desc_mfr, "Badge.team");
USBD_DESC_PRODUCT_DEFINE(desc_product, "MCH2022 badge");
USBD_DESC_SERIAL_NUMBER_DEFINE(desc_sn);
USBD_CONFIGURATION_DEFINE(desc_config, 0, 50, NULL);

/* ---- WebUSB ---- */

struct bos_webusb {
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_webusb cap;
} __packed;

static const struct bos_webusb bos_webusb = {
	.platform = {
		.bLength = sizeof(struct usb_bos_platform_descriptor) +
			   sizeof(struct usb_bos_capability_webusb),
		.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
		/* 3408b638-09a9-47a0-8bfd-a0768815b665 */
		.PlatformCapabilityUUID = {0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47, 0x8B,
					   0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65},
	},
	.cap = {
		.bcdVersion = sys_cpu_to_le16(0x0100),
		.bVendorCode = VENDOR_CODE_WEBUSB,
		.iLandingPage = LANDING_PAGE,
	},
};

/* bLength, the URL's type, https, the URL. */
static const uint8_t webusb_url[] = {
	3 + 18, 0x03, 0x01,
	'm', 'c', 'h', '2', '0', '2', '2', '.', 'b', 'a', 'd', 'g', 'e', '.', 't', 'e', 'a', 'm',
};

static struct net_buf *webusb_vreq(const struct usbd_context *const ctx,
				   const struct usb_setup_packet *const setup)
{
	uint16_t len = MIN(setup->wLength, sizeof(webusb_url));
	struct net_buf *buf = usbd_ep_ctrl_data_in_alloc(ctx, len);

	if (buf != NULL) {
		net_buf_add_mem(buf, webusb_url, len);
	}
	return buf;
}

USBD_DESC_BOS_VREQ_DEFINE(desc_bos_webusb, sizeof(bos_webusb), &bos_webusb, VENDOR_CODE_WEBUSB,
			  webusb_vreq, NULL);

/* ---- Microsoft OS 2.0 ---- */

#define GUID_CHARS(c) c, 0x00
/* The stock firmware's, for its two interfaces. {975F44D9-0D08-43FD-8B3E-127CA8AFFF9D} */
#define GUID_WEBUSB                                                                                \
	'{', 0, '9', 0, '7', 0, '5', 0, 'F', 0, '4', 0, '4', 0, 'D', 0, '9', 0, '-', 0, '0', 0,    \
	'D', 0, '0', 0, '8', 0, '-', 0, '4', 0, '3', 0, 'F', 0, 'D', 0, '-', 0, '8', 0, 'B', 0,    \
	'3', 0, 'E', 0, '-', 0, '1', 0, '2', 0, '7', 0, 'C', 0, 'A', 0, '8', 0, 'A', 0, 'F', 0,    \
	'F', 0, 'F', 0, '9', 0, 'D', 0, '}', 0, 0, 0, 0, 0
/* Interface 6's own. {6D2E1C74-3A8B-4F05-9E61-B7C2D4F8A013} */
#define GUID_LINK                                                                                  \
	'{', 0, '6', 0, 'D', 0, '2', 0, 'E', 0, '1', 0, 'C', 0, '7', 0, '4', 0, '-', 0, '3', 0,    \
	'A', 0, '8', 0, 'B', 0, '-', 0, '4', 0, 'F', 0, '0', 0, '5', 0, '-', 0, '9', 0, 'E', 0,    \
	'6', 0, '1', 0, '-', 0, 'B', 0, '7', 0, 'C', 0, '2', 0, 'D', 0, '4', 0, 'F', 0, '8', 0,    \
	'A', 0, '0', 0, '1', 0, '3', 0, '}', 0, 0, 0, 0, 0

struct msos2_function {
	struct msosv2_function_subset_header subset;
	struct msosv2_compatible_id id;
	struct msosv2_guids_property guids;
} __packed;

#define MSOS2_FUNCTION(first, ...)                                                                 \
	{                                                                                          \
		.subset = {                                                                        \
			.wLength = sys_cpu_to_le16(sizeof(struct msosv2_function_subset_header)),  \
			.wDescriptorType = sys_cpu_to_le16(MS_OS_20_SUBSET_HEADER_FUNCTION),       \
			.bFirstInterface = (first),                                                \
			.wSubsetLength = sys_cpu_to_le16(sizeof(struct msos2_function)),           \
		},                                                                                 \
		.id = {                                                                            \
			.wLength = sys_cpu_to_le16(sizeof(struct msosv2_compatible_id)),           \
			.wDescriptorType = sys_cpu_to_le16(MS_OS_20_FEATURE_COMPATIBLE_ID),        \
			.CompatibleID = {'W', 'I', 'N', 'U', 'S', 'B', 0, 0},                      \
		},                                                                                 \
		.guids = {                                                                         \
			.wLength = sys_cpu_to_le16(sizeof(struct msosv2_guids_property)),          \
			.wDescriptorType = sys_cpu_to_le16(MS_OS_20_FEATURE_REG_PROPERTY),         \
			.wPropertyDataType = sys_cpu_to_le16(MS_OS_20_PROPERTY_DATA_REG_MULTI_SZ), \
			.wPropertyNameLength = sys_cpu_to_le16(42),                                \
			.PropertyName = {DEVICE_INTERFACE_GUIDS_PROPERTY_NAME},                    \
			.wPropertyDataLength = sys_cpu_to_le16(80),                                \
			.bPropertyData = {__VA_ARGS__},                                            \
		},                                                                                 \
	}

struct msos2_set {
	struct msosv2_descriptor_set_header header;
	struct msosv2_configuration_subset_header config;
	struct msos2_function functions[3];
} __packed;

static const struct msos2_set msos2_set = {
	.header = {
		.wLength = sys_cpu_to_le16(sizeof(struct msosv2_descriptor_set_header)),
		.wDescriptorType = sys_cpu_to_le16(MS_OS_20_SET_HEADER_DESCRIPTOR),
		.dwWindowsVersion = sys_cpu_to_le32(MSOS2_WINDOWS),
		.wTotalLength = sys_cpu_to_le16(sizeof(struct msos2_set)),
	},
	.config = {
		.wLength = sys_cpu_to_le16(sizeof(struct msosv2_configuration_subset_header)),
		.wDescriptorType = sys_cpu_to_le16(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
		.bConfigurationValue = 0,
		.wTotalLength = sys_cpu_to_le16(sizeof(struct msos2_set) -
						sizeof(struct msosv2_descriptor_set_header)),
	},
	.functions = {
		MSOS2_FUNCTION(COPROC_IF_WEBUSB_ESP32, GUID_WEBUSB),
		MSOS2_FUNCTION(COPROC_IF_WEBUSB_FPGA, GUID_WEBUSB),
		MSOS2_FUNCTION(COPROC_IF_LINK, GUID_LINK),
	},
};

BUILD_ASSERT(sizeof(struct msos2_function) == 160, "the stock firmware's function subset");
BUILD_ASSERT(sizeof(struct msos2_set) == 498, "three functions");

struct bos_msos2 {
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_msos cap;
} __packed;

static const struct bos_msos2 bos_msos2 = {
	.platform = {
		.bLength = sizeof(struct usb_bos_platform_descriptor) +
			   sizeof(struct usb_bos_capability_msos),
		.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
		/* D8DD60DF-4589-4CC7-9CD2-659D9E648A9F */
		.PlatformCapabilityUUID = {0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C,
					   0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F},
	},
	.cap = {
		.dwWindowsVersion = sys_cpu_to_le32(MSOS2_WINDOWS),
		.wMSOSDescriptorSetTotalLength = sys_cpu_to_le16(sizeof(struct msos2_set)),
		.bMS_VendorCode = VENDOR_CODE_MSOS2,
		.bAltEnumCode = 0,
	},
};

static struct net_buf *msos2_vreq(const struct usbd_context *const ctx,
				  const struct usb_setup_packet *const setup)
{
	uint16_t len;
	struct net_buf *buf;

	if (setup->wIndex != MSOS2_DESC_INDEX) {
		return NULL;
	}
	len = MIN(setup->wLength, sizeof(msos2_set));
	buf = usbd_ep_ctrl_data_in_alloc(ctx, len);
	if (buf != NULL) {
		net_buf_add_mem(buf, &msos2_set, len);
	}
	return buf;
}

USBD_DESC_BOS_VREQ_DEFINE(desc_bos_msos2, sizeof(bos_msos2), &bos_msos2, VENDOR_CODE_MSOS2,
			  msos2_vreq, NULL);

/* ---- messages ---- */

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	static bool mounted, suspended;

	switch (msg->type) {
	case USBD_MSG_CONFIGURATION:
		mounted = msg->status != 0;
		bridge_usb_up(mounted);
		break;
	case USBD_MSG_RESET:
		mounted = false;
		suspended = false;
		bridge_usb_up(false);
		break;
	case USBD_MSG_SUSPEND:
		suspended = true;
		break;
	case USBD_MSG_RESUME:
		suspended = false;
		break;
	default:
		return;
	}
	regs_usb_state(mounted, suspended, ctx->status.rwup);
}

int usb_dev_init(void)
{
	struct usbd_context *ctx = coproc_usbd;
	struct usbd_desc_node *const desc[] = {
		&desc_lang, &desc_mfr, &desc_product, &desc_sn, &desc_bos_webusb, &desc_bos_msos2,
	};
	int ret;

	ARRAY_FOR_EACH(desc, k) {
		ret = usbd_add_descriptor(ctx, desc[k]);
		if (ret != 0) {
			return ret;
		}
	}
	ret = usbd_add_configuration(ctx, USBD_SPEED_FS, &desc_config);
	if (ret != 0) {
		return ret;
	}
	/* One by one: the order gives the interface numbers. */
	for (int k = 0; k < VENDOR_CLASSES; k++) {
		ret = usbd_register_class(ctx, vendor_class_names[k], USBD_SPEED_FS, 1);
		if (ret != 0) {
			LOG_ERR("class %s: %d", vendor_class_names[k], ret);
			return ret;
		}
	}
	(void)usbd_device_set_code_triple(ctx, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	(void)usbd_device_set_bcd_usb(ctx, USBD_SPEED_FS, 0x0210);
	/* One interface more than the stock firmware's 0x0100, which Windows may have cached. */
	(void)usbd_device_set_bcd_device(ctx, 0x0200);
	ret = usbd_msg_register_cb(ctx, msg_cb);
	if (ret != 0) {
		return ret;
	}
	ret = usbd_init(ctx);
	if (ret != 0) {
		return ret;
	}
	return usbd_enable(ctx);
}
