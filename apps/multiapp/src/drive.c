/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The apps as files: <name>.llext on the FAT volume (partition "fat", disk "FLASH"). The host reads
 * them at boot, before USB, and when an app starts. The PC may use the volume as a USB drive in
 * every boot that has an endpoint for it, an app running or not: the drive goes through a proxy
 * disk (src/drive_proxy.h). The device only reads the volume (an app's file, a kernel file), and
 * takes it for the time with a claim that makes the PC's disk accesses wait, without hiding the
 * medium. A device writer would hide it (multiapp_drive_write_begin()); the crystal calibration is
 * not one: it has a partition of its own. Reads go in chunks below a sector, so the flash driver
 * never gets a PSRAM buffer. The DFU target "fat" writes the volume whole (mkfat.py, west dfu
 * --storage).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <ff.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
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
#include "drive_proxy.h"

LOG_MODULE_DECLARE(multiapp, LOG_LEVEL_INF);

#define MOUNT_POINT "/FLASH:"
/* The flash disk's name, behind the proxy the PC's drive uses. */
#define DISK_NAME   "FLASH"
#define READ_CHUNK  256U
#define MAX_APPS    16U
/* How long the device waits to have the volume, the PC's command in flight included. */
#define CLAIM_MS    2000

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

/*
 * The PC's drive and the device take turns (src/drive_proxy.h): every device access to the volume
 * after boot is bracketed by drive_take() and drive_give(), in a thread of an internal stack (the
 * shell's, main's, an app's: the flash driver turns the cache off) that is not the USB thread. A
 * read waits for the PC's command in flight, makes the PC's next accesses wait, mounts the volume
 * afresh and reads; the medium stays. While the PC has the medium and no claim is held, the device
 * does not touch the volume.
 */
static struct drive_proxy px;

/* Before the PC gets the medium back after a writer. */
static void sync_volume(void)
{
	(void)disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);
}

/* Zephyr's USB thread: its callbacks carry the PC's data, so a claim there would wait for itself. */
static bool in_usb_thread(void)
{
	const char *name = k_thread_name_get(k_current_get());

	return name != NULL && strcmp(name, "usbd") == 0;
}

static int claim(bool write, k_timeout_t timeout)
{
	int ret = drive_take(&px, timeout, write);

	if (ret == -EDEADLK) {
		LOG_ERR("the volume is claimed from the USB thread (or a write inside a read): refused");
	}
	return ret;
}

int multiapp_drive_write_begin(k_timeout_t timeout)
{
	return claim(true, timeout);
}

int multiapp_drive_read_begin(k_timeout_t timeout)
{
	return claim(false, timeout);
}

void multiapp_drive_write_end(void)
{
	drive_give(&px);
}

void multiapp_drive_read_end(void)
{
	drive_give(&px);
}

void multiapp_drive_busy_hook(bool (*busy)(void))
{
	drive_proxy_set_busy(&px, busy);
}

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
 * The USB drive, in a host section whatever the boot runs: the LUN's disk is a proxy that reports no
 * medium while the device has the volume (drive_proxy.h). MicroPython registers disks of its own
 * that do the same (its src/usb_disks.c), and a boot with it points the LUN at them
 * (multiapp_drive_app_disks()). The SD card's LUN has no medium in the other boots.
 */
#define PROXY_NAME "HFLASH"
static char usb_flash_disk[8] = PROXY_NAME;

USBD_DEFINE_MSC_LUN(a_fat, usb_flash_disk, "ESP-SDR", "multiapp", "1.0");
#if DT_NODE_EXISTS(DT_NODELABEL(sd_fs))
USBD_DEFINE_MSC_LUN(b_sd, "USD", "ESP-SDR", "SD card", "1.0");
#endif

void multiapp_drive_app_disks(void)
{
	strcpy(usb_flash_disk, "UFLASH");
}

/* Only the device initializes the disk (the boot's mount does): the USB thread must not race it. */
static int proxy_init(struct disk_info *disk)
{
	ARG_UNUSED(disk);
	return 0;
}

static int proxy_status(struct disk_info *disk)
{
	ARG_UNUSED(disk);
	if (!drive_proxy_visible(&px)) {
		return DISK_STATUS_NOMEDIA;
	}
	return disk_access_status(DISK_NAME);
}

