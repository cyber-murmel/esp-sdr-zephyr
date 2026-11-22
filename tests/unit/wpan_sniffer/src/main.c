/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of sniffer_line() (apps/wpan/src/sniffer_line.c): the line the upstream Wireshark
 * extcap parses, byte for byte. Runs on native_sim.
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "sniffer_line.h"

#define RSSI_UNDEF (-32768)

ZTEST(wpan_sniffer, test_empty)
{
	char out[SNIFFER_LINE_MAX];
	size_t n = sniffer_line(out, sizeof(out), NULL, 0, 0, 0, 0);

	zassert_str_equal(out, "psdu:  power: 0 lqi: 0 time: 0\r\n");
	zassert_equal(n, strlen(out));
}

ZTEST(wpan_sniffer, test_ack)
{
	static const uint8_t ack[] = {0x02, 0x00, 0x2a, 0x00, 0x00};
	char out[SNIFFER_LINE_MAX];
	size_t n = sniffer_line(out, sizeof(out), ack, sizeof(ack), 255, -57, 1234567);

	zassert_str_equal(out, "psdu: 02002a0000 power: -57 lqi: 255 time: 1234567\r\n");
	zassert_equal(n, strlen(out));
}

ZTEST(wpan_sniffer, test_full_psdu)
{
	uint8_t psdu[127];
	char out[SNIFFER_LINE_MAX];
	char want[SNIFFER_LINE_MAX];
	size_t w = 0;
	size_t n;

	for (size_t i = 0; i < sizeof(psdu); i++) {
		psdu[i] = (uint8_t)(i * 3 + 0xa0);
	}
	w += snprintf(want, sizeof(want), "psdu: ");
	for (size_t i = 0; i < sizeof(psdu); i++) {
		w += snprintf(&want[w], sizeof(want) - w, "%02x", psdu[i]);
	}
	snprintf(&want[w], sizeof(want) - w, " power: 12 lqi: 7 time: 99\r\n");

	n = sniffer_line(out, sizeof(out), psdu, sizeof(psdu), 7, 12, 99);
	zassert_str_equal(out, want);
	zassert_equal(n, strlen(want));
	zassert_true(n < SNIFFER_LINE_MAX);
}

ZTEST(wpan_sniffer, test_rssi_values)
{
	static const uint8_t p[] = {0xab};
	char out[SNIFFER_LINE_MAX];

	sniffer_line(out, sizeof(out), p, 1, 1, RSSI_UNDEF, 5);
	zassert_str_equal(out, "psdu: ab power: -32768 lqi: 1 time: 5\r\n");
	sniffer_line(out, sizeof(out), p, 1, 1, 32767, 5);
	zassert_str_equal(out, "psdu: ab power: 32767 lqi: 1 time: 5\r\n");
	sniffer_line(out, sizeof(out), p, 1, 1, 10, 5);
	zassert_str_equal(out, "psdu: ab power: 10 lqi: 1 time: 5\r\n");
}

ZTEST(wpan_sniffer, test_time_max)
{
	static const uint8_t p[] = {0x00, 0xff};
	char out[SNIFFER_LINE_MAX];

	sniffer_line(out, sizeof(out), p, 2, 0, -1, UINT64_MAX);
	zassert_str_equal(out, "psdu: 00ff power: -1 lqi: 0 time: 18446744073709551615\r\n");
}

ZTEST(wpan_sniffer, test_truncation)
{
	static const uint8_t p[] = {0x12, 0x34, 0x56};
	char out[16];
	char big[8];
	size_t n;

	/* Cut at cap - 1 characters, always NUL-terminated, in the hex and in the tail. */
	memset(out, 'x', sizeof(out));
	n = sniffer_line(out, sizeof(out), p, 3, 1, 2, 3);
	zassert_equal(n, 15);
	zassert_equal(out[15], '\0');
	zassert_mem_equal(out, "psdu: 123456 pow", 15);

	memset(big, 'x', sizeof(big));
	n = sniffer_line(big, 9 - 1, p, 3, 1, 2, 3);
	zassert_equal(n, 7);
	zassert_str_equal(big, "psdu: 1");
	zassert_equal(big[7], '\0');

	n = sniffer_line(big, 7, p, 3, 1, 2, 3);
	zassert_equal(n, 6);
	zassert_str_equal(big, "psdu: ");

	n = sniffer_line(big, 3, p, 3, 1, 2, 3);
	zassert_equal(n, 2);
	zassert_str_equal(big, "ps");

	n = sniffer_line(big, 1, p, 3, 1, 2, 3);
	zassert_equal(n, 0);
	zassert_equal(big[0], '\0');

	big[0] = 'x';
	n = sniffer_line(big, 0, p, 3, 1, 2, 3);
	zassert_equal(n, 0);
	zassert_equal(big[0], 'x');
}

ZTEST_SUITE(wpan_sniffer, NULL, NULL, NULL, NULL, NULL);
