/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The follow mode's decisions (apps/pcap/src/follow.c). Times in ms, a survey of at least 100 and
 * stays that end 50 after their last frame.
 */

#include <stdint.h>

#include <zephyr/ztest.h>

#include "follow.h"

#define T0        1000U
#define SURVEY_MS 100U
#define STAY_MS   50U
/* A sure sighting and a doubtful one. */
#define SURE      FOLLOW_SIGHT_POINTS
#define HALF      1U

static struct follow f;
static uint32_t wait_ms;

static enum follow_act step(uint32_t now)
{
	return follow_step(&f, now, &wait_ms);
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	follow_init(&f, SURVEY_MS, STAY_MS, T0);
}

/* On channel @p channel from @p now on. */
static void go_to(int channel, uint32_t now)
{
	zassert_true(follow_sight(&f, channel, SURE, true, now));
	zassert_equal(step(now), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, channel);
}

ZTEST(pcap_follow, test_nothing_sighted_stays_in_the_survey)
{
	zassert_equal(step(T0), FOLLOW_STAY);
	zassert_equal(wait_ms, SURVEY_MS);
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_STAY);
	zassert_equal(wait_ms, FOLLOW_IDLE_MS);
	zassert_equal(f.channel, 0);
}

ZTEST(pcap_follow, test_sighting_goes_there_after_the_survey_time)
{
	zassert_true(follow_sight(&f, 20, SURE, true, T0 + 10U));
	zassert_equal(step(T0 + 10U), FOLLOW_STAY);
	zassert_equal(wait_ms, SURVEY_MS - 10U);
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 20);
	zassert_equal(wait_ms, STAY_MS);
}

ZTEST(pcap_follow, test_sighting_after_the_survey_time_goes_at_once)
{
	go_to(25, T0 + 5000U);
}

ZTEST(pcap_follow, test_goes_to_the_busiest_channel)
{
	zassert_true(follow_sight(&f, 15, SURE, true, T0 + 1U));
	zassert_true(follow_sight(&f, 25, SURE, true, T0 + 2U));
	zassert_true(follow_sight(&f, 15, SURE, true, T0 + 3U));
	zassert_true(follow_sight(&f, 11, SURE, true, T0 + 4U));
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 15);
}

ZTEST(pcap_follow, test_the_newest_among_equals)
{
	zassert_true(follow_sight(&f, 15, SURE, true, T0 + 1U));
	zassert_true(follow_sight(&f, 25, SURE, true, T0 + 2U));
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 25);
}

ZTEST(pcap_follow, test_a_doubtful_sighting_alone_goes_nowhere)
{
	zassert_false(follow_sight(&f, 18, HALF, true, T0 + 1U), "no reason to look");
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_STAY);
	zassert_equal(f.channel, 0);
	/* Another channel's does not add up with it. */
	zassert_false(follow_sight(&f, 19, HALF, true, T0 + SURVEY_MS));
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_STAY);
	/* A second one on the same channel does. */
	zassert_true(follow_sight(&f, 18, HALF, true, T0 + SURVEY_MS + 1U));
	zassert_equal(step(T0 + SURVEY_MS + 1U), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 18);
}

ZTEST(pcap_follow, test_a_doubtful_newest_does_not_beat_a_sure_one)
{
	zassert_true(follow_sight(&f, 15, SURE, true, T0 + 1U));
	zassert_false(follow_sight(&f, 25, HALF, true, T0 + 2U));
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 15);
}

ZTEST(pcap_follow, test_a_doubtful_burst_extends_a_stay)
{
	const uint32_t t = T0 + SURVEY_MS;

	go_to(20, t);
	zassert_false(follow_sight(&f, 20, HALF, false, t + 40U));
	zassert_equal(step(t + STAY_MS), FOLLOW_STAY);
}

ZTEST(pcap_follow, test_channels_take_turns)
{
	uint32_t t = T0 + SURVEY_MS;

	go_to(25, t);
	t += STAY_MS;
	zassert_equal(step(t), FOLLOW_TO_SURVEY);
	/* The last stay's channel shows more, but another one showed. */
	for (int k = 0; k < 5; k++) {
		zassert_true(follow_sight(&f, 25, SURE, true, t + 1U));
	}
	zassert_true(follow_sight(&f, 15, SURE, true, t + 2U));
	t += SURVEY_MS;
	zassert_equal(step(t), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 15);
	t += STAY_MS;
	zassert_equal(step(t), FOLLOW_TO_SURVEY);
	/* Nothing else showed: back to it, at once after its survey time. */
	zassert_true(follow_sight(&f, 15, SURE, true, t + SURVEY_MS));
	zassert_equal(step(t + SURVEY_MS), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 15);
}

ZTEST(pcap_follow, test_points_saturate)
{
	for (uint32_t k = 0; k < 40000U; k++) {
		zassert_true(follow_sight(&f, 11, SURE, true, T0));
	}
	zassert_equal(f.sights[0], UINT16_MAX);
}

