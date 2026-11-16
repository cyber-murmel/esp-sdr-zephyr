-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Wireshark dissector of the pcap app's burst record (doc/pcap.md): the packets on the
-- stream's LINKTYPE_USER0 interface, struct pcap_burst_hdr of apps/pcap/src/pcap_proto.h and
-- then the bits (the most significant bit of each byte first), little endian. The header is
-- read by its own length: fields a later version appends are skipped, the bits follow them.
-- Load it from Wireshark's personal Lua plugins folder (Help > About > Folders), or with
-- tshark -X lua_script:esdr_burst.lua.

local burst = Proto("esdr_burst", "ESP-SDR burst")

-- The header of version 1, bytes.
local V1_LEN = 52

-- enum pcap_mod
local modulations = {
	[0] = "unknown",
	[1] = "CW",
	[2] = "OOK",
	[3] = "2-FSK",
	[4] = "4-FSK",
	[5] = "MSK",
	[6] = "BPSK",
	[7] = "QPSK",
	[8] = "wideband",
}

-- PCAP_BF_*: field name, label, bit, description.
local flag_bits = {
	{ "truncated", "Truncated", 0x0001, "Cut at the buffer's cap, the channel still open" },
	{ "wideband", "Wideband", 0x0002, "Several adjacent channels opened together: no bits" },
	{ "transient", "Transient", 0x0004, "The filterbank's transient among the samples" },
	{ "neighbour", "Neighbour open", 0x0008, "A neighbouring channel open at the same time" },
	{ "dropped", "Dropped before", 0x0010, "The C6 dropped windows before this burst" },
	{ "clipped", "Clipped", 0x0020, "Samples reached the receiver's full scale" },
	{ "bits", "Bits sliced", 0x0040, "The bits were sliced at the estimated symbol rate" },
	{ "cut", "Cut", 0x0080, "Still open when the capture stopped" },
	{ "hole", "Hole", 0x0100, "Ended at a hole in the C6's stream: it may go on in the next record" },
	{ "skipped", "Not analysed", 0x0200, "The analysis was busy: the detector's metadata only" },
}

local f = burst.fields
f.version = ProtoField.uint8("esdr_burst.version", "Version", base.DEC)
f.hdr_len = ProtoField.uint8("esdr_burst.hdr_len", "Header length", base.DEC)
f.flags = ProtoField.uint16("esdr_burst.flags", "Flags", base.HEX)
local flag_fields = {}
for i, b in ipairs(flag_bits) do
	flag_fields[i] = ProtoField.bool("esdr_burst.flags." .. b[1], b[2], 16, nil, b[3], b[4])
	f["flags_" .. b[1]] = flag_fields[i]
end
f.freq = ProtoField.uint64("esdr_burst.freq", "Frequency", base.DEC, nil, nil,
	"The channel's centre frequency, Hz")
f.offset = ProtoField.int32("esdr_burst.offset", "Offset", base.DEC, nil, nil,
	"The signal's centre against the channel's (its carrier offset), Hz")
f.bandwidth = ProtoField.uint32("esdr_burst.bandwidth", "Bandwidth", base.DEC, nil, nil,
	"Occupied bandwidth (90 % of the power), Hz; 0 unknown")
f.duration = ProtoField.uint32("esdr_burst.duration", "Duration", base.DEC, nil, nil,
	"From the first to the last loud sample, microseconds")
f.symbol_rate = ProtoField.uint32("esdr_burst.symbol_rate", "Symbol rate", base.DEC, nil, nil,
	"Symbols a second; 0 unknown")
f.deviation = ProtoField.uint32("esdr_burst.deviation", "Deviation", base.DEC, nil, nil,
	"FSK: half the spacing of the outer tones, Hz; 0 unknown or other")
f.modulation = ProtoField.uint8("esdr_burst.modulation", "Modulation", base.DEC, modulations)
f.rx_gain = ProtoField.uint8("esdr_burst.rx_gain", "Receive gain", base.DEC, nil, nil,
	"The radio's gain index, 255 automatic")
f.channel = ProtoField.uint8("esdr_burst.channel", "Channel", base.DEC, nil, nil,
	"The channelizer's channel index")
f.channels = ProtoField.uint8("esdr_burst.channels", "Channels", base.DEC, nil, nil,
	"The channelizer's channel count M")
f.power = ProtoField.int16("esdr_burst.power", "Power", base.DEC, nil, nil,
	"Mean power over the loud part, dBFS times 10")
f.floor = ProtoField.int16("esdr_burst.floor", "Floor", base.DEC, nil, nil,
	"The channel's noise floor, dBFS times 10")
f.peak = ProtoField.int16("esdr_burst.peak", "Peak", base.DEC, nil, nil,
	"The peak sample power, dBFS times 10")
f.reserved = ProtoField.uint16("esdr_burst.reserved", "Reserved", base.HEX)
f.sample_rate = ProtoField.uint32("esdr_burst.sample_rate", "Sample rate", base.DEC, nil, nil,
	"The rate of the stream the analysis ended at, Hz")
f.nbits = ProtoField.uint32("esdr_burst.nbits", "Bit count", base.DEC)
f.extra = ProtoField.bytes("esdr_burst.extra", "Fields of a later version")
f.bits = ProtoField.bytes("esdr_burst.bits", "Bits", base.NONE, "The demodulated bits, MSB first")

