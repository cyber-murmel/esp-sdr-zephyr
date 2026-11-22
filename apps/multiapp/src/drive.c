/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The apps as files: <name>.llext on the FAT volume (partition "fat", disk
 * "FLASH"). The host reads it only at boot, before USB and before an app:
 * the FATFS object is in bank 0, which an app's ring overwrites. Reads go in
 * chunks below a sector,
 * so the flash driver never gets a PSRAM buffer. A boot without an app lends
 * the volume to the PC as a USB drive; the DFU target "fat" writes it whole
 * (mkfat.py, west dfu --storage).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/storage/stream_flash.h>
#include <zephyr/usb/class/usbd_dfu.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/usb/usbd.h>

#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/multiapp.h>

#include "app_usb.h"
#include "apps.h"

LOG_MODULE_DECLARE(multiapp, LOG_LEVEL_INF);

#define MOUNT_POINT "/FLASH:"
#define READ_CHUNK  256U
#define MAX_APPS    16U

static FATFS multiapp_fat_fs;
static struct fs_mount_t fat_mount = {
	.type = FS_FATFS,
	.mnt_point = MOUNT_POINT,
	.fs_data = &multiapp_fat_fs,
	.flags = FS_MOUNT_FLAG_READ_ONLY | FS_MOUNT_FLAG_NO_FORMAT,
};

/* What the boot's scan found: per built-in app, its file's size or -errno. */
static int drive_ret = -EAGAIN;
static ssize_t file_size[MAX_APPS];
static bool file_same[MAX_APPS];

static ESP_SDR_DATA(BULK, chunk) uint8_t chunk[READ_CHUNK];

static void file_path(char *path, size_t len, const char *name)
{
	(void)snprintf(path, len, "%s/%s.llext", MOUNT_POINT, name);
}

/* Read the open file into @p buf (up to @p max), or compare it with @p ref; bytes or -errno. */
static ssize_t read_file(struct fs_file_t *f, uint8_t *buf, size_t max, const uint8_t *ref,
			 bool *same)
{
	size_t done = 0;

	if (same != NULL) {
		*same = true;
	}
	for (;;) {
		ssize_t n = fs_read(f, chunk, sizeof(chunk));

		if (n < 0) {
			return n;
		}
		if (n == 0) {
			return (ssize_t)done;
		}
		if (buf != NULL) {
			if (done + (size_t)n > max) {
				return -EFBIG;
			}
			memcpy(buf + done, chunk, (size_t)n);
		}
		if (same != NULL && (done + (size_t)n > max || memcmp(ref + done, chunk, n) != 0)) {
			*same = false;
		}
		done += (size_t)n;
	}
}

#if defined(CONFIG_SETTINGS_FILE)
/* Mounted read-write for the boot's app: the reads below use that mount. */
static bool mounted_rw;

int multiapp_drive_mount_rw(void)
{
	int ret;

	/* Every app start asks, an in-place switch included: mounted, it stays so. */
	if (mounted_rw) {
		return 0;
	}
	fat_mount.flags = FS_MOUNT_FLAG_NO_FORMAT;
	ret = fs_mount(&fat_mount);
	if (ret != 0) {
		fat_mount.flags = FS_MOUNT_FLAG_READ_ONLY | FS_MOUNT_FLAG_NO_FORMAT;
		LOG_WRN("volume not mounted read-write (%d)", ret);
		return ret;
	}
	mounted_rw = true;
	return 0;
}
#endif

int multiapp_drive_scan(void)
{
	char path[48];
	int ret;

	ret = fs_mount(&fat_mount);
	if (ret != 0) {
		drive_ret = ret;
		LOG_WRN("no FAT volume (%d)", ret);
		return ret;
	}
	for (size_t i = 0; i < multiapp_embedded_count && i < MAX_APPS; i++) {
		const struct multiapp_embedded *app = &multiapp_embedded[i];
		struct fs_file_t f;

		fs_file_t_init(&f);
		file_path(path, sizeof(path), app->name);
		ret = fs_open(&f, path, FS_O_READ);
		if (ret != 0) {
			file_size[i] = ret;
			continue;
		}
		if (app->elf != NULL) {
			file_size[i] = read_file(&f, NULL, app->size, app->elf, &file_same[i]);
		} else {
			/* Nothing to compare it with, and it may be big: no reading it each boot. */
			struct fs_dirent entry;

			ret = fs_stat(path, &entry);
			file_size[i] = ret == 0 ? (ssize_t)entry.size : ret;
		}
		(void)fs_close(&f);
	}
	(void)fs_unmount(&fat_mount);
	drive_ret = 0;
	return 0;
}

int multiapp_drive_status(void)
{
	return drive_ret;
}

int multiapp_drive_file(const char *name, ssize_t *size, bool *same)
{
	if (drive_ret != 0) {
		return drive_ret;
	}
	for (size_t i = 0; i < multiapp_embedded_count && i < MAX_APPS; i++) {
		if (strcmp(multiapp_embedded[i].name, name) == 0) {
			*size = file_size[i];
			*same = file_same[i];
			return 0;
		}
	}
	return -ENOENT;
}

/*
 * The USB drives, in a host section whatever the boot runs. In a boot without an app the volume itself,
 * and the SD card's LUN has no medium. MicroPython registers disks of its own that hide a volume while
 * the board writes it (its src/usb_disks.c), and these point at them from the start of its boot.
 */
static char usb_flash_disk[8] = "FLASH";

