/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In front of an app that brings the USB device up itself, as standalone (osmosdr_hs): the host's
 * device instead (src/apps.c). sample_usbd_init_device() hands out the host's context once its
 * USB is up and takes the app's message callback, which the host calls after its own
 * (CONFIG_APP_USB_MSG_HOOK); usbd_enable() finds the device enabled.
 */

#ifndef MULTIAPP_USBD_PRELUDE_H_
#define MULTIAPP_USBD_PRELUDE_H_

#include <zephyr/usb/usbd.h>

struct usbd_context *multiapp_usbd(usbd_msg_cb_t msg_cb);
int multiapp_usbd_enable(struct usbd_context *ctx);

#define sample_usbd_init_device(msg_cb) multiapp_usbd(msg_cb)
#define usbd_enable(ctx)                multiapp_usbd_enable(ctx)

#endif /* MULTIAPP_USBD_PRELUDE_H_ */
