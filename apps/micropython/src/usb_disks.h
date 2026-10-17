/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef USB_DISKS_H_
#define USB_DISKS_H_

#include <stdbool.h>

/*
 * Takes the volume at @p mnt ("/FLASH:" or "/SD:") from the USB host before the board writes
 * it: the host sees the medium removed, and the board re-reads the volume (the host may
 * have changed it). 0, -ENOENT for an unknown mount point, -EIO if the volume did not mount.
 */
int usb_disk_claim(const char *mnt);

/*
 * Gives the volume back to the host once the board's files on it are closed: syncs the disk
 * and shows the medium again, at the earliest some seconds after the claim so that the host's
 * media poll has seen it gone.
 */
int usb_disk_release(const char *mnt);

/* Whether the host sees the volume at @p mnt. */
bool usb_disk_shared(const char *mnt);

#endif /* USB_DISKS_H_ */
