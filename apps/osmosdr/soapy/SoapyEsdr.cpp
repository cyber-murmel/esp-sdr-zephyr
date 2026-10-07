/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SoapySDR module for the osmosdr app's USB protocol (esdr_proto.h), so GNU
 * Radio (gr-soapy), gqrx and SoapySDRUtil can use the board directly. Finds
 * and opens the device over libusb, same vendor requests and bulk blocks as
 * tools/esdr_usb.c. Half duplex: activating one direction stops the other.
 *
 * Build (CMakeLists.txt next to this file):
 *   cmake -B build && cmake --build build
 * then point SOAPY_SDR_PLUGIN_PATH at build/ (or install it into SoapySDR's
 * module directory).
 */

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>

#include <libusb.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "esdr_proto.h"

#define ESDR_VID 0x2fe3
#define ESDR_PID 0x0005

namespace {

/* Device handle plus the endpoints and serial, found once at enumeration or open. */
struct UsbEsdr {
	libusb_device_handle *h = nullptr;
	uint8_t ep_in = 0, ep_out = 0;
	std::string serial;
};

bool openByCriteria(libusb_context *ctx, const std::string &wantSerial, UsbEsdr &out)
{
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	bool found = false;

	for (ssize_t i = 0; i < n && !found; i++) {
		libusb_device_descriptor dd;
		libusb_device_handle *h;
		libusb_config_descriptor *cfg;
		char sn[64] = "";

		if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != ESDR_VID ||
		    dd.idProduct != ESDR_PID) {
			continue;
		}
		if (libusb_open(list[i], &h) != 0) {
			continue;
		}
		if (dd.iSerialNumber != 0) {
			libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, (unsigned char *)sn,
							   sizeof(sn));
		}
		if (!wantSerial.empty() && wantSerial != sn) {
			libusb_close(h);
			continue;
		}
		if (libusb_get_active_config_descriptor(list[i], &cfg) != 0) {
			libusb_close(h);
			continue;
		}
		for (int k = 0; k < cfg->bNumInterfaces && !found; k++) {
			const libusb_interface_descriptor *id = &cfg->interface[k].altsetting[0];

			if (id->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC || id->bNumEndpoints != 2) {
				continue;
			}
			if (libusb_claim_interface(h, id->bInterfaceNumber) != 0) {
				continue;
			}
			for (int e = 0; e < 2; e++) {
				uint8_t a = id->endpoint[e].bEndpointAddress;

				if (a & LIBUSB_ENDPOINT_IN) {
					out.ep_in = a;
				} else {
					out.ep_out = a;
				}
			}
			out.h = h;
			out.serial = sn;
			found = true;
		}
		libusb_free_config_descriptor(cfg);
		if (!found) {
			libusb_close(h);
		}
	}
	libusb_free_device_list(list, 1);
	return found;
}

int ctrlOut(libusb_device_handle *h, uint8_t req, uint16_t value, const void *data, uint16_t len)
{
	return libusb_control_transfer(h,
					LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
						LIBUSB_RECIPIENT_DEVICE,
					req, value, 0, (unsigned char *)data, len, 1000);
}

int ctrlIn(libusb_device_handle *h, uint8_t req, void *data, uint16_t len)
{
	return libusb_control_transfer(h,
					LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
						LIBUSB_RECIPIENT_DEVICE,
					req, 0, 0, (unsigned char *)data, len, 1000);
}

int setU32(libusb_device_handle *h, uint8_t req, uint32_t v)
{
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};

	return ctrlOut(h, req, 0, b, 4);
}

int setFreq(libusb_device_handle *h, uint64_t hz)
{
	uint8_t b[8];

	for (int i = 0; i < 8; i++) {
		b[i] = (uint8_t)(hz >> (8 * i));
	}
	return ctrlOut(h, ESDR_REQ_SET_FREQ, 0, b, 8);
}

/* Receive-side ring of converted samples, written by the USB callback thread. */
class SampleRing {
public:
	void reset(size_t itemBytes)
	{
		std::lock_guard<std::mutex> g(m_);
		itemBytes_ = itemBytes;
		buf_.assign(CAPACITY * itemBytes, 0);
		head_ = tail_ = 0;
		overflowed_ = false;
		dropped_ = 0;
	}

	/* Items dropped because the reader fell behind, since the last reset. */
	uint64_t dropped(void)
	{
		std::lock_guard<std::mutex> g(m_);
		return dropped_;
	}

