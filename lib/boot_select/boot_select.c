/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MCUboot's choice among several complete images in the flash, for a chip whose apps cannot be
 * loaded as extensions (apps/osmosdr_serial/c6apps). The first image is the default. The last
 * sector of sys_partition can ask for another one. A request lasts until a power-on, which erases
 * it: not for one boot, because a terminal opening a USB-Serial-JTAG port resets the chip, which
 * would leave nobody able to reach the app's console. A request for image 0 also erases it. One for an image
 * that is not there or does not validate boots the default, and stays until then.
 */

#include <string.h>

#include <esp_rom_sys.h>
#include <soc/reset_reasons.h>
#include <zephyr/devicetree/partitions.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include "bootutil/boot_hooks.h"
#include "bootutil/bootutil.h"
#include "bootutil/bootutil_public.h"
#include "bootutil/fault_injection_hardening.h"

#define REQUEST_SECTOR 4096U
static const char request_magic[8] = {'E', 'S', 'D', 'R', 'B', 'O', 'O', 'T'};

static const uint8_t images[] = {
	PARTITION_ID(slot0_partition),
	PARTITION_ID(slot1_partition),
	PARTITION_ID(slot2_partition),
	PARTITION_ID(slot3_partition),
	PARTITION_ID(slot4_partition),
};

static struct image_header header;
static uint8_t validate_buf[256];
static size_t current;

/* The image asked for, 0 without a request or when this boot ends the request. */
static size_t request_read(void)
{
	const struct flash_area *fa;
	struct {
		char magic[sizeof(request_magic)];
		uint8_t index;
	} request;
	size_t index = 0U;
	off_t off;

	if (flash_area_open(PARTITION_ID(sys_partition), &fa) != 0) {
		return 0U;
	}
	off = flash_area_get_size(fa) - REQUEST_SECTOR;
	if (flash_area_read(fa, off, &request, sizeof(request)) == 0 &&
	    memcmp(request.magic, request_magic, sizeof(request_magic)) == 0) {
		if (request.index == 0U ||
		    esp_rom_get_reset_reason(0) == RESET_REASON_CHIP_POWER_ON) {
			(void)flash_area_erase(fa, off, REQUEST_SECTOR);
		} else if (request.index < ARRAY_SIZE(images)) {
			index = request.index;
		}
	}
	flash_area_close(fa);
	return index;
}

static bool image_good(size_t index, struct boot_rsp *rsp)
{
	const struct flash_area *fa;
	FIH_DECLARE(fih_rc, FIH_FAILURE);
	bool good = false;

	if (flash_area_open(images[index], &fa) != 0) {
		return false;
	}
	if (boot_image_load_header(fa, &header) == 0) {
		FIH_CALL(bootutil_img_validate, fih_rc, NULL, &header, fa, validate_buf,
			 sizeof(validate_buf), NULL, 0, NULL);
		if (FIH_EQ(fih_rc, FIH_SUCCESS)) {
			rsp->br_flash_dev_id = flash_area_get_device_id(fa);
			rsp->br_image_off = flash_area_get_off(fa);
			rsp->br_hdr = &header;
			good = true;
		}
	}
	flash_area_close(fa);
	return good;
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
	size_t wanted = request_read();

	if (wanted != 0U && image_good(wanted, rsp)) {
		current = wanted;
		FIH_RET(FIH_SUCCESS);
	}
	if (image_good(0U, rsp)) {
		current = 0U;
		FIH_RET(FIH_SUCCESS);
	}
	FIH_RET(FIH_FAILURE);
}

/* The port's do_boot() knows two slots; the load goes to the image chosen above. */
int flash_area_id_from_multi_image_slot_hook(int image_index, int slot, int *area_id)
{
	ARG_UNUSED(image_index);
	ARG_UNUSED(slot);
	*area_id = images[current];
	return 0;
}

int flash_area_get_device_id_hook(const struct flash_area *fa, uint8_t *dev_id)
{
	ARG_UNUSED(fa);
	ARG_UNUSED(dev_id);
	return BOOT_HOOK_REGULAR;
}
