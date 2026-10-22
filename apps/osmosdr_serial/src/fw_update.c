/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firmware update over the link, for a C6 that the ESP-IDF bootloader starts from an OTA
 * partition (CONFIG_ESP_IDF_BOOTLOADER, as the esp-hosted-mcu firmware left the EV board's C6):
 * the new image goes to the OTA partition the C6 does not run from, then the next otadata
 * entry selects it. The partitions come from the partition table, so either esp-hosted-mcu
 * layout works. The bootloader checks the image (checksum and SHA-256) at the restart and
 * stays with the running partition if it is broken.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "fw_update.h"

#define PART_TABLE_ADDR 0x8000U
#define PART_TABLE_ENTRIES 95U
#define PART_MAGIC 0x50aaU
#define PART_TYPE_APP 0x00U
#define PART_TYPE_DATA 0x01U
#define PART_SUBTYPE_OTA_0 0x10U
#define PART_SUBTYPE_OTA_MAX 0x1fU
#define PART_SUBTYPE_DATA_OTA 0x00U

#define SECTOR 0x1000U
#define IMAGE_MAGIC 0xe9U

/* The ESP-IDF partition table entry and otadata entry (esp_flash_partitions.h). */
struct part_entry {
	uint16_t magic;
	uint8_t type;
	uint8_t subtype;
	uint32_t offset;
	uint32_t size;
	char label[16];
	uint32_t flags;
} __packed;

struct ota_entry {
	uint32_t seq;
	uint8_t label[20];
	uint32_t state;
	uint32_t crc;
} __packed;

static const struct device *const flash = DEVICE_DT_GET(DT_PARENT(DT_CHOSEN(zephyr_flash)));

static struct {
	uint32_t ota_addr[2], ota_size[2];
	uint32_t otadata_addr;
	unsigned int ota_count;
	/* The update in progress: target partition, image size, bytes written, erased up to. */
	bool active;
	unsigned int target;
	uint32_t size, written, erased;
} fw;

/* What the ESP-IDF bootloader computes: esp_rom_crc32_le(UINT32_MAX, &seq, 4). */
static uint32_t ota_crc(uint32_t seq)
{
	uint8_t le[4];

	sys_put_le32(seq, le);
	return crc32_ieee_update(0xffffffffU, le, sizeof(le));
}

static bool ota_valid(const struct ota_entry *e)
{
	return e->seq != 0xffffffffU && e->crc == ota_crc(e->seq);
}

static int read_partitions(void)
{
	struct part_entry e;

	fw.ota_count = 0;
	fw.otadata_addr = 0;
	for (unsigned int i = 0; i < PART_TABLE_ENTRIES; i++) {
		int ret = flash_read(flash, PART_TABLE_ADDR + i * sizeof(e), &e, sizeof(e));

		if (ret != 0) {
			return ret;
		}
		if (sys_le16_to_cpu(e.magic) != PART_MAGIC) {
			break;
		}
		if (e.type == PART_TYPE_APP && e.subtype >= PART_SUBTYPE_OTA_0 &&
		    e.subtype <= PART_SUBTYPE_OTA_MAX && e.subtype - PART_SUBTYPE_OTA_0 < 2) {
			fw.ota_addr[e.subtype - PART_SUBTYPE_OTA_0] = sys_le32_to_cpu(e.offset);
			fw.ota_size[e.subtype - PART_SUBTYPE_OTA_0] = sys_le32_to_cpu(e.size);
			fw.ota_count++;
		} else if (e.type == PART_TYPE_DATA && e.subtype == PART_SUBTYPE_DATA_OTA) {
			fw.otadata_addr = sys_le32_to_cpu(e.offset);
		}
	}
	return fw.ota_count == 2U && fw.otadata_addr != 0U ? 0 : -ENOENT;
}

/* The newest valid otadata entry in either sector: its index, and its sequence number (0: none). */
static int read_otadata(unsigned int *newest_sector, uint32_t *newest_seq)
{
	*newest_sector = 0;
	*newest_seq = 0;
	for (unsigned int s = 0; s < 2U; s++) {
		struct ota_entry e;
		int ret = flash_read(flash, fw.otadata_addr + s * SECTOR, &e, sizeof(e));

		if (ret != 0) {
			return ret;
		}
		if (ota_valid(&e) && e.seq > *newest_seq) {
			*newest_seq = e.seq;
			*newest_sector = s;
		}
	}
	return 0;
}

int fw_update_begin(uint32_t size)
{
	unsigned int sector;
	uint32_t seq;
	int ret;

	fw.active = false;
	if (!device_is_ready(flash)) {
		return -ENODEV;
	}
	ret = read_partitions();
	if (ret == 0) {
		ret = read_otadata(&sector, &seq);
	}
	if (ret != 0) {
		return ret;
	}
	/* The bootloader runs partition (seq - 1) % count, the first one without an entry. */
	fw.target = seq == 0U ? 1U : (seq % fw.ota_count);
	if (size == 0U || size > fw.ota_size[fw.target]) {
		return -EFBIG;
	}
	fw.size = size;
	fw.written = 0;
	fw.erased = 0;
	fw.active = true;
	return 0;
}

int fw_update_write(uint32_t offset, const uint8_t *data, size_t len)
{
	uint32_t base = fw.ota_addr[fw.target];
	int ret;

	if (!fw.active) {
		return -EBADF;
	}
	if (offset != fw.written || len == 0U || len % 4U != 0U || fw.size - offset < len) {
		return -EINVAL;
	}
	if (offset == 0U && data[0] != IMAGE_MAGIC) {
		return -EINVAL;
	}
	/* Erase as the image grows, a sector at a time: each request stays short. */
	while (fw.erased < offset + len) {
		ret = flash_erase(flash, base + fw.erased, SECTOR);
		if (ret != 0) {
			return ret;
		}
		fw.erased += SECTOR;
	}
	ret = flash_write(flash, base + offset, data, len);
	if (ret != 0) {
		return ret;
	}
	fw.written += len;
	return 0;
}

static void reboot_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_fn);

int fw_update_end(uint32_t crc)
{
	uint8_t buf[256];
	uint32_t got = 0;
	unsigned int sector;
	uint32_t seq;
	struct ota_entry e;
	int ret;

	if (!fw.active || fw.written != fw.size) {
		return -EBADF;
	}
	fw.active = false;
	for (uint32_t off = 0; off < fw.size; off += sizeof(buf)) {
		size_t n = MIN(sizeof(buf), fw.size - off);

		ret = flash_read(flash, fw.ota_addr[fw.target] + off, buf, n);
		if (ret != 0) {
			return ret;
		}
		got = crc32_ieee_update(got, buf, n);
	}
	if (got != crc) {
		return -EIO;
	}

	/* The next sequence number that selects the target, in the older sector. */
	ret = read_otadata(&sector, &seq);
	if (ret != 0) {
		return ret;
	}
	do {
		seq++;
	} while ((seq - 1U) % fw.ota_count != fw.target);
	memset(&e, 0xff, sizeof(e));
	e.seq = seq;
	e.crc = ota_crc(seq);
	sector = seq == 1U ? 0U : 1U - sector;
	ret = flash_erase(flash, fw.otadata_addr + sector * SECTOR, SECTOR);
	if (ret == 0) {
		ret = flash_write(flash, fw.otadata_addr + sector * SECTOR, &e, sizeof(e));
	}
	if (ret != 0) {
		return ret;
	}
	/* Restart once the reply is out. */
	k_work_schedule(&reboot_work, K_MSEC(200));
	return 0;
}