ZTEST(pcap_follow, test_channels_off_the_grid_do_not_count)
{
	zassert_false(follow_sight(&f, 0, SURE, true, T0));
	zassert_false(follow_sight(&f, 10, SURE, true, T0));
	zassert_false(follow_sight(&f, 27, SURE, true, T0));
	zassert_false(follow_sight(&f, -1, SURE, true, T0));
	zassert_equal(step(T0 + SURVEY_MS), FOLLOW_STAY);
	zassert_true(follow_sight(&f, 11, SURE, true, T0));
	zassert_true(follow_sight(&f, 26, SURE, true, T0));
}

ZTEST(pcap_follow, test_stay_ends_when_quiet)
{
	const uint32_t t = T0 + SURVEY_MS;

	go_to(20, t);
	zassert_equal(step(t + STAY_MS - 1U), FOLLOW_STAY);
	zassert_equal(wait_ms, 1U);
	zassert_equal(step(t + STAY_MS), FOLLOW_TO_SURVEY);
	zassert_equal(f.channel, 0);
	zassert_equal(wait_ms, SURVEY_MS);
}

ZTEST(pcap_follow, test_frames_extend_the_stay)
{
	const uint32_t t = T0 + SURVEY_MS;

	go_to(20, t);
	zassert_false(follow_sight(&f, 20, SURE, false, t + 40U), "a stay needs no wake");
	zassert_equal(step(t + STAY_MS), FOLLOW_STAY);
	zassert_equal(wait_ms, 40U);
	zassert_equal(step(t + 90U), FOLLOW_TO_SURVEY);
}

ZTEST(pcap_follow, test_a_busy_channel_gives_the_survey_its_turn)
{
	const uint32_t t = T0 + SURVEY_MS, max = FOLLOW_STAY_MAX * STAY_MS;

	go_to(20, t);
	for (uint32_t m = 10U; m < max; m += 10U) {
		zassert_false(follow_sight(&f, 20, SURE, false, t + m));
		zassert_equal(step(t + m), FOLLOW_STAY, "at %u", m);
		zassert_true(wait_ms <= max - m, "waits %u at %u", wait_ms, m);
	}
	zassert_false(follow_sight(&f, 20, SURE, false, t + max));
	zassert_equal(step(t + max), FOLLOW_TO_SURVEY);
	/* And it holds its survey time, whatever is sighted at once. */
	zassert_true(follow_sight(&f, 20, SURE, true, t + max + 1U));
	zassert_equal(step(t + max + 1U), FOLLOW_STAY);
	zassert_equal(wait_ms, SURVEY_MS - 1U);
	zassert_equal(step(t + max + SURVEY_MS), FOLLOW_TO_CHANNEL);
}

ZTEST(pcap_follow, test_the_other_modes_bursts_do_not_count)
{
	const uint32_t t = T0 + SURVEY_MS;

	/* The channel mode's frames still queued in the survey. */
	zassert_false(follow_sight(&f, 20, SURE, false, T0 + 1U));
	zassert_equal(step(t), FOLLOW_STAY);
	/* The survey's bursts still queued on a channel, this one's and another's. */
	go_to(20, t);
	zassert_false(follow_sight(&f, 20, SURE, true, t + 40U));
	zassert_false(follow_sight(&f, 25, SURE, false, t + 40U));
	zassert_equal(step(t + STAY_MS), FOLLOW_TO_SURVEY);
}

ZTEST(pcap_follow, test_a_new_survey_forgets_the_last_ones_sightings)
{
	const uint32_t t = T0 + SURVEY_MS;

	zassert_true(follow_sight(&f, 15, SURE, true, T0 + 1U));
	zassert_true(follow_sight(&f, 15, SURE, true, T0 + 2U));
	zassert_true(follow_sight(&f, 25, SURE, true, T0 + 3U));
	zassert_equal(step(t), FOLLOW_TO_CHANNEL);
	zassert_equal(f.channel, 15);
	zassert_equal(step(t + STAY_MS), FOLLOW_TO_SURVEY);
	zassert_equal(step(t + STAY_MS + SURVEY_MS), FOLLOW_STAY);
	zassert_equal(f.channel, 0);
}

ZTEST(pcap_follow, test_the_clock_may_wrap)
{
	const uint32_t t = UINT32_MAX - 10U;

	follow_init(&f, SURVEY_MS, STAY_MS, t);
	zassert_true(follow_sight(&f, 20, SURE, true, t));
	zassert_equal(step(t + 20U), FOLLOW_STAY);
	zassert_equal(wait_ms, SURVEY_MS - 20U);
	zassert_equal(step(t + SURVEY_MS), FOLLOW_TO_CHANNEL);
	zassert_equal(step(t + SURVEY_MS + STAY_MS), FOLLOW_TO_SURVEY);
}

ZTEST_SUITE(pcap_follow, NULL, NULL, before_each, NULL, NULL);
