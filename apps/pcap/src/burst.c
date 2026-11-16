/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The burst pool (app.h): CONFIG_APP_PCAP_BURSTS buffers of BURST_MAX_SAMPLES int16 pairs in
 * PSRAM, handed out by a queue of the free ones.
 */

#include <zephyr/kernel.h>
#include <esp_sdr/esp_sdr_place.h>

#include "app.h"
#include "dec_util.h"

static int16_t mem[CONFIG_APP_PCAP_BURSTS][2U * BURST_MAX_SAMPLES] __aligned(16)
	ESP_SDR_DATA(BULK, mem);
static struct burst bursts[CONFIG_APP_PCAP_BURSTS];
K_MSGQ_DEFINE(free_q, sizeof(struct burst *), CONFIG_APP_PCAP_BURSTS, 4);

struct burst *burst_get(void)
{
	struct burst *b;

	if (k_msgq_get(&free_q, &b, K_NO_WAIT) != 0) {
		return NULL;
	}
	*b = (struct burst){.iq = b->iq};
	return b;
}

void burst_put(struct burst *b)
{
	(void)k_msgq_put(&free_q, &b, K_NO_WAIT);
}

uint32_t burst_free_count(void)
{
	return k_msgq_num_used_get(&free_q);
}

int16_t burst_dbfs_x10(float power)
{
	return dec_dbfs_x10(power);
}

static int burst_pool_init(void)
{
	for (unsigned int i = 0; i < CONFIG_APP_PCAP_BURSTS; i++) {
		struct burst *b = &bursts[i];

		b->iq = mem[i];
		(void)k_msgq_put(&free_q, &b, K_NO_WAIT);
	}
	return 0;
}
SYS_INIT(burst_pool_init, APPLICATION, 0);
