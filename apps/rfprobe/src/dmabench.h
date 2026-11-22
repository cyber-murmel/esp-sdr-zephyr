/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "probe dma": the S3's two DMAs (esp_sdr_dma.h) as register writers and chain starters. GDMA
 * SRAM-to-SRAM rows, the GDMA into a register (refused), the peripheral backup DMA writing and
 * reading register blocks, its map mode, and the backup DMA starting a GDMA chain.
 */

#ifndef DMABENCH_H_
#define DMABENCH_H_

#include <stddef.h>

#include <zephyr/shell/shell.h>

/* argv[0] is "dma": sram | reg [mac] | regs [n|mac] | map | kick. Returns 0 or -errno. */
int dmabench_run(const struct shell *sh, size_t argc, char **argv);

#endif
