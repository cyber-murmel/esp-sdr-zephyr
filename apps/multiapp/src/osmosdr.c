/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osmosdr's vendor USB function lives in the host: apps/osmosdr/src/usb_sdr.c
 * is built into the image, as USBD_DEFINE_CLASS() and its buffer pool need
 * the host's sections. Its handlers (usb_sdr.h) are osmosdr's own, found in
 * the loaded app by name; without it, transfers end and requests stall.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "apps.h"
#include "usb_sdr.h"

/*
 * The loaded app's handlers, found once it is there and dropped as it unloads
 * (multiapp_osmosdr_drop()): the next app has its own addresses. Always read and written with
 * the atomics below: the drop runs on another thread than the forwarders.
 */
static struct {
	void (*in_done)(uint8_t *data, size_t len, int err);
	void (*out_done)(uint8_t *data, size_t len, int err);
	void (*link)(bool up);
	int (*control_out)(uint8_t req, uint16_t value, const uint8_t *data, size_t len);
	int (*control_in)(uint8_t req, uint16_t value, uint8_t *buf, size_t max);
} app;
/* How many of the forwarders below are inside the app: all of them run on the USB thread. */
static atomic_t calls;

/* The loaded app's handler @p name, looked up once it is there; NULL if there is none. */
#define HANDLER(field, name)                                                                       \
	({                                                                                         \
		__typeof__(app.field) fn_ = __atomic_load_n(&app.field, __ATOMIC_SEQ_CST);         \
		if (fn_ == NULL) {                                                                 \
			fn_ = multiapp_app_fn(name);                                               \
			__atomic_store_n(&app.field, fn_, __ATOMIC_SEQ_CST);                       \
		}                                                                                  \
		fn_;                                                                               \
	})

/* Every cached handler gone. */
static void forget(void)
{
	__atomic_store_n(&app.in_done, NULL, __ATOMIC_SEQ_CST);
	__atomic_store_n(&app.out_done, NULL, __ATOMIC_SEQ_CST);
	__atomic_store_n(&app.link, NULL, __ATOMIC_SEQ_CST);
	__atomic_store_n(&app.control_out, NULL, __ATOMIC_SEQ_CST);
	__atomic_store_n(&app.control_in, NULL, __ATOMIC_SEQ_CST);
}

/*
 * The caller has made multiapp_app_fn() return NULL. Forget first, so that a forwarder that
 * counts itself in from now on (it counts, then reads the cache) finds nothing; then wait for the
 * ones that were in already (one may have been looking up, so forget again after they left).
 */
int multiapp_osmosdr_drop(void)
{
	forget();
	for (int ms = 0; atomic_get(&calls) != 0; ms++) {
		if (ms == 1000) {
			return -EBUSY;
		}
		k_msleep(1);
	}
	forget();
	return 0;
}

void usb_sdr_in_done(uint8_t *data, size_t len, int err)
{
	__typeof__(app.in_done) fn;

	atomic_inc(&calls);
	fn = HANDLER(in_done, "usb_sdr_in_done");
	if (fn != NULL) {
		fn(data, len, err);
	}
	atomic_dec(&calls);
}

void usb_sdr_out_done(uint8_t *data, size_t len, int err)
{
	__typeof__(app.out_done) fn;

	atomic_inc(&calls);
	fn = HANDLER(out_done, "usb_sdr_out_done");
	if (fn != NULL) {
		fn(data, len, err);
	}
	atomic_dec(&calls);
}

void usb_sdr_link(bool up)
{
	__typeof__(app.link) fn;

	atomic_inc(&calls);
	fn = HANDLER(link, "usb_sdr_link");
	if (fn != NULL) {
		fn(up);
	}
	atomic_dec(&calls);
}

int usb_sdr_control_out(uint8_t req, uint16_t value, const uint8_t *data, size_t len)
{
	__typeof__(app.control_out) fn;
	int ret = -ENOTSUP;

	atomic_inc(&calls);
	fn = HANDLER(control_out, "usb_sdr_control_out");
	if (fn != NULL) {
		ret = fn(req, value, data, len);
	}
	atomic_dec(&calls);
	return ret;
}

int usb_sdr_control_in(uint8_t req, uint16_t value, uint8_t *buf, size_t max)
{
	__typeof__(app.control_in) fn;
	int ret = -ENOTSUP;

	atomic_inc(&calls);
	fn = HANDLER(control_in, "usb_sdr_control_in");
	if (fn != NULL) {
		ret = fn(req, value, buf, max);
	}
	atomic_dec(&calls);
	return ret;
}
