/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shell for the MCH2022 badge's FPGA: load the embedded bitstream, reset, poke frames.
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <esp_sdr/esdr_fpga.h>

static const struct device *const fpga = DEVICE_DT_GET(DT_NODELABEL(fpga));

#ifdef ESDR_FPGA_IMAGE_EMBEDDED
static const uint8_t image[] = {
#include "fpga_image.inc"
};
#else
static const uint8_t image[1];
#endif

static int cmd_load(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (!IS_ENABLED(ESDR_FPGA_IMAGE_EMBEDDED) && sizeof(image) < 2) {
		shell_error(sh, "no embedded bitstream (build with -DESDR_FPGA_IMAGE=...)");
		return -ENOENT;
	}
	ret = esdr_fpga_load(fpga, image, sizeof(image));
	shell_print(sh, "load: %d", ret);
	return ret;
}

static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{
	int ret = esdr_fpga_off(fpga);

	shell_print(sh, "off: %d", ret);
	return ret;
}

static int cmd_reg(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t addr = strtoul(argv[1], NULL, 0);
	uint16_t w;
	int ret;

	if (argc > 2) {
		w = strtoul(argv[2], NULL, 0);
		ret = esdr_fpga_reg_write(fpga, addr, &w, 1);
	} else {
		ret = esdr_fpga_reg_read(fpga, addr, &w, 1);
		shell_print(sh, "0x%04x: 0x%04x", addr, w);
	}
	return ret;
}

/* The gateware's scratch RAM (gateware/esdr_fpga/core.py) and SCRATCH register. */
#define SCRATCH_RAM 0x1000
#define SCRATCH_REG 0x0007

static uint16_t tx_words[256], rx_words[256];

static int cmd_test(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int rounds = argc > 1 ? strtoul(argv[1], NULL, 0) : 1000;
	uint32_t hz = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
	unsigned int bad = 0, words = 0;
	uint32_t seed = k_cycle_get_32();
	int64_t start = k_uptime_get();

	if (hz) {
		esdr_fpga_set_read_frequency(fpga, hz);
	}
	for (unsigned int r = 0; r < rounds; r++) {
		size_t n = 1 + (seed % 256);

		for (size_t i = 0; i < 256; i++) {
			seed = seed * 1664525U + 1013904223U;
			tx_words[i] = seed >> 16;
		}
		if (esdr_fpga_reg_write(fpga, SCRATCH_RAM, tx_words, n) ||
		    esdr_fpga_reg_read(fpga, SCRATCH_RAM, rx_words, n) ||
		    esdr_fpga_reg_write(fpga, SCRATCH_REG, &tx_words[0], 1) ||
		    esdr_fpga_reg_read(fpga, SCRATCH_REG, &rx_words[n], 1)) {
			shell_error(sh, "bus error in round %u", r);
			return -EIO;
		}
		for (size_t i = 0; i < n; i++) {
			bad += rx_words[i] != tx_words[i];
		}
		bad += rx_words[n] != tx_words[0];
		words += n + 1;
	}
	shell_print(sh, "test: %u rounds, %u words, %u bad, read %u Hz, %lld ms", rounds, words,
		    bad, hz, k_uptime_get() - start);
	return bad ? -EIO : 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fpga_cmds,
	SHELL_CMD_ARG(reg, NULL, "reg <addr> [value]", cmd_reg, 2, 1),
	SHELL_CMD_ARG(test, NULL, "test [rounds] [read Hz]: scratch RAM round trips", cmd_test,
		      1, 2),
	SHELL_CMD(load, NULL, "Load the embedded bitstream", cmd_load),
	SHELL_CMD(off, NULL, "Hold the FPGA in reset", cmd_off),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(fpga, &fpga_cmds, "MCH2022 FPGA", NULL);

int main(void)
{
	if (!device_is_ready(fpga)) {
		printk("fpga: not ready\n");
		return 0;
	}
#ifdef ESDR_FPGA_IMAGE_EMBEDDED
	printk("fpga: load %d\n", esdr_fpga_load(fpga, image, sizeof(image)));
#endif
	return 0;
}
