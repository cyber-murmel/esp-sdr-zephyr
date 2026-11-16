/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See esp_sdr_gain_guard.h. From apps/ubertooth's clip guard (ubt_rssi.c), its thresholds now the
 * app's configuration.
 */

#include <string.h>

#include <esp_sdr/esp_sdr_gain_guard.h>
#include <esp_sdr/esp_sdr_place.h>

void esp_sdr_gain_guard_init(struct esp_sdr_gain_guard *g, const struct esp_sdr_gain_guard_cfg *cfg,
			     int64_t now_ms)
{
	memset(g, 0, sizeof(*g));
	g->cfg = cfg;
	g->t_loud = now_ms;
	g->hold_ms = cfg->up_ms;
	g->t_up = now_ms - (int64_t)cfg->up_max_ms;
}

ESP_SDR_CODE(CHUNK, esp_sdr_gain_guard_update)
bool esp_sdr_gain_guard_update(struct esp_sdr_gain_guard *g, int64_t now_ms, uint32_t clipped,
			       uint32_t loud)
{
	const struct esp_sdr_gain_guard_cfg *cfg = g->cfg;
	uint32_t dc = clipped - g->clipped_seen;

	if (loud != g->loud_seen) {
		g->t_loud = now_ms;
	}
	g->clipped_seen = clipped;
	g->loud_seen = loud;
	if (dc != 0U) {
		if (g->nclip == 0U || now_ms - g->t_clip > (int64_t)cfg->down_ms) {
			g->nclip = 0;
			g->t_clip = now_ms;
		}
		g->nclip = (uint8_t)(g->nclip + dc > 255U ? 255U : g->nclip + dc);
		if (g->nclip >= cfg->down_n && g->level + 1U < cfg->levels) {
			if (now_ms - g->t_up < (int64_t)cfg->bounce_ms) {
				/* Back down soon after a step up: hold longer. */
				g->hold_ms = 2U * g->hold_ms < cfg->up_max_ms ? 2U * g->hold_ms
									   : cfg->up_max_ms;
			}
			g->level++;
			g->nclip = 0;
			g->t_loud = now_ms;
			return true;
		}
	}
	if (g->level > 0U && now_ms - g->t_loud >= (int64_t)g->hold_ms) {
		g->level--;
		g->nclip = 0;
		g->t_loud = now_ms;
		g->t_up = now_ms;
		return true;
	}
	if (g->level == 0U && now_ms - g->t_loud >= (int64_t)cfg->up_max_ms &&
	    now_ms - g->t_up >= (int64_t)cfg->up_max_ms) {
		g->hold_ms = cfg->up_ms;
	}
	return false;
}