/* The MSC thread's accesses (its stack is internal: the flash driver turns the cache off). */
static int proxy_read(struct disk_info *disk, uint8_t *buf, uint32_t sector, uint32_t count)
{
	int ret = -EIO;

	ARG_UNUSED(disk);
	if (drive_proxy_io_begin(&px)) {
		ret = disk_access_read(DISK_NAME, buf, sector, count);
		drive_proxy_io_end(&px);
	}
	return ret;
}

static int proxy_write(struct disk_info *disk, const uint8_t *buf, uint32_t sector,
		       uint32_t count)
{
	int ret = -EIO;

	ARG_UNUSED(disk);
	if (drive_proxy_io_begin(&px)) {
		ret = disk_access_write(DISK_NAME, buf, sector, count);
		drive_proxy_io_end(&px);
	}
	return ret;
}

static int proxy_ioctl(struct disk_info *disk, uint8_t cmd, void *buf)
{
	ARG_UNUSED(disk);
	switch (cmd) {
	case DISK_IOCTL_GET_SECTOR_COUNT:
	case DISK_IOCTL_GET_SECTOR_SIZE:
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
	case DISK_IOCTL_CTRL_SYNC:
		return disk_access_ioctl(DISK_NAME, cmd, buf);
	default:
		/* Init, deinit and power stay with the device's mounts. */
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

static struct disk_info proxy_info = {.name = PROXY_NAME, .ops = &proxy_ops};

/* Before USB starts (app_usb_init()), whose MSC class looks the LUN's disk up. */
static int drive_proxy_setup(void)
{
	/* Device users mount the volume themselves, inside the claim: nothing to remount. */
	static const struct drive_proxy_hooks hooks = {
		.sync = sync_volume,
		.in_usb_thread = in_usb_thread,
	};

	drive_proxy_init(&px, &hooks);
	return disk_access_register(&proxy_info);
}
SYS_INIT(drive_proxy_setup, APPLICATION, 0);

bool multiapp_drive_on_bus(void)
{
	return drive_proxy_is_on_bus(&px);
}

void multiapp_drive_msc_set(bool on_bus)
{
	/* MSC reports no medium until the disk is initialized (the boot's scan did, when it found a volume). */
	if (on_bus && disk_access_status(DISK_NAME) != DISK_STATUS_OK) {
		(void)disk_access_init(DISK_NAME);
	}
	drive_proxy_on_bus(&px, on_bus);
}

/*
 * Read the file at @p path of the volume whole into @p buf; its size or -errno. The claim first:
 * -EBUSY when the PC's access does not end in time (a copy that goes on), -EDEADLK from the USB
 * thread. The volume is mounted for this read and unmounted after: f_mount(..., 1) reads the boot
 * sector and the FAT afresh, so what the PC wrote is what is read.
 */
static ssize_t read_path(const char *path, uint8_t *buf, size_t max)
{
	struct fs_file_t f;
	ssize_t ret;

	ret = claim(false, K_MSEC(CLAIM_MS));
	if (ret != 0) {
		return ret == -EAGAIN ? -EBUSY : ret;
	}
	ret = fs_mount(&fat_mount);
	if (ret == 0) {
		fs_file_t_init(&f);
		ret = fs_open(&f, path, FS_O_READ);
		if (ret == 0) {
			ret = read_file(&f, buf, max, NULL, NULL);
			(void)fs_close(&f);
		}
		(void)fs_unmount(&fat_mount);
	}
	drive_give(&px);
	return ret;
}

ssize_t multiapp_drive_read(const char *name, uint8_t *buf, size_t max)
{
	char path[48];

	file_path(path, sizeof(path), name);
	return read_path(path, buf, max);
}

ssize_t multiapp_drive_read_file(const char *file, uint8_t *buf, size_t max)
{
	char path[48];
	int n;

	/* A name in the volume's root: no path, no hidden file. */
	if (file[0] == '\0' || file[0] == '.' || strpbrk(file, "/\\:") != NULL) {
		return -EINVAL;
	}
	n = snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, file);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		return -ENAMETOOLONG;
	}
	return read_path(path, buf, max);
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
