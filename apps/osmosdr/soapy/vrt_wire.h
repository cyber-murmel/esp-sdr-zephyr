/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The VITA 49.2 words of apps/stream_eth that the SoapySDR module needs, without libvrt: the
 * command packet that sets frequency, gain and sample rate (or only asks for the state), the
 * acknowledges and context packets that come back, and the header and trailer of a signal data
 * packet. Header only, plain C++17, no SoapySDR and no libusb, so tests/vrt_wire_test.cpp can
 * check every word against libvrt's encoder and decoder.
 *
 * Layout (big endian words), as libvrt writes them for the board (apps/stream_eth/src/ctl.c):
 *
 *   command:  0x6<size> | stream ID | CAM | message ID | CIF0 | fields
 *             CAM execute, query state and errors 0x010d0000 (two acknowledges come back: the
 *             execution one, naming the fields it refuses, then the query-state one with the
 *             settings epoch), query state alone 0x01040000
 *             CIF0 bit 27 RF reference frequency (64 bit, 44.20 fixed, Hz), bit 23 gain (stage 2
 *             high half, stage 1 low half, 9.7 fixed, PHY gain index, -1 automatic), bit 21 sample
 *             rate (64 bit, 44.20 fixed); fields in this order
 *   ack:      0x64<size> | stream ID | CAM | message ID | [errors] or the query state as CIF0 + fields
 *   context:  0x4<flags><size> | stream ID | seconds | picoseconds (2) | CIF0 | [CIF1] | fields
 *   data:     0x1<flags><size> | stream ID | [class ID] | seconds | picoseconds (2) | int8 I,Q pairs | trailer
 */

#ifndef SOAPY_VRT_WIRE_H_
#define SOAPY_VRT_WIRE_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vrtwire {

constexpr uint32_t STREAM_ID = 0x5d5d0001U;
constexpr uint16_t DEFAULT_PORT = 4992;

inline uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

inline void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* A 64 bit field with 20 fractional bits (frequency, sample rate, bandwidth). */
inline void putFix20(uint8_t *p, double v)
{
	uint64_t f = (uint64_t)std::llround(v * 1048576.0);

	put32(p, (uint32_t)(f >> 32));
	put32(p + 4, (uint32_t)f);
}

inline double getFix20(const uint8_t *p)
{
	int64_t f = (int64_t)((uint64_t)get32(p) << 32 | get32(p + 4));

	return (double)f / 1048576.0;
}

/* Which settings a command carries; with none it only asks for the state. */
struct Settings {
	bool hasFreq = false, hasGain = false, hasRate = false;
	double freqHz = 0;
	/* PHY gain index, -1 for automatic. */
	int gain = -1;
	double rateHz = 0;
};

/*
 * EXECUTE and query state with the errors bit: the board applies the settings, answers with the
 * execution acknowledge (the errors bit makes it name the fields it refuses, ctl.c: cam.errors),
 * then with the query-state acknowledge, which carries the settings epoch after this command.
 */
constexpr uint32_t CAM_EXECUTE = 0x010d0000U;
constexpr uint32_t CAM_QUERY = 0x01040000U;
constexpr uint32_t CAM_EXECUTION_BIT = 1U << 19;
constexpr uint32_t CAM_QUERY_BIT = 1U << 18;
constexpr uint32_t CAM_ERRORS_BIT = 1U << 16;
constexpr uint32_t CAM_WARNINGS_BIT = 1U << 17;
constexpr uint32_t CAM_EXECUTED_BIT = 1U << 10;

constexpr size_t CMD_MAX_BYTES = 40;

/* The command packet for these settings: bytes written, or 0 if out is too small. */
inline size_t buildCommand(uint8_t *out, size_t cap, uint32_t messageId, const Settings &s)
{
	size_t w = 5;

	if (cap < CMD_MAX_BYTES) {
		return 0;
	}
	uint32_t cif0 = 0;

	if (s.hasFreq) {
		cif0 |= 1U << 27;
	}
	if (s.hasGain) {
		cif0 |= 1U << 23;
	}
	if (s.hasRate) {
		cif0 |= 1U << 21;
	}
	put32(out + 4, STREAM_ID);
	put32(out + 8, (s.hasFreq || s.hasGain || s.hasRate) ? CAM_EXECUTE : CAM_QUERY);
	put32(out + 12, messageId);
	put32(out + 16, cif0);
	if (s.hasFreq) {
		putFix20(out + 4 * w, s.freqHz);
		w += 2;
	}
	if (s.hasGain) {
		int16_t g = (int16_t)(s.gain * 128);

		put32(out + 4 * w, (uint32_t)(uint16_t)g);
		w += 1;
	}
	if (s.hasRate) {
		putFix20(out + 4 * w, s.rateHz);
		w += 2;
	}
	put32(out, 0x60000000U | (uint32_t)w);
	return 4 * w;
}