USBD_DEFINE_MSC_LUN(a_fat, usb_flash_disk, "ESP-SDR", "multiapp", "1.0");
#if DT_NODE_EXISTS(DT_NODELABEL(sd_fs))
USBD_DEFINE_MSC_LUN(b_sd, "USD", "ESP-SDR", "SD card", "1.0");
#endif

void multiapp_drive_app_disks(void)
{
	strcpy(usb_flash_disk, "UFLASH");
}

/*
 * The device's writes to the volume (calibration settings today, files later) take one lock, so
 * that the writers keep out of each other's way and out of the PC's: a PC that has the volume as a
 * drive is its only writer (doc/apps/multiapp-internals.md, "Writers and readers of the volume").
 */
static K_MUTEX_DEFINE(write_lock);
/* HOOK: set while the PC holds the volume over USB MSC; the begin below refuses then. */
static atomic_t msc_exported;

void multiapp_drive_msc_set(bool exported)
{
	atomic_set(&msc_exported, exported ? 1 : 0);
}

int multiapp_drive_write_begin(k_timeout_t timeout)
{
	if (atomic_get(&msc_exported) != 0) {
		return -EBUSY;
	}
	if (k_mutex_lock(&write_lock, timeout) != 0) {
		return -EAGAIN;
	}
	/* The PC may have taken the volume while this waited. */
	if (atomic_get(&msc_exported) != 0) {
		k_mutex_unlock(&write_lock);
		return -EBUSY;
	}
	return 0;
}

void multiapp_drive_write_end(void)
{
	k_mutex_unlock(&write_lock);
}

int multiapp_drive_export(void)
{
	/* The PC writes from here on: the device's writers get -EBUSY. */
	multiapp_drive_msc_set(true);
	/* MSC reports no medium until the disk is initialized. */
	return disk_access_init("FLASH");
}

ssize_t multiapp_drive_read(const char *name, uint8_t *buf, size_t max)
{
	struct fs_file_t f;
	char path[48];
	ssize_t ret;

#if defined(CONFIG_SETTINGS_FILE)
	if (!mounted_rw) {
		ret = fs_mount(&fat_mount);
		if (ret != 0) {
			return ret;
		}
	}
#else
	ret = fs_mount(&fat_mount);
	if (ret != 0) {
		return ret;
	}
#endif
	fs_file_t_init(&f);
	file_path(path, sizeof(path), name);
	ret = fs_open(&f, path, FS_O_READ);
	if (ret == 0) {
		ret = read_file(&f, buf, max, NULL, NULL);
		(void)fs_close(&f);
	}
#if defined(CONFIG_SETTINGS_FILE)
	if (!mounted_rw) {
		(void)fs_unmount(&fat_mount);
	}
#else
	(void)fs_unmount(&fat_mount);
#endif
	return ret;
}

/*
 * ---- DFU target "fat": the volume image ----
 *
 * stream_flash, not flash_img: that one writes an MCUboot image, from the
 * offset swap-using-offset gives the update slot. Pages are erased as the
 * writes reach them.
 */

static struct {
	struct stream_flash_ctx sf;
	uint8_t buf[CONFIG_USBD_DFU_TRANSFER_SIZE];
	uint32_t last_block;
	uint32_t uploaded;
	/* A download went to the volume, not to the image slot. */
	bool written;
} dfu;

static int fat_dfu_read(void *const priv, const uint32_t block, const uint16_t size,
			uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	const struct flash_area *fa;
	uint32_t len;
	int ret;

	ARG_UNUSED(priv);
	if (size == 0U) {
		return 0;
	}
	if (block == 0U) {
		dfu.uploaded = 0;
	} else if (dfu.last_block + 1U != block) {
		return -EINVAL;
	}
	ret = flash_area_open(PARTITION_ID(fat_partition), &fa);
	if (ret != 0) {
		return ret;
	}
	len = MIN((uint32_t)size, fa->fa_size - dfu.uploaded);
	ret = flash_area_read(fa, dfu.uploaded, buf, len);
	flash_area_close(fa);
	if (ret != 0) {
		return ret;
	}
	dfu.last_block = block;
	dfu.uploaded += len;
	return (int)len;
}

static int fat_dfu_write(void *const priv, const uint32_t block, const uint16_t size,
			 const uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	ARG_UNUSED(priv);
	if (block == 0U) {
		const struct flash_area *fa;
		int ret = flash_area_open(PARTITION_ID(fat_partition), &fa);

		if (ret != 0) {
			return ret;
		}
		ret = stream_flash_init(&dfu.sf, flash_area_get_device(fa), dfu.buf,
					sizeof(dfu.buf), fa->fa_off, fa->fa_size, NULL);
		flash_area_close(fa);
		if (ret != 0) {
			return ret;
		}
		dfu.written = true;
		if (size == 0U) {
			return 0;
		}
	} else if (dfu.last_block + 1U != block) {
		return -EINVAL;
	}
	dfu.last_block = block;
	/* Size 0 ends the download: write out what is buffered. */
	return stream_flash_buffered_write(&dfu.sf, buf, size, size == 0U);
}

static bool fat_dfu_next(void *const priv, const enum usb_dfu_state state,
			 const enum usb_dfu_state next)
{
	ARG_UNUSED(priv);
	ARG_UNUSED(state);
	ARG_UNUSED(next);
	return true;
}

USBD_DFU_DEFINE_IMG(fat_image, "fat", NULL, fat_dfu_read, fat_dfu_write, fat_dfu_next);

bool app_usb_dfu_was_slot(void)
{
	return !dfu.written;
}