	/* Appends n items (dropping the oldest on overflow, flagged for the next read). */
	void push(const uint8_t *data, size_t n)
	{
		std::lock_guard<std::mutex> g(m_);
		for (size_t k = 0; k < n; k++) {
			std::memcpy(&buf_[(head_ % CAPACITY) * itemBytes_], data + k * itemBytes_,
				   itemBytes_);
			head_++;
			if (head_ - tail_ > CAPACITY) {
				tail_ = head_ - CAPACITY;
				overflowed_ = true;
				dropped_++;
			}
		}
		cv_.notify_one();
	}

	/* Copies at most n items into out; returns how many, and whether an overflow happened since. */
	size_t pop(uint8_t *out, size_t n, int timeoutUs, bool &overflow)
	{
		std::unique_lock<std::mutex> lk(m_);
		if (head_ == tail_) {
			cv_.wait_for(lk, std::chrono::microseconds(timeoutUs),
				    [&] { return head_ != tail_; });
		}
		size_t avail = head_ - tail_;
		size_t k = std::min(avail, n);

		for (size_t i = 0; i < k; i++) {
			std::memcpy(out + i * itemBytes_, &buf_[(tail_ % CAPACITY) * itemBytes_],
				   itemBytes_);
			tail_++;
		}
		overflow = overflowed_;
		overflowed_ = false;
		return k;
	}

private:
	/* About 1 s at 250 kS/s: more only adds latency when the reader lags. */
	static constexpr size_t CAPACITY = 1u << 18; /* items */
	std::mutex m_;
	std::condition_variable cv_;
	std::vector<uint8_t> buf_;
	size_t itemBytes_ = 2;
	uint64_t head_ = 0, tail_ = 0, dropped_ = 0;
	bool overflowed_ = false;
};

} /* namespace */

class SoapyEsdr : public SoapySDR::Device {
public:
	explicit SoapyEsdr(const SoapySDR::Kwargs &args)
	{
		if (libusb_init(&ctx_) != 0) {
			throw std::runtime_error("esdr: libusb_init failed");
		}
		std::string serial = args.count("serial") ? args.at("serial") : "";

		if (!openByCriteria(ctx_, serial, usb_)) {
			libusb_exit(ctx_);
			throw std::runtime_error("esdr: device not found" +
						 (serial.empty() ? "" : " (serial " + serial + ")"));
		}
		struct esdr_info info{};

		if (ctrlIn(usb_.h, ESDR_REQ_GET_INFO, &info, sizeof(info)) == (int)sizeof(info) &&
		    info.magic == ESDR_INFO_MAGIC) {
			freqMinHz_ = (double)info.freq_min_hz;
			freqMaxHz_ = (double)info.freq_max_hz;
			rateMaxHz_ = (double)info.rx_rate_max_hz;
			rateSteps_ = info.rx_rate_count;
			rxGainMax_ = info.rx_gain_max;
			txGainMax_ = info.tx_gain_max;
		}
		/* Start from what the device runs with. */
		struct esdr_state st{};

		if (ctrlIn(usb_.h, ESDR_REQ_GET_STATE, &st, sizeof(st)) == (int)sizeof(st)) {
			freqHz_ = (double)st.freq_hz;
			rateHz_ = (double)st.sample_rate_hz;
			bwHz_ = (double)st.bandwidth_hz;
			rxAuto_ = st.rx_gain == ESDR_GAIN_AUTO;
			rxGain_ = rxAuto_ ? 0 : st.rx_gain;
			txGain_ = st.tx_gain;
			corrPpm_ = st.freq_corr_ppb / 1000.0;
		}
	}

	~SoapyEsdr() override
	{
		if (rxRunning_) {
			stopRx();
		}
		if (txRunning_) {
			stopTx();
		}
		if (usb_.h != nullptr) {
			libusb_release_interface(usb_.h, 0);
			libusb_close(usb_.h);
		}
		libusb_exit(ctx_);
	}

	/* ---- identification ---- */
	std::string getDriverKey(void) const override { return "esdr"; }
	std::string getHardwareKey(void) const override { return "ESP-SDR osmosdr"; }

	SoapySDR::Kwargs getHardwareInfo(void) const override
	{
		SoapySDR::Kwargs info;

		info["serial"] = usb_.serial;
		return info;
	}