/* What an acknowledge or context packet tells (fields present are marked). */
struct State {
	/*
	 * The settings epoch: CIF0 bit 16 (state and event indicators), its user-defined bits 7..0. It
	 * goes up by one at every accepted change that restarts the ring and is in the context of every
	 * run and in the query-state acknowledge: equal epochs mean the same settings.
	 */
	bool hasEpoch = false;
	uint8_t epoch = 0;
	bool hasFreq = false, hasGain = false, hasRate = false, hasBw = false;
	double freqHz = 0, rateHz = 0, bwHz = 0;
	int gain = -1;
};

/*
 * CIF0 fields from bit 30 down to 15 (those the board sends); stops at the first field it cannot
 * size. p points at the first field word, words is how many follow.
 */
inline void parseCif0(uint32_t cif0, const uint8_t *p, size_t words, State &st)
{
	static const struct {
		int bit, words;
	} sizes[] = {{30, 1}, {29, 2}, {28, 2}, {27, 2}, {26, 2}, {25, 2}, {24, 1}, {23, 1},
		     {22, 1}, {21, 2}, {20, 2}, {19, 1}, {18, 1}, {17, 2}, {16, 1}, {15, 2}};
	size_t at = 0;

	for (const auto &f : sizes) {
		if ((cif0 & (1U << f.bit)) == 0U) {
			continue;
		}
		if (at + (size_t)f.words > words) {
			return;
		}
		const uint8_t *b = p + 4 * at;

		if (f.bit == 29) {
			st.hasBw = true;
			st.bwHz = getFix20(b);
		} else if (f.bit == 27) {
			st.hasFreq = true;
			st.freqHz = getFix20(b);
		} else if (f.bit == 23) {
			st.hasGain = true;
			st.gain = (int)std::lround((double)(int16_t)(get32(b) & 0xffffU) / 128.0);
		} else if (f.bit == 21) {
			st.hasRate = true;
			st.rateHz = getFix20(b);
		} else if (f.bit == 16) {
			st.hasEpoch = true;
			st.epoch = (uint8_t)(get32(b) & 0xffU);
		}
		at += (size_t)f.words;
	}
}

enum class Kind { Bad, Data, Context, Ack };

/* A command acknowledge. */
struct Ack {
	uint32_t messageId = 0;
	bool execution = false, query = false, errors = false, warnings = false, executed = false;
	State state;
	/* The error words of a refusal: which field, and the word (VRT_WEF_*, 0x10000000: out of range). */
	bool errFreq = false, errGain = false, errRate = false, errFormat = false, errOther = false;
	uint32_t codeFreq = 0, codeGain = 0, codeRate = 0, codeFormat = 0;
};

/* The words of a context packet's body. */
struct Context {
	bool hasTime = false;
	uint32_t sec = 0;
	uint64_t ps = 0;
	State state;
};

inline Kind classify(const uint8_t *b, size_t len)
{
	if (len < 8 || (len & 3U) != 0U) {
		return Kind::Bad;
	}
	uint32_t w0 = get32(b);

	if ((size_t)(w0 & 0xffffU) * 4U != len) {
		return Kind::Bad;
	}
	switch (w0 >> 28) {
	case 0:
	case 1:
		return Kind::Data;
	case 4:
		return Kind::Context;
	case 6:
		return (w0 & 0x04000000U) != 0U ? Kind::Ack : Kind::Bad;
	default:
		return Kind::Bad;
	}
}

inline bool parseAck(const uint8_t *b, size_t len, Ack &a)
{
	if (classify(b, len) != Kind::Ack || len < 16) {
		return false;
	}
	uint32_t cam = get32(b + 8);
	size_t at = 4;

	a.messageId = get32(b + 12);
	a.execution = (cam & CAM_EXECUTION_BIT) != 0U;
	a.query = (cam & CAM_QUERY_BIT) != 0U;
	a.errors = (cam & CAM_ERRORS_BIT) != 0U;
	a.warnings = (cam & CAM_WARNINGS_BIT) != 0U;
	a.executed = (cam & CAM_EXECUTED_BIT) != 0U;
	/* Controllee and controller identifiers: one word, or four for a UUID. */
	if ((cam & (1U << 31)) != 0U) {
		at += (cam & (1U << 30)) != 0U ? 4 : 1;
	}
	if ((cam & (1U << 29)) != 0U) {
		at += (cam & (1U << 28)) != 0U ? 4 : 1;
	}
	if (a.errors && !a.query && !a.warnings && 4 * (at + 1) <= len) {
		/* CIF0 of the fields in error, then one error word each in the same order. */
		static const int bits[] = {30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15};
		uint32_t ind = get32(b + 4 * at);
		size_t w = at + 1;

		if ((ind & 0x2U) != 0U) { /* a CIF1 indicator word follows: not decoded */
			w++;
		}
		for (int bit : bits) {
			if ((ind & (1U << bit)) == 0U || 4 * (w + 1) > len) {
				continue;
			}
			uint32_t code = get32(b + 4 * w++);

			if (bit == 27) {
				a.errFreq = true;
				a.codeFreq = code;
			} else if (bit == 23) {
				a.errGain = true;
				a.codeGain = code;
			} else if (bit == 21) {
				a.errRate = true;
				a.codeRate = code;
			} else if (bit == 15) {
				a.errFormat = true;
				a.codeFormat = code;
			} else {
				a.errOther = true;
			}
		}
	}
	if (a.query && !a.errors && 4 * (at + 1) <= len) {
		/* The state as context fields: CIF0, then (CIF1 if flagged: not skipped, fields end) */
		uint32_t cif0 = get32(b + 4 * at);

		parseCif0(cif0, b + 4 * (at + 1), len / 4 - (at + 1), a.state);
	}
	return true;
}