local e_short = ProtoExpert.new("esdr_burst.short", "Record shorter than its header and bits",
	expert.group.MALFORMED, expert.severity.ERROR)
burst.experts = { e_short }

-- @v divided by 10^@shift with @dec decimals, rounded half away from zero, the decimal point a
-- point whatever the locale (as the packet comments have it).
local function fixed(v, shift, dec)
	local neg = v < 0
	if neg then
		v = -v
	end
	local p = 10 ^ dec
	local q = math.floor(v / 10 ^ (shift - dec) + 0.5)
	local s = string.format("%d", math.floor(q / p))
	if dec > 0 then
		s = s .. "." .. string.format("%0" .. dec .. "d", q % p)
	end
	if neg and q ~= 0 then
		s = "-" .. s
	end
	return s
end

local function hz_text(label, hz, shift, unit)
	return string.format("%s: %s %s (%s Hz)", label, fixed(hz, shift, 3), unit, tostring(hz))
end

local function dbfs_text(label, x10)
	return string.format("%s: %s dBFS", label, fixed(x10, 1, 1))
end

function burst.dissector(tvb, pinfo, tree)
	local len = tvb:len()
	if len < 2 then
		return 0
	end
	pinfo.cols.protocol = "ESDR"
	local hdr_len = tvb(1, 1):uint()
	local avail = math.min(hdr_len, len)
	local t = tree:add(burst, tvb())
	-- A field is there when it lies within the header and the packet.
	local function has(off, n)
		return off + n <= avail
	end
	local function item(field, off, n)
		return t:add_le(field, tvb(off, n))
	end

	item(f.version, 0, 1)
	item(f.hdr_len, 1, 1)
	if has(2, 2) then
		local ft = item(f.flags, 2, 2)
		for _, field in ipairs(flag_fields) do
			ft:add_le(field, tvb(2, 2))
		end
	end
	local freq, modulation, rate, nbits, duration = 0, 0, 0, 0, 0
	if has(4, 8) then
		local v = tvb(4, 8):le_uint64()
		freq = v:tonumber()
		item(f.freq, 4, 8):set_text(string.format("Frequency: %s MHz (%s Hz)",
			fixed(freq, 6, 3), tostring(v)))
	end
	if has(12, 4) then
		item(f.offset, 12, 4):set_text(hz_text("Offset", tvb(12, 4):le_int(), 3, "kHz"))
	end
	if has(16, 4) then
		item(f.bandwidth, 16, 4):set_text(hz_text("Bandwidth", tvb(16, 4):le_uint(), 3,
			"kHz"))
	end
	if has(20, 4) then
		duration = tvb(20, 4):le_uint()
		item(f.duration, 20, 4):set_text(string.format("Duration: %d us", duration))
	end
	if has(24, 4) then
		rate = tvb(24, 4):le_uint()
		item(f.symbol_rate, 24, 4):set_text(string.format("Symbol rate: %d Bd", rate))
	end
	if has(28, 4) then
		item(f.deviation, 28, 4):set_text(hz_text("Deviation", tvb(28, 4):le_uint(), 3,
			"kHz"))
	end
	if has(32, 1) then
		modulation = tvb(32, 1):uint()
		item(f.modulation, 32, 1)
	end
	if has(33, 1) then
		local gi = item(f.rx_gain, 33, 1)
		if tvb(33, 1):uint() == 255 then
			gi:set_text("Receive gain: automatic (255)")
		end
	end
	if has(34, 1) then
		item(f.channel, 34, 1)
	end
	if has(35, 1) then
		item(f.channels, 35, 1)
	end
	if has(36, 2) then
		item(f.power, 36, 2):set_text(dbfs_text("Power", tvb(36, 2):le_int()))
	end
	if has(38, 2) then
		item(f.floor, 38, 2):set_text(dbfs_text("Floor", tvb(38, 2):le_int()))
	end
	if has(40, 2) then
		item(f.peak, 40, 2):set_text(dbfs_text("Peak", tvb(40, 2):le_int()))
	end
	if has(42, 2) then
		item(f.reserved, 42, 2)
	end
	if has(44, 4) then
		item(f.sample_rate, 44, 4):set_text(hz_text("Sample rate", tvb(44, 4):le_uint(), 6,
			"MS/s"))
	end
	if has(48, 4) then
		nbits = tvb(48, 4):le_uint()
		item(f.nbits, 48, 4)
	end
	if avail > V1_LEN then
		item(f.extra, V1_LEN, avail - V1_LEN)
	end

	local nbytes = math.floor((nbits + 7) / 8)
	local take = math.min(nbytes, math.max(len - hdr_len, 0))
	if take > 0 then
		t:add(f.bits, tvb(hdr_len, take))
	end
	if hdr_len < V1_LEN or hdr_len > len or take < nbytes then
		t:add_proto_expert_info(e_short)
	end

	local name = modulations[modulation] or string.format("modulation %d", modulation)
	t:append_text(string.format(", %s at %s MHz", name, fixed(freq, 6, 3)))
	pinfo.cols.info = string.format("%s %s MHz %d Bd %d bits %d us", name, fixed(freq, 6, 3),
		rate, nbits, duration)
	return len
end

DissectorTable.get("wtap_encap"):add((wtap_encaps or wtap).USER0, burst)
