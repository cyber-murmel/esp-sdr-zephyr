/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The USB classes a boot registers, from what its app declares (the `USB` mask of
 * multiapp_extension()). Header only, so that tests/host/test_multiapp_drive.py compiles it as it is.
 */

#ifndef MULTIAPP_PROFILE_H_
#define MULTIAPP_PROFILE_H_

#include <stdbool.h>
#include <stdint.h>

#include <multiapp/multiapp.h>

/*
 * The classes of a boot with an app declaring @p declared: its own, and the host's drive, so that the
 * PC can reach the volume while the app runs. CDC-NCM (stream) leaves it out: with the shell's two IN
 * endpoints it takes all four of the ESP32-S3's OTG controller. Endpoints go to the classes in
 * registration order, first fit: CDC-ACM, DFU, the vendor class, the drive; tests/host checks, from
 * the FIFO tables, that every profile built here fits and that a profile that leaves the drive out
 * would not. An app that brings its own disks declares the drive itself (MULTIAPP_USB_OWN_DISKS).
 * @p msc_built: the host has the class (CONFIG_USBD_MSC_CLASS).
 */
static inline uint32_t multiapp_usb_profile(uint32_t declared, bool msc_built)
{
	if (msc_built && (declared & MULTIAPP_USB_NCM) == 0U) {
		declared |= MULTIAPP_USB_MSC;
	}
	return declared;
}

#endif /* MULTIAPP_PROFILE_H_ */
