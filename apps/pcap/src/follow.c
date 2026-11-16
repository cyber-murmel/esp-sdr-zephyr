/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The follow mode's decisions (follow.h).
 */

#include <string.h>

#include "follow.h"

void follow_init(struct follow *f, uint32_t survey_ms, uint32_t channel_ms, uint32_t now_ms)
{
	memset(f, 0, sizeof(*f));
	f->survey_ms = survey_ms;
	f->channel_ms = channel_ms;
	f->since = now_ms;
}

bool follow_sight(struct follow *f, int channel, uint32_t points, bool from_survey,
		  uint32_t now_ms)
{
	unsigned int k = (unsigned int)(channel - FOLLOW_CHANNEL_FIRST);

	if (k >= FOLLOW_CHANNELS) {
		return false;
	}
	if (f->channel != 0U) {
		if (!from_survey && channel == f->channel) {
			f->last = now_ms;
		}
		return false;
	}
	if (!from_survey) {
		return false;
	}
	f->sights[k] = f->sights[k] > UINT16_MAX - points ? UINT16_MAX
							  : (uint16_t)(f->sights[k] + points);
	f->newest = (uint8_t)channel;
	return f->sights[k] >= FOLLOW_SIGHT_POINTS;
}

/*
 * The channel with the most points in this survey, the newest among equals, leaving out
 * @p except; 0: none has enough.
 */
static uint8_t busiest(const struct follow *f, uint8_t except)
{
	uint16_t most = FOLLOW_SIGHT_POINTS - 1U;
	uint8_t channel = 0;

	for (unsigned int k = 0; k < FOLLOW_CHANNELS; k++) {
		if (FOLLOW_CHANNEL_FIRST + k != except && f->sights[k] > most) {
			most = f->sights[k];
			channel = (uint8_t)(FOLLOW_CHANNEL_FIRST + k);
		}
	}
	if (channel != 0U && f->newest != except &&
	    f->sights[f->newest - FOLLOW_CHANNEL_FIRST] == most) {
		channel = f->newest;
	}
	return channel;
}

enum follow_act follow_step(struct follow *f, uint32_t now_ms, uint32_t *wait_ms)
{
	if (f->channel == 0U) {
		uint32_t in = now_ms - f->since;
		uint8_t channel;

		if (in < f->survey_ms) {
			*wait_ms = f->survey_ms - in;
			return FOLLOW_STAY;
		}
		/* Others take turns with a busy channel: back to it only if nothing else showed. */
		channel = busiest(f, f->previous);
		if (channel == 0U) {
			channel = busiest(f, 0U);
		}
		if (channel == 0U) {
			*wait_ms = FOLLOW_IDLE_MS;
			return FOLLOW_STAY;
		}
		f->channel = channel;
		f->since = now_ms;
		f->last = now_ms;
		*wait_ms = f->channel_ms;
		return FOLLOW_TO_CHANNEL;
	}

	uint32_t quiet = now_ms - f->last, stay = now_ms - f->since;
	uint32_t stay_max = FOLLOW_STAY_MAX * f->channel_ms;

	if (quiet >= f->channel_ms || stay >= stay_max) {
		f->previous = f->channel;
		f->channel = 0U;
		f->newest = 0U;
		f->since = now_ms;
		memset(f->sights, 0, sizeof(f->sights));
		*wait_ms = f->survey_ms;
		return FOLLOW_TO_SURVEY;
	}
	*wait_ms = f->channel_ms - quiet < stay_max - stay ? f->channel_ms - quiet
							   : stay_max - stay;
	return FOLLOW_STAY;
}