inline bool parseContext(const uint8_t *b, size_t len, Context &c)
{
	if (classify(b, len) != Kind::Context) {
		return false;
	}
	uint32_t w0 = get32(b);
	unsigned tsi = (w0 >> 22) & 3U, tsf = (w0 >> 20) & 3U;
	size_t at = 2; /* words: header, stream ID */

	if ((w0 >> 27) & 1U) { /* class ID */
		at += 2;
	}
	if (tsi != 0) {
		if (4 * (at + 1) > len) {
			return false;
		}
		c.sec = get32(b + 4 * at);
		c.hasTime = true;
		at += 1;
	}
	if (tsf != 0) {
		if (4 * (at + 2) > len) {
			return false;
		}
		c.ps = (uint64_t)get32(b + 4 * at) << 32 | get32(b + 4 * at + 4);
		at += 2;
	}
	if (4 * (at + 1) > len) {
		return false;
	}
	uint32_t cif0 = get32(b + 4 * at);
	size_t fields = at + 1;

	/* CIF0 bit 1 announces CIF1 (and so on): its word sits before the fields. */
	for (int bit = 3; bit >= 1; bit--) {
		if ((cif0 & (1U << bit)) != 0U) {
			fields++;
		}
	}
	if (4 * fields > len) {
		return false;
	}
	parseCif0(cif0, b + 4 * fields, len / 4 - fields, c.state);
	return true;
}

/* A signal data packet's header, trailer and where the samples are. */
struct Data {
	uint32_t streamId = 0, sec = 0;
	uint64_t ps = 0;
	unsigned count = 0;
	const uint8_t *body = nullptr;
	size_t bodyBytes = 0;
	bool loss = false, over = false;
};

inline bool parseData(const uint8_t *b, size_t len, Data &d)
{
	if (len < 28 || classify(b, len) != Kind::Data) {
		return false;
	}
	uint32_t w0 = get32(b);
	size_t i = 4;
	bool trailer = (w0 >> 26) & 1U;
	unsigned tsi = (w0 >> 22) & 3U, tsf = (w0 >> 20) & 3U;

	d.count = (w0 >> 16) & 15U;
	d.streamId = get32(b + i);
	i += 4;
	if ((w0 >> 27) & 1U) {
		i += 8;
	}
	if (tsi != 0) {
		if (i + 4 > len) {
			return false;
		}
		d.sec = get32(b + i);
		i += 4;
	}
	if (tsf != 0) {
		if (i + 8 > len) {
			return false;
		}
		d.ps = (uint64_t)get32(b + i) << 32 | get32(b + i + 4);
		i += 8;
	}
	if (d.streamId != STREAM_ID || tsi == 0 || tsf != 2 || !trailer || i + 4 > len) {
		return false;
	}
	d.body = b + i;
	d.bodyBytes = len - i - 4;
	uint32_t t = get32(b + len - 4);

	d.loss = ((t >> 12) & 1U) != 0U && ((t >> 24) & 1U) != 0U;
	d.over = ((t >> 13) & 1U) != 0U && ((t >> 25) & 1U) != 0U;
	return d.bodyBytes != 0U && (d.bodyBytes & 3U) == 0U;
}

/* Index of a packet's first sample on the board's axis: seconds times the rate, plus the fraction. */
inline uint64_t sampleIndex(const Data &d, uint64_t rate)
{
	return (uint64_t)d.sec * rate +
	       (uint64_t)(((unsigned __int128)d.ps * rate + 500000000000ULL) / 1000000000000ULL);
}

} /* namespace vrtwire */

#endif /* SOAPY_VRT_WIRE_H_ */
