/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The follow mode's decisions (doc/pcap.md, "Modes"): from the survey to the 802.15.4 channel it
 * sighted, back when that channel goes quiet. Plain C on times the caller passes:
 * tests/unit/pcap_follow runs it on native_sim. The caller holds a lock around these.
 */

#ifndef FOLLOW_H_
#define FOLLOW_H_

#include <stdbool.h>
#include <stdint.h>

#define FOLLOW_CHANNEL_FIRST 11
#define FOLLOW_CHANNELS      16U
/*
 * Points on a channel that make the survey go there: a sure sighting has them, a doubtful one a
 * part of them.
 */
#define FOLLOW_SIGHT_POINTS  2U
/* A stay ends after this many times channel_ms, however busy the channel: the survey's turn. */
#define FOLLOW_STAY_MAX      4U
/* How often the survey looks with nothing sighted (a sighting wakes it at once). */
#define FOLLOW_IDLE_MS       1000U

enum follow_act {
	FOLLOW_STAY,
	FOLLOW_TO_CHANNEL,
	FOLLOW_TO_SURVEY,
};

struct follow {
	uint32_t survey_ms, channel_ms;
	/* When this survey or stay began, and the stay's last frame. */
	uint32_t since, last;
	/* This survey's points per channel, and the channel of the newest sighting. */
	uint16_t sights[FOLLOW_CHANNELS];
	uint8_t newest;
	/* The channel of this stay; 0 in the survey. The channel of the last stay. */
	uint8_t channel, previous;
};

/* In the survey from @p now_ms on, for at least @p survey_ms; a stay ends @p channel_ms after its
 * last frame.
 */
void follow_init(struct follow *f, uint32_t survey_ms, uint32_t channel_ms, uint32_t now_ms);

/*
 * The decoder found 802.15.4 on @p channel, worth @p points, in a burst of the survey or of the
 * channel mode: the other mode's bursts, still queued after a switch, do not count, and any
 * burst of the channel followed extends a stay. Returns whether follow_step() may have something
 * to do at once.
 */
bool follow_sight(struct follow *f, int channel, uint32_t points, bool from_survey,
		  uint32_t now_ms);

/*
 * What to do at @p now_ms; f->channel then says where to. @p wait_ms: when to ask again, at the
 * latest.
 */
enum follow_act follow_step(struct follow *f, uint32_t now_ms, uint32_t *wait_ms);

#endif /* FOLLOW_H_ */