	/* ---- channels ---- */
	size_t getNumChannels(const int) const override { return 1; }
	bool getFullDuplex(const int, const size_t) const override { return false; }

	/* ---- streaming ---- */
	std::vector<std::string> getStreamFormats(const int, const size_t) const override
	{
		return {SOAPY_SDR_CS8, SOAPY_SDR_CS16, SOAPY_SDR_CF32};
	}

	std::string getNativeStreamFormat(const int, const size_t, double &fullScale) const override
	{
		fullScale = 32767;
		return SOAPY_SDR_CS16;
	}

	SoapySDR::Stream *setupStream(const int direction, const std::string &format,
				      const std::vector<size_t> &channels,
				      const SoapySDR::Kwargs &) override
	{
		if (!channels.empty() && (channels.size() != 1 || channels[0] != 0)) {
			throw std::runtime_error("esdr: one channel only");
		}
		if (format != SOAPY_SDR_CS8 && format != SOAPY_SDR_CS16 && format != SOAPY_SDR_CF32) {
			throw std::runtime_error("esdr: format must be CS8, CS16 or CF32");
		}
		/* CF32 travels as 16-bit items. */
		if (direction == SOAPY_SDR_TX) {
			txFloat_ = format == SOAPY_SDR_CF32;
			txBits_ = (format == SOAPY_SDR_CS8) ? 8 : 16;
			return reinterpret_cast<SoapySDR::Stream *>(&txTag_);
		}
		float_ = format == SOAPY_SDR_CF32;
		bits_ = (format == SOAPY_SDR_CS8) ? 8 : 16;
		return reinterpret_cast<SoapySDR::Stream *>(&rxTag_);
	}

	void closeStream(SoapySDR::Stream *stream) override
	{
		deactivateStream(stream, 0, 0);
	}

	size_t getStreamMTU(SoapySDR::Stream *) const override { return 4096; }

	/* Half duplex: starting one direction stops the other. */
	int activateStream(SoapySDR::Stream *stream, const int, const long long, const size_t) override
	{
		if (isTx(stream)) {
			if (rxRunning_) {
				stopRx();
			}
			startTx();
		} else {
			if (txRunning_) {
				stopTx();
			}
			startRx();
		}
		return 0;
	}

	int deactivateStream(SoapySDR::Stream *stream, const int, const long long) override
	{
		if (isTx(stream)) {
			if (txRunning_) {
				stopTx();
			}
		} else if (rxRunning_) {
			stopRx();
		}
		return 0;
	}

