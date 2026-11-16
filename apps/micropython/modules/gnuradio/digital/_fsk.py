# SPDX-License-Identifier: GPL-3.0-or-later
"""The (G)FSK modem behind digital.gfsk_mod, gfsk_demod and correlate_access_code_bb_ts on the
board, in plain Python (CPython runs it too: tests/host/test_fsk.py). The esdr module has the
same receiver and modulator in C (cmod/esdr_fsk.c), which the board uses: Python manages about
50 kS/s, the receiver needs 250.

Modulator, as GNU Radio's gfsk_mod: bits as NRZ +-1, a Gaussian filter (firdes.gaussian(1, sps,
bt, 4 * sps) convolved with sps ones) interpolating by sps, a frequency modulator (the phase
advances by sensitivity * y radians per sample); the filter is flushed with zeros at the end.

Receiver, made for bursts from a peer whose carrier is tens of kHz off: a discriminator (the
phase step between samples, a binary angle of 65536 per turn, from an integer arctangent), its
sum over a symbol (a matched filter for the rectangular part of the pulse), and for each of
the sps sampling phases a hard decision against the mean of that phase's last 32 symbols,
shifted into a register. When the register matches the access code's last 32 bits with at most
min(threshold, 4) errors, the receiver looks at the symbol around it: the sampling phase whose
window correlates best with the whole access code wins, the window gives the carrier offset
(its mean, corrected for an unbalanced code) and the deviation, and all of the code's bits are
sliced again against that offset; with at most threshold errors the frame is taken. Then, as
correlate_access_code_bb_ts: two 16-bit lengths (bytes) that must agree, and that many payload
bytes, sliced on the same phase against the same offset. No timing tracking: 40 ppm of clock
offset move the sampling point by 0.04 symbols over 1000 bits.

All of the receiver is integer arithmetic, so the C version gives the same frames bit for bit.
"""

import math

TURN = 65536  # binary angle units per turn
# The trigger: the access code's last bits, with at most this many errors.
TRIG_BITS = 32
TRIG_ERRORS_MAX = 4
# Symbols in the running mean the trigger slices against.
MEAN_SYMBOLS = 32
# The noise floor: the mean I^2 + Q^2 of the first 2^FLOOR_START samples of a run, then a running
# mean over about 2^FLOOR_SHIFT samples of those below FLOOR_GATE times it (a burst does not
# raise it).
FLOOR_START = 8
FLOOR_SHIFT = 12
FLOOR_GATE = 8
SPS_MAX = 32
LEN_MAX = 255

_POP8 = bytes(bin(k).count("1") for k in range(256))


def popcount32(x):
    return _POP8[x & 255] + _POP8[(x >> 8) & 255] + _POP8[(x >> 16) & 255] + _POP8[(x >> 24) & 255]


def code_bits(code):
    """An access code given as a string of 0 and 1 (GNU Radio's form): (value, length)."""
    code = code.strip()
    if not code or len(code) > 64 or any(c not in "01" for c in code):
        raise ValueError("access code: 1 to 64 characters 0 and 1")
    return int(code, 2), len(code)


# ---- modulator ----


def gaussian(gain, spb, bt, ntaps):
    """firdes.gaussian()."""
    taps = []
    s = 1.0 / (math.sqrt(math.log(2.0)) / (math.tau * bt))
    dt = 1.0 / spb
    t0 = -0.5 * ntaps
    scale = 0.0
    for _ in range(ntaps):
        t0 += 1
        ts = s * dt * t0
        v = math.exp(-0.5 * ts * ts)
        taps.append(v)
        scale += v
    return [v / scale * gain for v in taps]


def gfsk_taps(sps, bt):
    """gfsk_mod's filter: the Gaussian pulse convolved with sps ones (5 sps - 1 taps)."""
    g = gaussian(1.0, sps, bt, 4 * sps)
    out = [0.0] * (len(g) + sps - 1)
    for i, v in enumerate(g):
        for j in range(sps):
            out[i + j] += v
    return out


def flush_symbols(ntaps, sps):
    """Zero symbols after the last bit that let the filter decay."""
    return (ntaps + sps - 1) // sps


def mod_samples(nbits, sps, ntaps):
    """The samples modulate() writes for nbits bits."""
    return (nbits + flush_symbols(ntaps, sps)) * sps


def modulate(data, nbits, sps, taps, sensitivity, amp, out, phase=0.0):
    """The first nbits bits of data (most significant first) -> int16 I/Q, interleaved, into out
    (mod_samples() samples), the filter flushed; as esdr_fsk_mod(). Returns (samples, phase):
    the samples written and the phase reached."""
    nsym = nbits + flush_symbols(len(taps), sps)
    n = nsym * sps
    ntaps = len(taps)
    a = amp * 32767.0
    for k in range(nsym):
        for j in range(sps):
            y = 0.0
            m = 0
            while j + m * sps < ntaps:
                s = k - m
                if 0 <= s < nbits:
                    y += taps[j + m * sps] if (data[s >> 3] >> (7 - (s & 7))) & 1 else -taps[j + m * sps]
                m += 1
            phase += sensitivity * y
            if phase > math.pi or phase < -math.pi:
                phase -= math.tau * math.floor((phase + math.pi) / math.tau)
            o = 2 * (k * sps + j)
            out[o] = _clip(round(a * math.cos(phase)))
            out[o + 1] = _clip(round(a * math.sin(phase)))
    return n, phase


