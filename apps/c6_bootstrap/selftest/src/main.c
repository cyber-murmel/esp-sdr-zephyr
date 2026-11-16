/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The first image the EV board's ESP32-C6 gets (through ../, apps/c6_bootstrap): it shows that
 * the ESP-IDF bootloader starts a Zephyr image (CONFIG_ESP_IDF_BOOTLOADER) and steps back. It
 * prints where it runs from on UART0 (the ESP-Prog connector), erases the otadata sector with
 * the newest entry, the one that selected it, and restarts: the bootloader then starts the
 * firmware the C6 ran before (esp-hosted-mcu), which the ESP32-P4 sees answer again.
 */

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/version.h>

#define OTADATA_OFFSET FIXED_PARTITION_OFFSET(otadata_partition)
#define SECTOR 0x1000U

/* esp_ota_select_entry_t: the sequence number, and the ROM's CRC-32 of it. */
struct ota_entry {
	uint32_t seq;
	uint8_t label[20];
	uint32_t state;
	uint32_t crc;
} __packed;

static bool ota_valid(const struct ota_entry *e)
{
	uint8_t le[4];

	sys_put_le32(e->seq, le);
	return e->seq != 0xffffffffU && e->crc == crc32_ieee_update(0xffffffffU, le, sizeof(le));
}

int main(void)
{
	const struct device *flash = FIXED_PARTITION_DEVICE(otadata_partition);
	struct ota_entry e[2];
	unsigned int newest = 2;
	int ret;

	printk("esp-sdr c6 selftest: Zephyr %s started by the ESP-IDF bootloader\n",
	       KERNEL_VERSION_STRING);
	for (unsigned int s = 0; s < 2U; s++) {
		ret = flash_read(flash, OTADATA_OFFSET + s * SECTOR, &e[s], sizeof(e[s]));
		if (ret != 0) {
			printk("esp-sdr c6 selftest: otadata read %d\n", ret);
			return 0;
		}
		printk("esp-sdr c6 selftest: otadata %u seq %u %s\n", s, e[s].seq,
		       ota_valid(&e[s]) ? "valid" : "invalid");
		if (ota_valid(&e[s]) && (newest == 2U || e[s].seq > e[newest].seq)) {
			newest = s;
		}
	}
	if (newest == 2U) {
		printk("esp-sdr c6 selftest: no otadata entry selects this image, staying\n");
		return 0;
	}
	ret = flash_erase(flash, OTADATA_OFFSET + newest * SECTOR, SECTOR);
	printk("esp-sdr c6 selftest: erased otadata %u: %d, restarting\n", newest, ret);
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