	/*
	 * Blocking bulk OUT: the device holds the transfer back while its DAC
	 * backlog is full, which paces the caller to the sample rate.
	 */
	int writeStream(SoapySDR::Stream *, const void *const *buffs, const size_t numElems,
			int &flags, const long long, const long timeoutUs) override
	{
		size_t item = txBits_ == 16 ? 4 : 2;
		size_t n = std::min(numElems, txBuf_.size() / item);
		uint8_t *o = txBuf_.data();
		int done = 0;

		flags = 0;
		if (!txRunning_) {
			return SOAPY_SDR_STREAM_ERROR;
		}
		if (txFloat_) {
			const float *in = reinterpret_cast<const float *>(buffs[0]);

			for (size_t k = 0; k < 2 * n; k++) {
				float v = std::max(-1.0f, std::min(1.0f, in[k]));

				if (txBits_ == 16) {
					int16_t q = (int16_t)std::lrint(v * 32767.0f);

					o[2 * k] = (uint8_t)q;
					o[2 * k + 1] = (uint8_t)(q >> 8);
				} else {
					o[k] = (uint8_t)(int8_t)std::lrint(v * 127.0f);
				}
			}
		} else {
			std::memcpy(o, buffs[0], n * item);
		}
		int r = libusb_bulk_transfer(usb_.h, usb_.ep_out, o, (int)(n * item), &done,
					     (unsigned int)std::max(1L, timeoutUs / 1000));

		if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
			return SOAPY_SDR_STREAM_ERROR;
		}
		if (done == 0) {
			return SOAPY_SDR_TIMEOUT;
		}
		return done / (int)item;
	}

	int readStream(SoapySDR::Stream *, void *const *buffs, const size_t numElems,
		       int &flags, long long &timeNs, const long timeoutUs) override
	{
		bool overflow = false;
		size_t n;

		if (float_) {
			size_t want = std::min(numElems, conv_.size() / 2);
			float *out = reinterpret_cast<float *>(buffs[0]);

			n = ring_.pop(reinterpret_cast<uint8_t *>(conv_.data()), want, (int)timeoutUs,
				      overflow);
			for (size_t k = 0; k < 2 * n; k++) {
				out[k] = (float)conv_[k] * (1.0f / 32768.0f);
			}
		} else {
			n = ring_.pop(reinterpret_cast<uint8_t *>(buffs[0]), numElems, (int)timeoutUs,
				      overflow);
		}

		flags = 0;
		timeNs = 0;
		if (n == 0) {
			return SOAPY_SDR_TIMEOUT;
		}
		if (overflow) {
			flags |= SOAPY_SDR_END_BURST;
		}
		return (int)n;
	}

	/* ---- gains, frequency, rate, bandwidth: forwarded to the device ---- */
	std::vector<std::string> listGains(const int direction, const size_t) const override
	{
		if (direction == SOAPY_SDR_RX) {
			return {"RF"};
		}
		return {"TX"};
	}

	bool hasGainMode(const int direction, const size_t) const override
	{
		return direction == SOAPY_SDR_RX;
	}

	void setGainMode(const int direction, const size_t, const bool automatic) override
	{
		if (direction != SOAPY_SDR_RX) {
			return;
		}
		rxAuto_ = automatic;
		ctrlOut(usb_.h, ESDR_REQ_SET_RX_GAIN, automatic ? ESDR_GAIN_AUTO : (uint16_t)rxGain_,
			nullptr, 0);
	}

	bool getGainMode(const int direction, const size_t) const override
	{
		return direction == SOAPY_SDR_RX && rxAuto_;
	}

	void setGain(const int direction, const size_t channel, const double value) override
	{
		setGain(direction, channel, direction == SOAPY_SDR_RX ? "RF" : "TX", value);
	}

	void setGain(const int direction, const size_t, const std::string &, const double value) override
	{
		if (direction == SOAPY_SDR_RX) {
			rxAuto_ = false;
			rxGain_ = (int)std::max(0.0, std::min((double)rxGainMax_, value));
			ctrlOut(usb_.h, ESDR_REQ_SET_RX_GAIN, (uint16_t)rxGain_, nullptr, 0);
		} else {
			txGain_ = (int)std::max(0.0, std::min((double)txGainMax_, value));
			ctrlOut(usb_.h, ESDR_REQ_SET_TX_GAIN, (uint16_t)txGain_, nullptr, 0);
		}
	}

	double getGain(const int direction, const size_t) const override
	{
		return direction == SOAPY_SDR_RX ? (double)rxGain_ : (double)txGain_;
	}

	double getGain(const int direction, const size_t channel, const std::string &) const override
	{
		return getGain(direction, channel);
	}

	/* PHY gain table indices and TX power steps, not dB. */
	SoapySDR::Range getGainRange(const int direction, const size_t) const override
	{
		return SoapySDR::Range(0, direction == SOAPY_SDR_RX ? rxGainMax_ : txGainMax_, 1);
	}

	SoapySDR::Range getGainRange(const int direction, const size_t channel,
				     const std::string &) const override
	{
		return getGainRange(direction, channel);
	}

	/* One LO: a direction's frequency goes to the device while that direction runs. */
	void setFrequency(const int direction, const size_t, const double frequency,
			  const SoapySDR::Kwargs &) override
	{
		if (direction == SOAPY_SDR_TX) {
			txFreqHz_ = frequency;
			if (txRunning_) {
				setFreq(usb_.h, (uint64_t)frequency);
			}
			return;
		}
		freqHz_ = frequency;
		if (!txRunning_) {
			setFreq(usb_.h, (uint64_t)frequency);
		}
	}

	double getFrequency(const int direction, const size_t) const override
	{
		return direction == SOAPY_SDR_TX ? txFreqHz_ : freqHz_;
	}

	SoapySDR::RangeList getFrequencyRange(const int, const size_t) const override
	{
		return {SoapySDR::Range(freqMinHz_, freqMaxHz_)};
	}

	/*
	 * The device takes the nearest of its rates: report that one, or the
	 * application reads at a different rate than the samples arrive.
	 */
	void setSampleRate(const int direction, const size_t, const double rate) override
	{
		double actual = deviceRate(rate);

		if (actual != rate) {
			SoapySDR_logf(SOAPY_SDR_INFO, "esdr: %.0f S/s requested, %.0f S/s used", rate,
				      actual);
		}
		if (direction == SOAPY_SDR_TX) {
			txRateHz_ = actual;
			if (txRunning_) {
				setU32(usb_.h, ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)actual);
			}
			return;
		}
		rateHz_ = actual;
		if (!txRunning_) {
			setU32(usb_.h, ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)actual);
		}
	}

	/* The device's rate nearest to @p rate on a log scale (its own rounding). */
	double deviceRate(double rate) const
	{
		for (unsigned k = 0; k + 1 < rateSteps_; k++) {
			double r = rateMaxHz_ / (double)(1u << k);

			if (2.0 * rate * rate >= r * r) {
				return r;
			}
		}
		return rateMaxHz_ / (double)(1u << (rateSteps_ - 1));
	}

	double getSampleRate(const int direction, const size_t) const override
	{
		return direction == SOAPY_SDR_TX ? txRateHz_ : rateHz_;
	}

	std::vector<double> listSampleRates(const int, const size_t) const override
	{
		std::vector<double> rates;

		for (unsigned k = 0; k < rateSteps_; k++) {
			rates.push_back(rateMaxHz_ / (double)(1u << k));
		}
		return rates;
	}

	SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override
	{
		return {SoapySDR::Range(rateMaxHz_ / (double)(1u << (rateSteps_ - 1)), rateMaxHz_)};
	}

	void setBandwidth(const int, const size_t, const double bw) override
	{
		bwHz_ = bw;
		setU32(usb_.h, ESDR_REQ_SET_BANDWIDTH, (uint32_t)bw);
	}

	double getBandwidth(const int, const size_t) const override { return bwHz_; }

	bool hasFrequencyCorrection(const int direction, const size_t) const override
	{
		return direction == SOAPY_SDR_RX;
	}

	void setFrequencyCorrection(const int direction, const size_t, const double value) override
	{
		if (direction != SOAPY_SDR_RX) {
			return;
		}
		corrPpm_ = value;
		setU32(usb_.h, ESDR_REQ_SET_FREQ_CORR, (uint32_t)(int32_t)(value * 1000.0));
	}

	double getFrequencyCorrection(const int, const size_t) const override { return corrPpm_; }