def _clip(v):
    return 32767 if v > 32767 else (-32767 if v < -32767 else v)


# ---- receiver ----


def angle(re, im):
    """atan2(im, re) as a binary angle (-32768 .. 32767), integer arithmetic within 32 bits for
    |re|, |im| below 2^31, error < 0.25 degree; as esdr_fsk_angle() in C."""
    ax = re if re >= 0 else -re
    ay = im if im >= 0 else -im
    mx, mn = (ax, ay) if ax >= ay else (ay, ax)
    if mx == 0:
        return 0
    while mx >= 32768:
        mx >>= 1
        mn >>= 1
    r = (mn << 15) // mx  # tan of the angle from the nearer axis, Q15
    a = (r >> 2) + ((((r * (32768 - r)) >> 15) * 2847) >> 15)  # pi/4 r + 0.273 r (1 - r) rad
    if ay > ax:
        a = 16384 - a
    if re < 0:
        a = 32768 - a
    if im < 0:
        a = -a
    return ((a + 32768) & 0xFFFF) - 32768


class Frame:
    """A received frame: the payload and what the access code told about the signal."""

    def __init__(self, data, errors, end, phase, dc, amp, var, power, floor):
        self.data = data  # payload bytes
        self.errors = errors  # access code bit errors
        self.end = end  # run sample index of the last payload bit
        self.phase = phase  # sampling phase (0 .. sps - 1)
        self.dc = dc  # carrier offset: binary angle per symbol (sum over sps samples)
        self.amp = amp  # deviation: binary angle per symbol
        self.var = var  # variance of the access code's symbols around the ideal
        self.power = power  # mean I^2 + Q^2 over the frame
        self.floor = floor  # mean I^2 + Q^2 of the noise before it

    def snr_db(self):
        """Signal to noise in the receiver's band: the frame's power over the noise floor's."""
        s = self.power - self.floor
        if s <= 0:
            return 0.0
        return min(99.0, 10 * math.log10(s / max(1, self.floor)))

    def offset_hz(self, fs, sps):
        return self.dc / sps / TURN * fs

    def deviation_hz(self, fs, sps):
        return self.amp / sps / TURN * fs


SEARCH, PEND, HEADER, PAYLOAD = 0, 1, 2, 3


