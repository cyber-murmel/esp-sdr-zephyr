/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The volumes on USB: MSC LUN 0 is the internal FAT volume (disk FLASH), LUN 1 the SD card
 * (disk SD). The LUNs reach the disks through proxies that report no medium while the board
 * writes a volume (usb_disk_claim()): the host drops what it cached and reads the volume
 * afresh when it comes back. Zephyr's MSC has no other way to tell the host that the
 * medium changed (no unit attention), and two writers on one FAT corrupt it. The board's
 * own reads are not guarded: after the host changed a volume, a claim and release (or a
 * soft reset) refreshes the board's view.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/disk.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/usb/class/usbd_msc.h>

#include <ff.h>

#include "usb_disks.h"

/* Long enough for the host's media poll (udisks: every 2 s) to see the medium gone. */
#define HIDE_MIN_MS 3000

FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(flash_fs));
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(sd_fs));

struct usb_disk {
	struct disk_info info;
	const char *target;
	struct fs_mount_t *mount;
	struct k_mutex lock;
	bool shared;
	int64_t hidden_at;
};

static struct usb_disk *to_usb_disk(struct disk_info *disk)
{
	return CONTAINER_OF(disk, struct usb_disk, info);
}

/*
 * Only the board initialises the disks (mounting them): an init from the USB thread as well
 * would race it. Until the board has mounted a volume, the host sees no medium.
 */
static int proxy_init(struct disk_info *disk)
{
	ARG_UNUSED(disk);
	return 0;
}

static int proxy_status(struct disk_info *disk)
{
	struct usb_disk *u = to_usb_disk(disk);

	if (!u->shared) {
		return DISK_STATUS_NOMEDIA;
	}
	return disk_access_status(u->target);
}

static int proxy_read(struct disk_info *disk, uint8_t *buf, uint32_t sector, uint32_t count)
{
	struct usb_disk *u = to_usb_disk(disk);
	int ret = -EIO;

	k_mutex_lock(&u->lock, K_FOREVER);
	if (u->shared && disk_access_status(u->target) == DISK_STATUS_OK) {
		ret = disk_access_read(u->target, buf, sector, count);
	}
	k_mutex_unlock(&u->lock);
	return ret;
}

static int proxy_write(struct disk_info *disk, const uint8_t *buf, uint32_t sector,
		       uint32_t count)
{
	struct usb_disk *u = to_usb_disk(disk);
	int ret = -EIO;

	k_mutex_lock(&u->lock, K_FOREVER);
	if (u->shared && disk_access_status(u->target) == DISK_STATUS_OK) {
		ret = disk_access_write(u->target, buf, sector, count);
	}
	k_mutex_unlock(&u->lock);
	return ret;
}

static int proxy_ioctl(struct disk_info *disk, uint8_t cmd, void *buf)
{
	switch (cmd) {
	case DISK_IOCTL_GET_SECTOR_COUNT:
	case DISK_IOCTL_GET_SECTOR_SIZE:
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
	case DISK_IOCTL_CTRL_SYNC:
		return disk_access_ioctl(to_usb_disk(disk)->target, cmd, buf);
	default:
		/* Init, deinit and power stay with the board's mounts. */
		return 0;
	}
}

static const struct disk_operations proxy_ops = {
	.init = proxy_init,
	.status = proxy_status,
	.read = proxy_read,
	.write = proxy_write,
	.ioctl = proxy_ioctl,
};

static struct usb_disk disks[] = {
	{
		.info = {.name = "UFLASH", .ops = &proxy_ops},
		.target = "FLASH",
		.mount = &FS_FSTAB_ENTRY(DT_NODELABEL(flash_fs)),
		.shared = true,
	},
	{
		.info = {.name = "USD", .ops = &proxy_ops},
		.target = "SD",
		.mount = &FS_FSTAB_ENTRY(DT_NODELABEL(sd_fs)),
		.shared = true,
	},
};

/* Sorted by id: LUN 0 FLASH, LUN 1 SD. */
USBD_DEFINE_MSC_LUN(a_flash, "UFLASH", "ESP-SDR", "FLASH", "1.0");
USBD_DEFINE_MSC_LUN(b_sd, "USD", "ESP-SDR", "SD card", "1.0");

static struct usb_disk *find(const char *mnt)
{
	ARRAY_FOR_EACH_PTR(disks, u) {
		if (strcmp(u->mount->mnt_point, mnt) == 0) {
			return u;
		}
	}
	return NULL;
}

int usb_disk_claim(const char *mnt)
{
	struct usb_disk *u = find(mnt);

	if (u == NULL) {
		return -ENOENT;
	}
	k_mutex_lock(&u->lock, K_FOREVER);
	if (u->shared) {
		u->shared = false;
		u->hidden_at = k_uptime_get();
	}
	k_mutex_unlock(&u->lock);
	/*
	 * No host access from here on: drop what FatFs cached and read the volume afresh. A
	 * remount through the fs API would power the disk off and on, and the SD card does not
	 * always come back from that; registering the work area again keeps the disk as it is.
	 */
	return f_mount((FATFS *)u->mount->fs_data, &u->mount->mnt_point[1], 1) == FR_OK ? 0 : -EIO;
}

int usb_disk_release(const char *mnt)
{
	struct usb_disk *u = find(mnt);
	int64_t wait;

	if (u == NULL) {
		return -ENOENT;
	}
	(void)disk_access_ioctl(u->target, DISK_IOCTL_CTRL_SYNC, NULL);
	wait = u->hidden_at + HIDE_MIN_MS - k_uptime_get();
	if (wait > 0) {
		k_msleep((int32_t)wait);
	}
	k_mutex_lock(&u->lock, K_FOREVER);
	u->shared = true;
	k_mutex_unlock(&u->lock);
	return 0;
}

bool usb_disk_shared(const char *mnt)
{
	struct usb_disk *u = find(mnt);

	return u != NULL && u->shared;
}

static int usb_disks_init(void)
{
	/* A card that does not mount is left alone, never formatted. */
	FS_FSTAB_ENTRY(DT_NODELABEL(sd_fs)).flags |= FS_MOUNT_FLAG_NO_FORMAT;
	ARRAY_FOR_EACH_PTR(disks, u) {
		k_mutex_init(&u->lock);
		(void)disk_access_register(&u->info);
	}
	return 0;
}

/* Before USB starts (app_usb_init(), boot.c), whose MSC class looks the LUNs' disks up, and
 * before MicroPython mounts the volumes. */
SYS_INIT(usb_disks_init, APPLICATION, 0);