private:
	bool isTx(SoapySDR::Stream *stream) const
	{
		return reinterpret_cast<const char *>(stream) == &txTag_;
	}

	void startTx()
	{
		ctrlOut(usb_.h, ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		setFreq(usb_.h, (uint64_t)txFreqHz_);
		setU32(usb_.h, ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)txRateHz_);
		ctrlOut(usb_.h, ESDR_REQ_SET_TX_GAIN, (uint16_t)txGain_, nullptr, 0);
		ctrlOut(usb_.h, ESDR_REQ_SET_FORMAT, (uint16_t)txBits_, nullptr, 0);
		ctrlOut(usb_.h, ESDR_REQ_SET_MODE, ESDR_MODE_TX, nullptr, 0);
		txRunning_ = true;
	}

	void stopTx()
	{
		ctrlOut(usb_.h, ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		txRunning_ = false;
	}

	void startRx()
	{
		size_t itemBytes = bits_ == 16 ? 4 : 2;

		ring_.reset(itemBytes);
		expect_ = 0;
		haveExpect_ = false;
		synced_ = false;
		carryLen_ = 0;
		rxSamples_ = gaps_ = gapZeros_ = resyncs_ = 0;
		ctrlOut(usb_.h, ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		setFreq(usb_.h, (uint64_t)freqHz_);
		setU32(usb_.h, ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)rateHz_);
		if (bwHz_ > 0) {
			setU32(usb_.h, ESDR_REQ_SET_BANDWIDTH, (uint32_t)bwHz_);
		}
		ctrlOut(usb_.h, ESDR_REQ_SET_RX_GAIN, rxAuto_ ? ESDR_GAIN_AUTO : (uint16_t)rxGain_,
			nullptr, 0);
		ctrlOut(usb_.h, ESDR_REQ_SET_FORMAT, (uint16_t)bits_, nullptr, 0);
		ctrlOut(usb_.h, ESDR_REQ_SET_MODE, ESDR_MODE_RX, nullptr, 0);

		rxStop_ = false;
		rxRunning_ = true;
		rxThread_ = std::thread(&SoapyEsdr::rxLoop, this);
	}

	void stopRx()
	{
		/* The device first: with no transfers queued it would only fill and overflow. */
		ctrlOut(usb_.h, ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		rxStop_ = true;
		if (rxThread_.joinable()) {
			rxThread_.join();
		}
		rxRunning_ = false;
		SoapySDR_logf(SOAPY_SDR_INFO,
			      "esdr: rx stopped: %llu samples, %llu gaps (%llu zero filled), "
			      "%llu dropped by a slow reader, %llu resync steps",
			      (unsigned long long)rxSamples_, (unsigned long long)gaps_,
			      (unsigned long long)gapZeros_, (unsigned long long)ring_.dropped(),
			      (unsigned long long)resyncs_);
	}

	/*
	 * Async bulk IN into fixed buffers, resubmitted as they complete. No
	 * timeout: queued transfers fill one after another, so a timeout counted
	 * from submission would expire on the later ones at low rates; each
	 * completed buffer is split into whole EBLK blocks (the device never
	 * splits one across transfers) and converted into the ring. A gap in
	 * the device's sample index is filled with zeros, bounded, so GRC/gqrx
	 * see a continuous time axis instead of a jump.
	 */
	void rxLoop()
	{
		static constexpr int XFERS = 8;
		static constexpr size_t XFER_BYTES = 16 * ESDR_BLOCK_BYTES;
		std::vector<libusb_transfer *> xfers(XFERS);
		std::vector<std::vector<uint8_t>> bufs(XFERS);

		auto cb = [](libusb_transfer *t) {
			auto *self = reinterpret_cast<SoapyEsdr *>(t->user_data);

			self->onTransfer(t);
			if (self->rxStop_ || libusb_submit_transfer(t) != 0) {
				self->rxPending_--;
			}
		};
		for (int k = 0; k < XFERS; k++) {
			bufs[k].resize(XFER_BYTES);
			xfers[k] = libusb_alloc_transfer(0);
			libusb_fill_bulk_transfer(xfers[k], usb_.h, usb_.ep_in, bufs[k].data(),
						  (int)XFER_BYTES, cb, this, 0);
			if (libusb_submit_transfer(xfers[k]) == 0) {
				rxPending_++;
			}
		}
		while (!rxStop_) {
			timeval tv{0, 50000};

			libusb_handle_events_timeout(ctx_, &tv);
		}
		for (int k = 0; k < XFERS; k++) {
			libusb_cancel_transfer(xfers[k]);
		}
		/* Until every cancelled transfer has come back (at most a second). */
		for (int tries = 0; tries < 50 && rxPending_ > 0; tries++) {
			timeval tv{0, 20000};

			libusb_handle_events_timeout(ctx_, &tv);
		}
		for (auto *x : xfers) {
			libusb_free_transfer(x);
		}
	}

	/* A plausible block header (the magic alone could occur in sample data). */
	static bool headerOk(const uint8_t *p)
	{
		struct esdr_block_hdr h{};

		std::memcpy(&h, p, sizeof(h));
		return h.magic == ESDR_BLOCK_MAGIC && h.samples <= ESDR_BLOCK_PAYLOAD / 2U &&
		       (h.flags & ~0x000fU) == 0U;
	}

	void onBlock(const uint8_t *p)
	{
		struct esdr_block_hdr h{};

		std::memcpy(&h, p, sizeof(h));
		if (haveExpect_ && h.index > expect_ && h.index - expect_ < 1000000U) {
			/* Missing samples become zeros: the time axis stays continuous. */
			static const uint8_t zeros[4 * 1024] = {0};
			size_t item = bits_ == 16 ? 4 : 2;

			for (uint64_t z = h.index - expect_; z > 0;) {
				uint64_t k = std::min<uint64_t>(z, sizeof(zeros) / item);

				ring_.push(zeros, (size_t)k);
				z -= k;
			}
			gaps_++;
			gapZeros_ += h.index - expect_;
		}
		ring_.push(p + sizeof(h), h.samples);
		rxSamples_ += h.samples;
		expect_ = h.index + h.samples;
		haveExpect_ = true;
	}

	/*
	 * Bulk data into whole blocks. Alignment is lost when an earlier session
	 * stopped mid-block; it is found again on the device's 64-byte packets.
	 */
	void onTransfer(libusb_transfer *t)
	{
		/* A cancelled transfer may still hold data: never drop what arrived. */
		if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_CANCELLED &&
		    t->status != LIBUSB_TRANSFER_TIMED_OUT) {
			return;
		}
		const uint8_t *p = t->buffer;
		size_t n = (size_t)t->actual_length;

		while (n > 0) {
			if (!synced_) {
				if (n < 64) {
					return;
				}
				if (headerOk(p)) {
					synced_ = true;
					carryLen_ = 0;
					continue;
				}
				p += 64;
				n -= 64;
				resyncs_++;
				continue;
			}
			if (carryLen_ > 0 || n < ESDR_BLOCK_BYTES) {
				size_t k = std::min(ESDR_BLOCK_BYTES - carryLen_, n);

				std::memcpy(carry_ + carryLen_, p, k);
				carryLen_ += k;
				p += k;
				n -= k;
				if (carryLen_ < ESDR_BLOCK_BYTES) {
					return;
				}
				carryLen_ = 0;
				if (!headerOk(carry_)) {
					synced_ = false;
					continue;
				}
				onBlock(carry_);
				continue;
			}
			if (!headerOk(p)) {
				synced_ = false;
				continue;
			}
			onBlock(p);
			p += ESDR_BLOCK_BYTES;
			n -= ESDR_BLOCK_BYTES;
		}
	}

	libusb_context *ctx_ = nullptr;
	UsbEsdr usb_;

	double freqMinHz_ = 100e6, freqMaxHz_ = 6000e6, rateMaxHz_ = 250e3;
	unsigned rateSteps_ = 5;
	unsigned rxGainMax_ = 82, txGainMax_ = 17;

	double freqHz_ = 100e6, rateHz_ = 250e3, bwHz_ = 0, corrPpm_ = 0;
	int rxGain_ = 0, txGain_ = 0;
	bool rxAuto_ = true;
	int bits_ = 16;
	bool float_ = false;
	/* Stream handles: addresses of these tags. */
	char rxTag_ = 0, txTag_ = 0;
	/* Transmit: own frequency and rate (the device has one of each, set at activation). */
	double txFreqHz_ = 2450e6, txRateHz_ = 250e3;
	int txBits_ = 16;
	bool txFloat_ = false, txRunning_ = false;
	std::vector<uint8_t> txBuf_ = std::vector<uint8_t>(16 * ESDR_BLOCK_BYTES);
	/* CF32 staging: CS16 items popped from the ring. */
	std::vector<int16_t> conv_ = std::vector<int16_t>(2 * 65536);

	std::thread rxThread_;
	std::atomic<bool> rxStop_{false};
	/* Bulk IN transfers submitted and not yet returned (USB event thread). */
	int rxPending_ = 0;
	bool rxRunning_ = false;
	/* Receive parser state and counters (USB event thread). */
	uint64_t rxSamples_ = 0, gaps_ = 0, gapZeros_ = 0, resyncs_ = 0;
	uint64_t expect_ = 0;
	bool haveExpect_ = false, synced_ = false;
	uint8_t carry_[ESDR_BLOCK_BYTES];
	size_t carryLen_ = 0;
	SampleRing ring_;
};