class Receiver:
    """The bursts in a stream of int16 I/Q samples, fed in any chunks: process() returns the
    frames that ended in the chunk."""

    def __init__(self, sps, code, code_len, threshold, max_len=LEN_MAX):
        if not 2 <= sps <= SPS_MAX:
            raise ValueError("samples per symbol: 2 .. {}".format(SPS_MAX))
        if not 8 <= code_len <= 64:
            raise ValueError("access code: 8 .. 64 bits")
        if not 1 <= max_len <= LEN_MAX:
            raise ValueError("frame length: 1 .. {}".format(LEN_MAX))
        self.sps = sps
        self.code = code & ((1 << code_len) - 1)
        self.code_len = code_len
        self.threshold = threshold
        self.max_len = max_len
        self.trig_bits = min(TRIG_BITS, code_len)
        self.trig_mask = (1 << self.trig_bits) - 1
        self.trig = self.code & self.trig_mask
        self.trig_errors = min(threshold, TRIG_ERRORS_MAX)
        # The code as +-1, first bit first; its sum (0 when balanced).
        self.c = [1 if (self.code >> (code_len - 1 - i)) & 1 else -1 for i in range(code_len)]
        self.csum = sum(self.c)
        self.mask = hist_size(sps, code_len) - 1
        self.reset()

    def reset(self):
        """A new run (the receiver restarted): nothing carries over."""
        self.hist = [0] * (self.mask + 1)
        self.dring = [0] * self.sps
        self.dsum = 0
        self.prev = (0, 0)
        self.n = 0
        self.ph = 0
        self.mean = [0] * self.sps
        self.reg = [0] * self.sps
        self.fl = 0
        self.state = SEARCH

    def _verify(self):
        """The sampling point within a symbol and a half of the trigger whose window correlates
        best with the access code, and the code's checks there."""
        sps, mask, hist = self.sps, self.mask, self.hist
        L = self.code_len
        best_corr = best_e = None
        for e in range(self.pend - sps // 2, self.pend + sps):
            corr = 0
            for i in range(L):
                v = hist[(e - (L - 1 - i) * sps) & mask]
                corr += v if self.c[i] > 0 else -v
            if best_e is None or corr > best_corr:
                best_corr, best_e = corr, e
        e, corr = best_e, best_corr
        w = [hist[(e - (L - 1 - i) * sps) & mask] for i in range(L)]
        M = sum(w)
        S = self.csum
        den = L * L - S * S
        num = M * L - corr * S  # the offset is num / den
        errors = 0
        for ci, v in zip(self.c, w):
            if (v * den > num) != (ci > 0):
                errors += 1
        if errors > self.threshold:
            return False
        self.num = num
        self.den = den
        dc = num / den
        amp = (corr * L - M * S) / den
        var = 0.0
        for ci, v in zip(self.c, w):
            r = v - dc - ci * amp
            var += r * r
        self.cur = (errors, e % sps, dc, amp, var / L, self.fl >> FLOOR_SHIFT)
        self.state = HEADER
        self.bits = 0
        self.acc = 0
        self.power = 0
        self.npower = 0
        self.next = e + sps
        return True

    def _bit(self, v, nn):
        """The next bit of a frame (its symbol's matched filter output v); a Frame at its end."""
        self.acc = (self.acc << 1) | (1 if v * self.den > self.num else 0)
        self.bits += 1
        if self.state == HEADER:
            if self.bits == 32:
                l0 = self.acc >> 16
                l1 = self.acc & 0xFFFF
                if l0 != l1 or l0 == 0 or l0 > self.max_len:
                    self.state = SEARCH
                else:
                    self.state = PAYLOAD
                    self.want = 8 * l0
                    self.data = bytearray(l0)
                    self.bits = 0
                    self.acc = 0
            return None
        if self.bits & 7 == 0:
            self.data[(self.bits >> 3) - 1] = self.acc & 255
            self.acc = 0
        if self.bits < self.want:
            return None
        self.state = SEARCH
        e, ph, dc, amp, var, floor = self.cur
        return Frame(bytes(self.data), e, nn, ph, dc, amp, var, self.power / max(1, self.npower),
                     floor)

    def process(self, iq, n=None, start=0):
        """n samples (default: all) of iq (int16 I, Q interleaved) from sample start; returns
        the frames that ended among them."""
        if n is None:
            n = (len(iq) - 2 * start) // 2
        frames = []
        sps, mask, hist = self.sps, self.mask, self.hist
        dring, mean, reg = self.dring, self.mean, self.reg
        pi, pq = self.prev
        for k in range(start, start + n):
            # Halved: the products of the next line stay within 32 bits.
            i = iq[2 * k] >> 1
            q = iq[2 * k + 1] >> 1
            d = angle(i * pi + q * pq, q * pi - i * pq)
            pi, pq = i, q
            p = self.ph
            self.dsum += d - dring[p]
            dring[p] = d
            v = self.dsum
            nn = self.n
            hist[nn & mask] = v
            mean[p] += v - hist[(nn - MEAN_SYMBOLS * sps) & mask]
            reg[p] = ((reg[p] << 1) | (1 if v * MEAN_SYMBOLS > mean[p] else 0)) & 0xFFFFFFFF
            st = self.state
            if st <= PEND:
                pw = i * i + q * q
                if nn < (1 << FLOOR_START):
                    self.fl += pw << (FLOOR_SHIFT - FLOOR_START)
                elif pw << FLOOR_SHIFT <= FLOOR_GATE * self.fl:
                    self.fl += pw - (self.fl >> FLOOR_SHIFT)
            if st == SEARCH:
                if nn >= (self.code_len + 1) * sps and \
                        popcount32((reg[p] ^ self.trig) & self.trig_mask) <= self.trig_errors:
                    self.state = PEND
                    self.pend = nn
            elif st == PEND:
                if nn == self.pend + sps - 1 and self._verify():
                    # The first bits after the code may be behind already: from the history.
                    while self.state != SEARCH and self.next <= nn:
                        f = self._bit(hist[self.next & mask], self.next)
                        self.next += sps
                        if f is not None:
                            frames.append(f)
                elif nn == self.pend + sps - 1:
                    self.state = SEARCH
            else:
                self.power += i * i + q * q
                self.npower += 1
                if nn == self.next:
                    self.next += sps
                    f = self._bit(v, nn)
                    if f is not None:
                        frames.append(f)
            self.n = nn + 1
            self.ph = p + 1 if p + 1 < sps else 0
        self.prev = (pi, pq)
        return frames


def hist_size(sps, code_len):
    """Entries of the matched filter history: a power of two holding the access code's window
    and a symbol and a half around it, and the trigger's mean."""
    size = 1
    while size < (code_len + 2) * sps or size <= (MEAN_SYMBOLS + 1) * sps:
        size *= 2
    return size


def header(payload_len, code, code_len):
    """digital.header_format_default's header: the access code, then the payload length in
    bytes twice (16 bits each), most significant bit first; as bytes when the code is whole
    bytes."""
    v = (code << 32) | (payload_len << 16) | payload_len
    nbits = code_len + 32
    if nbits % 8:
        raise ValueError("header_format_default needs an access code of whole bytes")
    return v.to_bytes(nbits // 8, "big")