static SoapySDR::KwargsList findEsdr(const SoapySDR::Kwargs &args)
{
	SoapySDR::KwargsList result;
	libusb_context *ctx;

	if (libusb_init(&ctx) != 0) {
		return result;
	}
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);

	for (ssize_t i = 0; i < n; i++) {
		libusb_device_descriptor dd;
		libusb_device_handle *h;
		char sn[64] = "";

		if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != ESDR_VID ||
		    dd.idProduct != ESDR_PID) {
			continue;
		}
		if (libusb_open(list[i], &h) == 0) {
			if (dd.iSerialNumber != 0) {
				libusb_get_string_descriptor_ascii(h, dd.iSerialNumber,
								   (unsigned char *)sn, sizeof(sn));
			}
			libusb_close(h);
		}
		if (args.count("serial") && args.at("serial") != sn) {
			continue;
		}
		SoapySDR::Kwargs dev;

		dev["driver"] = "esdr";
		dev["serial"] = sn;
		dev["label"] = std::string("ESP-SDR osmosdr (") + sn + ")";
		result.push_back(dev);
	}
	libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return result;
}

static SoapySDR::Device *makeEsdr(const SoapySDR::Kwargs &args)
{
	return new SoapyEsdr(args);
}

static SoapySDR::Registry registerEsdr("esdr", &findEsdr, &makeEsdr, SOAPY_SDR_ABI_VERSION);
