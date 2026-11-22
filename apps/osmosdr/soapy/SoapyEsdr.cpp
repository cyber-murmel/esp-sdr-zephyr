/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SoapySDR module for the osmosdr app's protocol (esdr_proto.h), so GNU
 * Radio (gr-soapy), gqrx and SoapySDRUtil can use the board directly. Two
 * links: USB (the osmosdr app, found over libusb: vendor requests and bulk
 * blocks as tools/esdr_usb.c) and serial (the osmosdr_serial app, selected
 * with port=/dev/...: the same requests as frames on a byte stream). Half
 * duplex: activating one direction stops the other.
 *
 * A third link, VRT, is the ESP32-S31 apps/stream_eth board's VITA 49.2 receive stream over UDP
 * (vrt=<address>[:port], see VrtLink below): the vendor requests become VITA command packets and the
 * data packets are repacked as receive blocks, so everything after the link is shared.
 *
 * Stream argument trigger=<ratio> (a radio with ESDR_MODE_TRIGGER: the WHY2025 badge's C6):
 * the receive stream carries triggered snapshots instead, each ESDR_TRIG_SAMPLES at 80 MS/s
 * around a burst ratio times the quiet power, one after another (post=<samples> after the
 * trigger, default half); the sample rate setting does not apply.
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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "esdr_proto.h"
#include "vrt_wire.h"
#include <esp_sdr/esdr_link.h>

#define ESDR_VID 0x2fe3
#define ESDR_PID 0x0005

namespace {

/* Device handle plus the endpoints and serial, found once at enumeration or open. */
struct UsbEsdr {
	libusb_device_handle *h = nullptr;
	/* The vendor interface: 0 on osmosdr's own image, another under multiapp. */
	int iface = 0;
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
			out.iface = id->bInterfaceNumber;
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

/* Receive-side ring of converted samples, written by the USB callback thread. */
class SampleRing {
public:
	void reset(size_t itemBytes, double rateHz)
	{
		std::lock_guard<std::mutex> g(m_);
		itemBytes_ = itemBytes;
		capacity_ = std::max(MIN_CAPACITY, (size_t)(rateHz * BACKLOG_S));
		buf_.assign(capacity_ * itemBytes, 0);
		head_ = tail_ = 0;
		overflowed_ = false;
		dropped_ = 0;
	}

	/* Samples went that the ring never held: the next read that returns any says so. */
	void flagLoss(void)
	{
		std::lock_guard<std::mutex> g(m_);
		overflowed_ = true;
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
		for (size_t done = 0; done < n;) {
			size_t at = head_ % capacity_;
			size_t k = std::min(n - done, capacity_ - at);

			std::memcpy(&buf_[at * itemBytes_], data + done * itemBytes_, k * itemBytes_);
			head_ += k;
			done += k;
		}
		if (head_ - tail_ > capacity_) {
			dropped_ += head_ - tail_ - capacity_;
			tail_ = head_ - capacity_;
			overflowed_ = true;
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

		for (size_t done = 0; done < k;) {
			size_t at = tail_ % capacity_;
			size_t m = std::min(k - done, capacity_ - at);

			std::memcpy(out + done * itemBytes_, &buf_[at * itemBytes_], m * itemBytes_);
			tail_ += m;
			done += m;
		}
		/* An empty read is a timeout, with no flags to carry this: it stays for the next. */
		overflow = overflowed_ && k > 0;
		if (k > 0) {
			overflowed_ = false;
		}
		return k;
	}

private:
	/* About 1 s at 250 kS/s: more only adds latency when the reader lags. */
	static constexpr size_t MIN_CAPACITY = 1u << 18; /* items */
	/* At high speed a burst of a reader's lag is the same time, not the same items. */
	static constexpr double BACKLOG_S = 0.1;
	size_t capacity_ = MIN_CAPACITY;
	std::mutex m_;
	std::condition_variable cv_;
	std::vector<uint8_t> buf_;
	size_t itemBytes_ = 2;
	uint64_t head_ = 0, tail_ = 0, dropped_ = 0;
	bool overflowed_ = false;
};

/* Requests and sample transport: USB (osmosdr) or a serial byte stream (osmosdr_serial). */
class Link {
public:
	using RxFn = std::function<void(const uint8_t *, size_t)>;

	virtual ~Link() = default;
	/* Data length, or negative on error. */
	virtual int ctrlOut(uint8_t req, uint16_t value, const void *data, uint16_t len) = 0;
	virtual int ctrlIn(uint8_t req, void *data, uint16_t len) = 0;
	/* Transmit bytes: how many were taken (0 on timeout), or negative on error. */
	virtual int writeSamples(const uint8_t *p, size_t n, long timeoutUs) = 0;
	/* Receive: the stream's bytes go to fn (in order, any split) until stopRx(). */
	virtual void startRx(RxFn fn) = 0;
	virtual void stopRx() = 0;
	virtual std::string serial() const = 0;
	/* Link statistics as sensors (the VRT link's loss counters), and a line for the log. */
	virtual std::vector<std::string> sensorKeys() const { return {}; }
	virtual bool readSensor(const std::string &, std::string &) const { return false; }
	virtual std::string statusLine() const { return ""; }
};

class UsbLink : public Link {
public:
	explicit UsbLink(const std::string &serial)
	{
		if (libusb_init(&ctx_) != 0) {
			throw std::runtime_error("esdr: libusb_init failed");
		}
		if (!openByCriteria(ctx_, serial, usb_)) {
			libusb_exit(ctx_);
			throw std::runtime_error("esdr: device not found" +
						 (serial.empty() ? "" : " (serial " + serial + ")"));
		}
	}

	~UsbLink() override
	{
		stopRx();
		libusb_release_interface(usb_.h, usb_.iface);
		libusb_close(usb_.h);
		libusb_exit(ctx_);
	}

	int ctrlOut(uint8_t req, uint16_t value, const void *data, uint16_t len) override
	{
		return libusb_control_transfer(usb_.h,
					       LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
						       LIBUSB_RECIPIENT_DEVICE,
					       req, value, 0, (unsigned char *)data, len, 1000);
	}

	int ctrlIn(uint8_t req, void *data, uint16_t len) override
	{
		return libusb_control_transfer(usb_.h,
					       LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
						       LIBUSB_RECIPIENT_DEVICE,
					       req, 0, 0, (unsigned char *)data, len, 1000);
	}

	/* Blocking bulk OUT: the device holds the transfer back while its backlog is full. */
	int writeSamples(const uint8_t *p, size_t n, long timeoutUs) override
	{
		int done = 0;
		int r = libusb_bulk_transfer(usb_.h, usb_.ep_out, const_cast<uint8_t *>(p), (int)n,
					     &done, (unsigned int)std::max(1L, timeoutUs / 1000));

		if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
			return -1;
		}
		return done;
	}

	void startRx(RxFn fn) override
	{
		fn_ = std::move(fn);
		stop_ = false;
		thread_ = std::thread(&UsbLink::rxLoop, this);
	}

	void stopRx() override
	{
		stop_ = true;
		if (thread_.joinable()) {
			thread_.join();
		}
	}

	std::string serial() const override { return usb_.serial; }

private:
	/*
	 * Async bulk IN into fixed buffers, resubmitted as they complete. No
	 * timeout: queued transfers fill one after another, so a timeout counted
	 * from submission would expire on the later ones at low rates.
	 */
	void rxLoop()
	{
		static constexpr int XFERS = 32;
		static constexpr size_t XFER_BYTES = 16 * ESDR_BLOCK_BYTES;
		std::vector<libusb_transfer *> xfers(XFERS);
		std::vector<std::vector<uint8_t>> bufs(XFERS);

		auto cb = [](libusb_transfer *t) {
			auto *self = reinterpret_cast<UsbLink *>(t->user_data);

			/* A cancelled transfer may still hold data: never drop what arrived. */
			if (t->status == LIBUSB_TRANSFER_COMPLETED ||
			    t->status == LIBUSB_TRANSFER_CANCELLED ||
			    t->status == LIBUSB_TRANSFER_TIMED_OUT) {
				self->fn_(t->buffer, (size_t)t->actual_length);
			}
			if (self->stop_ || libusb_submit_transfer(t) != 0) {
				self->pending_--;
			}
		};
		pending_ = 0;
		for (int k = 0; k < XFERS; k++) {
			bufs[k].resize(XFER_BYTES);
			xfers[k] = libusb_alloc_transfer(0);
			libusb_fill_bulk_transfer(xfers[k], usb_.h, usb_.ep_in, bufs[k].data(),
						  (int)XFER_BYTES, cb, this, 0);
			if (libusb_submit_transfer(xfers[k]) == 0) {
				pending_++;
			}
		}
		while (!stop_) {
			timeval tv{0, 50000};

			libusb_handle_events_timeout(ctx_, &tv);
		}
		for (int k = 0; k < XFERS; k++) {
			libusb_cancel_transfer(xfers[k]);
		}
		/* Until every cancelled transfer has come back (at most a second). */
		for (int tries = 0; tries < 50 && pending_ > 0; tries++) {
			timeval tv{0, 20000};

			libusb_handle_events_timeout(ctx_, &tv);
		}
		for (auto *x : xfers) {
			libusb_free_transfer(x);
		}
	}

	libusb_context *ctx_ = nullptr;
	UsbEsdr usb_;
	RxFn fn_;
	std::thread thread_;
	std::atomic<bool> stop_{true};
	int pending_ = 0;
};

/*
 * The serial transport: request frames and their replies, receive blocks in
 * between (esdr_proto.h). One reader thread sorts what arrives. The port is
 * opened raw without touching DTR/RTS: on USB-Serial-JTAG that resets the chip.
 * A line rate (baud=) matters behind a USB bridge (the MCH2022 badge's RP2040
 * passes it on to the ESP32's UART); USB-Serial-JTAG ignores it.
 */
static speed_t baudSpeed(unsigned long baud)
{
	switch (baud) {
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 1500000: return B1500000;
	case 2000000: return B2000000;
	case 3000000: return B3000000;
	default: throw std::runtime_error("esdr: baud " + std::to_string(baud) + " not supported");
	}
}

class SerialLink : public Link {
public:
	explicit SerialLink(const std::string &port, unsigned long baud = 0) : port_(port)
	{
		fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY);
		if (fd_ < 0) {
			throw std::runtime_error("esdr: cannot open " + port);
		}
		termios a{};

		tcgetattr(fd_, &a);
		cfmakeraw(&a);
		a.c_cflag |= CLOCAL | CREAD;
		a.c_cflag &= ~HUPCL;
		a.c_cc[VMIN] = 0;
		a.c_cc[VTIME] = 1;
		if (baud != 0) {
			cfsetspeed(&a, baudSpeed(baud));
		}
		tcsetattr(fd_, TCSANOW, &a);
		tcflush(fd_, TCIOFLUSH);
		/*
		 * A previous host may have stopped inside a transmit frame: zeros
		 * complete any payload, then the device resynchronizes on the magic.
		 */
		std::vector<uint8_t> zeros(sizeof(esdr_ser_hdr) + 4096, 0);

		writeAll(zeros.data(), zeros.size(), 0);
		reader_ = std::thread(&SerialLink::readLoop, this);
	}

	~SerialLink() override
	{
		stop_ = true;
		if (reader_.joinable()) {
			reader_.join();
		}
		::close(fd_);
	}

	int ctrlOut(uint8_t req, uint16_t value, const void *data, uint16_t len) override
	{
		std::vector<uint8_t> rep;

		return request(req, 0, value, data, len, rep);
	}

	int ctrlIn(uint8_t req, void *data, uint16_t len) override
	{
		std::vector<uint8_t> rep;
		int r = request(req, ESDR_SER_IN, 0, nullptr, len, rep);

		if (r < 0) {
			return r;
		}
		size_t n = std::min<size_t>(rep.size(), len);

		std::memcpy(data, rep.data(), n);
		return (int)n;
	}

	/* Frames of transmit data; the device's flow control paces the writes. */
	int writeSamples(const uint8_t *p, size_t n, long timeoutUs) override
	{
		size_t done = 0;

		while (done < n) {
			uint16_t k = (uint16_t)std::min<size_t>(n - done, 4096);
			esdr_ser_hdr h{ESDR_SER_MAGIC, ESDR_SER_TX_DATA, 0, 0, k, 0};
			std::lock_guard<std::mutex> g(wm_);

			if (!writeAll(&h, sizeof(h), timeoutUs) || !writeAll(p + done, k, timeoutUs)) {
				return done > 0 ? (int)done : -1;
			}
			done += k;
		}
		return (int)done;
	}

	void startRx(RxFn fn) override
	{
		std::lock_guard<std::mutex> g(rxm_);
		fn_ = std::move(fn);
	}

	void stopRx() override
	{
		std::lock_guard<std::mutex> g(rxm_);
		fn_ = nullptr;
	}

	std::string serial() const override { return port_; }

private:
	bool writeAll(const void *p, size_t n, long timeoutUs)
	{
		const uint8_t *b = static_cast<const uint8_t *>(p);
		int ms = timeoutUs > 0 ? (int)std::max(1L, timeoutUs / 1000) : 2000;

		while (n > 0) {
			pollfd pf{fd_, POLLOUT, 0};

			if (poll(&pf, 1, ms) <= 0) {
				return false;
			}
			ssize_t w = ::write(fd_, b, n);

			if (w < 0) {
				return false;
			}
			b += w;
			n -= (size_t)w;
		}
		return true;
	}

	int request(uint8_t req, uint8_t flags, uint16_t value, const void *data, uint16_t len,
		    std::vector<uint8_t> &rep)
	{
		std::unique_lock<std::mutex> lk(cm_);
		esdr_ser_hdr h{ESDR_SER_MAGIC, req, flags, value, len, 0};

		gotReply_ = false;
		wantReq_ = req;
		{
			std::lock_guard<std::mutex> g(wm_);

			if (!writeAll(&h, sizeof(h), 0) ||
			    ((flags & ESDR_SER_IN) == 0 && len > 0 && !writeAll(data, len, 0))) {
				return -1;
			}
		}
		if (!cv_.wait_for(lk, std::chrono::seconds(3), [&] { return gotReply_; })) {
			SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: no reply to request %u", req);
			return -1;
		}
		if (replyStatus_ != 0) {
			return replyStatus_;
		}
		rep = reply_;
		return (flags & ESDR_SER_IN) != 0 ? (int)rep.size() : (int)len;
	}

	/* Sort the stream into reply frames and receive blocks; skip anything else byte by byte. */
	void readLoop()
	{
		std::vector<uint8_t> buf;
		uint8_t tmp[65536];

		while (!stop_) {
			pollfd pf{fd_, POLLIN, 0};

			if (poll(&pf, 1, 100) <= 0) {
				continue;
			}
			ssize_t r = ::read(fd_, tmp, sizeof(tmp));

			if (r <= 0) {
				continue;
			}
			buf.insert(buf.end(), tmp, tmp + r);
			size_t at = 0;

			while (buf.size() - at >= 4) {
				uint32_t magic;

				std::memcpy(&magic, &buf[at], 4);
				if (magic == ESDR_BLOCK_MAGIC) {
					if (buf.size() - at < ESDR_BLOCK_BYTES) {
						break;
					}
					{
						std::lock_guard<std::mutex> g(rxm_);

						if (fn_) {
							fn_(&buf[at], ESDR_BLOCK_BYTES);
						}
					}
					at += ESDR_BLOCK_BYTES;
				} else if (magic == ESDR_SER_MAGIC) {
					esdr_ser_hdr h;

					if (buf.size() - at < sizeof(h)) {
						break;
					}
					std::memcpy(&h, &buf[at], sizeof(h));
					if (buf.size() - at < sizeof(h) + h.len) {
						break;
					}
					{
						std::lock_guard<std::mutex> g(cm_);

						if (h.req == wantReq_) {
							reply_.assign(&buf[at + sizeof(h)],
								      &buf[at + sizeof(h) + h.len]);
							replyStatus_ = h.status;
							gotReply_ = true;
							cv_.notify_all();
						}
					}
					at += sizeof(h) + h.len;
				} else {
					at++;
				}
			}
			buf.erase(buf.begin(), buf.begin() + (long)at);
		}
	}

	std::string port_;
	int fd_ = -1;
	std::thread reader_;
	std::atomic<bool> stop_{false};
	/* Writes (frames stay whole), requests (one at a time), receive callback. */
	std::mutex wm_, cm_, rxm_;
	std::condition_variable cv_;
	bool gotReply_ = false;
	uint8_t wantReq_ = 0;
	int16_t replyStatus_ = 0;
	std::vector<uint8_t> reply_;
	RxFn fn_;
};

} /* namespace */

/*
 * The framed link (esp_sdr/esdr_link.h) through the MCH2022 badge's RP2040 running
 * apps/mch2022_coproc: its bulk interface 6, blocks through the FPGA where its bitstream allows,
 * else up to 5 Mbaud on the ESP32's UART. One reader thread sorts the frames; blocks come by their
 * length and go on padded to the size the callers expect.
 */
static constexpr uint16_t COPROC_VID = 0x16D0, COPROC_PID = 0x0F9A;
static constexpr uint8_t COPROC_IF_LINK = 6, COPROC_EP_OUT = 0x07, COPROC_EP_IN = 0x87;
/* esp_sdr/mch2022_coproc.h */
static constexpr uint8_t COPROC_REQ_SET_BAUD = 0x10, COPROC_REQ_ESP32_RESET = 0x20,
			 COPROC_REQ_LINK_OPEN = 0x30, COPROC_REQ_LINK_CLOSE = 0x31,
			 COPROC_REQ_FPGA_FASTLINK = 0x40;
static constexpr uint32_t LINK_BOOT_BAUD = 2000000;
/* The FPGA's fast line, in units of 100 kbaud. */
static constexpr uint16_t LINK_FAST_RATE = 150;
/*
 * Wire bytes the device may have on the way: what the RP2040's ring takes if USB stops, on the UART;
 * the FPGA's FIFO and the larger ring behind it, through the FPGA.
 */
static constexpr uint32_t LINK_WINDOW = 16384, LINK_WINDOW_FAST = ESDR_LINK_FPGA_FIFO,
			  LINK_CREDIT_STEP = 4096;

/* A badge with interface 6, of this serial if one is given. */
static bool openCoproc(libusb_context *ctx, const std::string &wantSerial, libusb_device_handle **out,
		       std::string *serial)
{
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	bool found = false;

	for (ssize_t i = 0; i < n && !found; i++) {
		libusb_device_descriptor dd;
		libusb_config_descriptor *cfg;
		libusb_device_handle *h;
		char sn[64] = "";

		if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != COPROC_VID ||
		    dd.idProduct != COPROC_PID || libusb_get_active_config_descriptor(list[i], &cfg) != 0) {
			continue;
		}
		bool has_link = cfg->bNumInterfaces > COPROC_IF_LINK;

		libusb_free_config_descriptor(cfg);
		if (!has_link || libusb_open(list[i], &h) != 0) {
			continue;
		}
		if (dd.iSerialNumber != 0) {
			libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, (unsigned char *)sn,
							   sizeof(sn));
		}
		if ((!wantSerial.empty() && wantSerial != sn) ||
		    (out != nullptr && libusb_claim_interface(h, COPROC_IF_LINK) != 0)) {
			libusb_close(h);
			continue;
		}
		if (serial != nullptr) {
			*serial = sn;
		}
		if (out != nullptr) {
			*out = h;
		} else {
			libusb_close(h);
		}
		found = true;
	}
	libusb_free_device_list(list, 1);
	return found;
}

class FramedLink : public Link {
public:
	FramedLink(const std::string &serial, unsigned long baud, bool fast)
	{
		if (libusb_init(&ctx_) != 0) {
			throw std::runtime_error("esdr: libusb_init failed");
		}
		if (!openCoproc(ctx_, serial, &h_, &serial_)) {
			libusb_exit(ctx_);
			throw std::runtime_error("esdr: no MCH2022 badge with interface 6");
		}
		reader_ = std::thread(&FramedLink::readLoop, this);
		decoder_ = std::thread(&FramedLink::decodeLoop, this);
		uint32_t rate = LINK_BOOT_BAUD;

		coproc(COPROC_REQ_SET_BAUD, 0, &rate, sizeof(rate));
		coproc(COPROC_REQ_LINK_OPEN, 0, nullptr, 0);
		if (fast) {
			coproc(COPROC_REQ_FPGA_FASTLINK, LINK_FAST_RATE, nullptr, 0);
		}
		wantFast_ = fast;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (!hello(3)) {
			/* At another rate after a host that went away, or still booting: anew. */
			coproc(COPROC_REQ_ESP32_RESET, 0, nullptr, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
			if (!hello(5)) {
				shutdown();
				throw std::runtime_error("esdr: no answer from the ESP32");
			}
		}
		if (fast && !fast_) {
			/* No link bitstream on the FPGA: its pins back to its UART. */
			coproc(COPROC_REQ_FPGA_FASTLINK, 0, nullptr, 0);
		}
		if (fast_) {
			/* The UART only carries requests now. */
			const uint32_t more = LINK_WINDOW_FAST - LINK_WINDOW;

			sendFrame(ESDR_LINK_CREDIT, &more, sizeof(more), nullptr, 0);
		} else if (baud != 0 && baud != LINK_BOOT_BAUD && !setBaud((uint32_t)baud)) {
			SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: staying at %u baud", baud_);
		}
	}

	~FramedLink() override
	{
		if (baud_ != LINK_BOOT_BAUD) {
			setBaud(LINK_BOOT_BAUD);
		}
		sendFrame(ESDR_LINK_BYE, nullptr, 0, nullptr, 0);
		coproc(COPROC_REQ_LINK_CLOSE, 0, nullptr, 0);
		shutdown();
	}

	int ctrlOut(uint8_t req, uint16_t value, const void *data, uint16_t len) override
	{
		std::vector<uint8_t> rep;

		return request(req, 0, value, data, len, rep);
	}

	int ctrlIn(uint8_t req, void *data, uint16_t len) override
	{
		std::vector<uint8_t> rep;
		int r = request(req, ESDR_SER_IN, 0, nullptr, len, rep);

		if (r < 0) {
			return r;
		}
		size_t n = std::min<size_t>(rep.size(), len);

		std::memcpy(data, rep.data(), n);
		return (int)n;
	}

	/* No transmit on the badge's ESP32. */
	int writeSamples(const uint8_t *, size_t, long) override { return -1; }

	void startRx(RxFn fn) override
	{
		std::lock_guard<std::mutex> g(rxm_);
		fn_ = std::move(fn);
	}

	void stopRx() override
	{
		std::lock_guard<std::mutex> g(rxm_);
		fn_ = nullptr;
	}

	std::string serial() const override { return serial_; }

private:
	void shutdown()
	{
		stop_ = true;
		if (reader_.joinable()) {
			reader_.join();
		}
		rxqcv_.notify_all();
		if (decoder_.joinable()) {
			decoder_.join();
		}
		libusb_release_interface(h_, COPROC_IF_LINK);
		libusb_close(h_);
		libusb_exit(ctx_);
	}

	void coproc(uint8_t req, uint16_t value, void *data, uint16_t len)
	{
		libusb_control_transfer(h_, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
					    LIBUSB_RECIPIENT_INTERFACE,
					req, value, COPROC_IF_LINK, (unsigned char *)data, len, 1000);
	}

	bool write(const uint8_t *p, size_t n)
	{
		int done = 0;

		if (libusb_bulk_transfer(h_, COPROC_EP_OUT, const_cast<uint8_t *>(p), (int)n, &done,
					 2000) != 0) {
			return false;
		}
		/* The firmware's buffer completes at a short packet (esp_sdr/mch2022_coproc.h). */
		if (n % 64U == 0U) {
			libusb_bulk_transfer(h_, COPROC_EP_OUT, const_cast<uint8_t *>(p), 0, &done, 2000);
		}
		return true;
	}

	bool sendFrame(uint8_t type, const void *a, size_t alen, const void *b, size_t blen)
	{
		std::vector<uint8_t> wire(ESDR_LINK_ENCODED_MAX(alen + blen) + 1U);
		std::lock_guard<std::mutex> g(wm_);
		size_t n = esdr_link_encode(wire.data(), wire.size(), type, txSeq_++, a, alen, b, blen);

		return n != 0U && write(wire.data(), n);
	}

	bool hello(int tries)
	{
		const esdr_link_hello h{ESDR_LINK_VERSION,
					(uint8_t)(wantFast_ ? ESDR_LINK_HELLO_F_FAST : 0U), 0,
					LINK_WINDOW, 0};
		const uint8_t zero = 0;

		for (int k = 0; k < tries; k++) {
			std::unique_lock<std::mutex> lk(cm_);

			gotHello_ = false;
			unacked_ = 0;
			{
				std::lock_guard<std::mutex> g(wm_);

				/* A zero first ends whatever the device had before this frame. */
				write(&zero, 1);
			}
			sendFrame(ESDR_LINK_HELLO, &h, sizeof(h), nullptr, 0);
			if (cv_.wait_for(lk, std::chrono::milliseconds(300), [&] { return gotHello_; })) {
				return true;
			}
		}
		return false;
	}

	bool setBaud(uint32_t rate)
	{
		std::vector<uint8_t> rep;
		uint32_t r = rate;

		wantFast_ = false;
		if (request(ESDR_REQ_SET_LINK_BAUD, 0, 0, &r, sizeof(r), rep) < 0) {
			return false;
		}
		coproc(COPROC_REQ_SET_BAUD, 0, &r, sizeof(r));
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (!hello(3)) {
			return false;
		}
		baud_ = rate;
		return true;
	}

	int request(uint8_t req, uint8_t flags, uint16_t value, const void *data, uint16_t len,
		    std::vector<uint8_t> &rep)
	{
		std::unique_lock<std::mutex> lk(cm_);
		esdr_ser_hdr h{ESDR_SER_MAGIC, req, flags, value, len, 0};

		gotReply_ = false;
		wantReq_ = req;
		if (!sendFrame(ESDR_LINK_REQUEST, &h, sizeof(h), data,
			       (flags & ESDR_SER_IN) == 0 ? len : 0U)) {
			return -1;
		}
		if (!cv_.wait_for(lk, std::chrono::seconds(3), [&] { return gotReply_; })) {
			SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: no reply to request %u", req);
			return -1;
		}
		if (replyStatus_ != 0) {
			return replyStatus_;
		}
		rep = reply_;
		return (flags & ESDR_SER_IN) != 0 ? (int)rep.size() : (int)len;
	}

	void onFrame(uint8_t *wire, size_t n)
	{
		esdr_link_frame f;

		unacked_ += (uint32_t)n + 1U;
		if (unacked_ >= LINK_CREDIT_STEP) {
			uint32_t grant = unacked_;

			unacked_ = 0;
			sendFrame(ESDR_LINK_CREDIT, &grant, sizeof(grant), nullptr, 0);
		}
		if (esdr_link_decode(wire, n, &f) != 0) {
			badFrames_++;
			return;
		}
		if (f.type == ESDR_LINK_BLOCK && f.len >= sizeof(esdr_block_hdr) &&
		    f.len <= ESDR_BLOCK_BYTES) {
			std::memcpy(block_, f.payload, f.len);
			std::memset(block_ + f.len, 0, ESDR_BLOCK_BYTES - f.len);
			std::lock_guard<std::mutex> g(rxm_);

			if (fn_) {
				fn_(block_, ESDR_BLOCK_BYTES);
			}
		} else if (f.type == ESDR_LINK_REPLY && f.len >= sizeof(esdr_ser_hdr)) {
			esdr_ser_hdr h;

			std::memcpy(&h, f.payload, sizeof(h));
			std::lock_guard<std::mutex> g(cm_);

			if (h.req == wantReq_) {
				reply_.assign(f.payload + sizeof(h), f.payload + f.len);
				replyStatus_ = h.status;
				gotReply_ = true;
				cv_.notify_all();
			}
		} else if (f.type == ESDR_LINK_HELLO) {
			std::lock_guard<std::mutex> g(cm_);
			esdr_link_hello h{};

			std::memcpy(&h, f.payload, std::min<size_t>(f.len, sizeof(h)));
			fast_ = (h.flags & ESDR_LINK_HELLO_F_FAST) != 0U;
			gotHello_ = true;
			cv_.notify_all();
		}
	}

	/*
	 * Bulk IN by queued transfers, as UsbLink's: one read at a time leaves USB idle between
	 * them, which at the stream's rate is a tenth of what it carries. What comes goes to
	 * decodeLoop(): the credit it sends is a synchronous write, which a transfer's callback
	 * must not do. The credit also bounds what can wait there.
	 */
	void readLoop()
	{
		static constexpr int XFERS = 4;
		static constexpr size_t XFER_BYTES = 16384;
		std::vector<libusb_transfer *> xfers(XFERS);
		std::vector<std::vector<uint8_t>> bufs(XFERS);

		auto cb = [](libusb_transfer *t) {
			auto *self = reinterpret_cast<FramedLink *>(t->user_data);

			if (t->actual_length > 0) {
				std::lock_guard<std::mutex> g(self->rxqm_);

				self->rxq_.emplace_back(t->buffer, t->buffer + t->actual_length);
				self->rxqcv_.notify_one();
			}
			if (self->stop_ || libusb_submit_transfer(t) != 0) {
				self->pending_--;
			}
		};
		pending_ = 0;
		for (int k = 0; k < XFERS; k++) {
			bufs[k].resize(XFER_BYTES);
			xfers[k] = libusb_alloc_transfer(0);
			libusb_fill_bulk_transfer(xfers[k], h_, COPROC_EP_IN, bufs[k].data(), (int)XFER_BYTES,
						  cb, this, 0);
			if (libusb_submit_transfer(xfers[k]) == 0) {
				pending_++;
			}
		}
		while (!stop_) {
			timeval tv{0, 50000};

			libusb_handle_events_timeout(ctx_, &tv);
		}
		for (int k = 0; k < XFERS; k++) {
			libusb_cancel_transfer(xfers[k]);
		}
		/* Until every cancelled transfer has come back (at most a second). */
		for (int tries = 0; tries < 50 && pending_ > 0; tries++) {
			timeval tv{0, 20000};

			libusb_handle_events_timeout(ctx_, &tv);
		}
		for (auto *x : xfers) {
			libusb_free_transfer(x);
		}
	}

	void decodeLoop()
	{
		std::vector<uint8_t> frame;

		frame.reserve(ESDR_LINK_ENCODED_MAX(ESDR_BLOCK_BYTES));
		for (;;) {
			std::vector<uint8_t> chunk;
			{
				std::unique_lock<std::mutex> g(rxqm_);

				rxqcv_.wait(g, [this] { return stop_ || !rxq_.empty(); });
				if (rxq_.empty()) {
					return;
				}
				chunk = std::move(rxq_.front());
				rxq_.pop_front();
			}
			for (const uint8_t b : chunk) {
				if (b != 0U) {
					frame.push_back(b);
				} else if (!frame.empty()) {
					onFrame(frame.data(), frame.size());
					frame.clear();
				}
			}
		}
	}

	libusb_context *ctx_ = nullptr;
	libusb_device_handle *h_ = nullptr;
	std::string serial_;
	std::thread reader_, decoder_;
	std::atomic<bool> stop_{false};
	int pending_ = 0;
	/* What the transfers brought, for decodeLoop(). */
	std::deque<std::vector<uint8_t>> rxq_;
	std::mutex rxqm_;
	std::condition_variable rxqcv_;
	/* Writes (frames stay whole), requests (one at a time), receive callback. */
	std::mutex wm_, cm_, rxm_;
	std::condition_variable cv_;
	bool gotReply_ = false, gotHello_ = false;
	/* Blocks through the FPGA: asked for, and what the device's hello said. */
	bool wantFast_ = false, fast_ = false;
	uint8_t wantReq_ = 0;
	int16_t replyStatus_ = 0;
	std::vector<uint8_t> reply_;
	RxFn fn_;
	uint16_t txSeq_ = 0;
	uint32_t baud_ = LINK_BOOT_BAUD;
	uint32_t unacked_ = 0;
	uint64_t badFrames_ = 0;
	uint8_t block_[ESDR_BLOCK_BYTES];
};

/*
 * The VRT link: the ESP32-S31 apps/stream_eth board's VITA 49.2 stream over UDP (vrt=<address>).
 *
 * The board takes no USB requests, so the link answers the module's requests itself:
 *  - GET_INFO, GET_RATES and GET_STATE are made up. The board cannot be asked for its ranges, so
 *    these are the fixed values of the app (radio.c, README): 2200 to 2900 MHz, gain index 0 to 71
 *    (or automatic), the diag16 rates 40 down to 0.64 MS/s, no transmit. Only the current frequency,
 *    gain and rate are asked of the board (a VITA query-state command, which needs no subscription).
 *  - SET_FREQ, SET_SAMPLE_RATE and SET_RX_GAIN set the want. SET_MODE RX subscribes (VRTS, VRTC +
 *    cookie, VRTS + cookie, VRTK) and sends the want in one command packet (execute and query
 *    state: the board answers with the execution acknowledge, then the query-state acknowledge with
 *    its settings epoch). The epoch goes up by one at every accepted change that restarts the ring
 *    and is in the context of every run: samples count only while the last context carries the
 *    epoch of the last acknowledged command (equality only, no tolerance, no timing). While
 *    streaming a worker thread sends changes (at most 20 commands a second, the latest want
 *    winning) and again what was not acknowledged; a call waits at most 0.5 s. SET_MODE OFF sends
 *    VRTX + cookie. Anything else (bandwidth, correction, digital gain, temperature, calibration)
 *    is refused or absent.
 *  - One thread owns the socket from the start: recvmmsg of up to 64 datagrams, acknowledges to
 *    whoever waits, context packets (rate), signal data repacked into ESDR_BLOCK_BYTES receive
 *    blocks (int8 I,Q is the module's own format) with the sample index as the block index, from
 *    the packet time (seconds and picoseconds against the rate), so a lost packet is a jump in the
 *    index and the module zero fills or reports it exactly as for the USB link. A restart on the
 *    board (new run, time going back, rate change) jumps the index by more than the module fills
 *    (1M samples) so the reader sees SOAPY_SDR_END_BURST.
 *  - The subscription is renewed four times a second (VRTS + cookie: the board forgets a host after
 *    5 s without one and, flooded, loses most of what it is sent; it takes 60 a second from the
 *    subscriber). The settings are not repeated at that pace. Data and context packets count as
 *    life; 5 s without one is the link gone: a warning, readStream goes on timing out, the
 *    renewals go on, and when the board answers again the settings are sent again.
 *
 * The socket is connect()ed to the board, so only its datagrams arrive, from its port 4992: no
 * inbound firewall rule is needed. The receive buffer is the largest the system grants without
 * root (net.core.rmem_max); SO_RXQ_OVFL counts what the kernel dropped.
 */
class VrtLink : public Link {
public:
	/* "host", "host:port", "[v6]:port", "fe80::1%eth0" (a bare IPv6 address has no port). */
	VrtLink(const std::string &spec, const std::string &portArg, bool takeover = false)
		: takeover_(takeover)
	{
		std::string host = spec, port = portArg.empty() ? "4992" : portArg;

		if (!spec.empty() && spec[0] == '[') {
			size_t e = spec.find(']');

			if (e == std::string::npos) {
				throw std::runtime_error("esdr: vrt=" + spec + ": missing ]");
			}
			host = spec.substr(1, e - 1);
			if (e + 1 < spec.size() && spec[e + 1] == ':') {
				port = spec.substr(e + 2);
			}
		} else if (std::count(spec.begin(), spec.end(), ':') == 1) {
			size_t c = spec.find(':');

			host = spec.substr(0, c);
			port = spec.substr(c + 1);
		}
		label_ = host + ":" + port;
		addrinfo hints{}, *ai = nullptr;

		hints.ai_socktype = SOCK_DGRAM;
		int r = getaddrinfo(host.c_str(), port.c_str(), &hints, &ai);

		if (r != 0) {
			throw std::runtime_error("esdr: vrt=" + spec + ": " + gai_strerror(r));
		}
		/* IPv6 first: link-local, then the other IPv6 addresses, then IPv4 (as vrt_udp). */
		struct Cand {
			sockaddr_storage ss;
			socklen_t len;
			bool v6;
		};
		std::vector<Cand> cands;

		for (int pass = 0; pass < 3; pass++) {
			for (addrinfo *a = ai; a != nullptr; a = a->ai_next) {
				bool v6 = a->ai_family == AF_INET6;
				bool ll = false;

				if (v6) {
					const auto *sa = reinterpret_cast<const sockaddr_in6 *>(a->ai_addr);

					ll = sa->sin6_addr.s6_addr[0] == 0xfe && (sa->sin6_addr.s6_addr[1] & 0xc0) == 0x80;
				}
				if ((pass == 0 && !ll) || (pass == 1 && (!v6 || ll)) || (pass == 2 && v6)) {
					continue;
				}
				Cand c{};

				std::memcpy(&c.ss, a->ai_addr, a->ai_addrlen);
				c.len = a->ai_addrlen;
				c.v6 = v6;
				cands.push_back(c);
			}
		}
		freeaddrinfo(ai);
		/* Each address in turn until one answers (a name may have an AAAA the board does not use). */
		std::string lastFail;
		vrtwire::Ack a, qa;
		bool up = false;

		for (const Cand &c : cands) {
			int fd = socket(c.ss.ss_family, SOCK_DGRAM, 0);

			if (fd < 0 || connect(fd, reinterpret_cast<const sockaddr *>(&c.ss), c.len) != 0) {
				lastFail = strerror(errno);
				if (fd >= 0) {
					::close(fd);
				}
				continue;
			}
			fd_ = fd;
			stop_ = false;
			setupSocket();
			thread_ = std::thread(&VrtLink::rxLoop, this);
			/* What the board runs with now: this also tells that it is there. */
			int got = -2;

			for (int k = 0; k < 3 && (got == -2 || got == -3); k++) {
				got = command(vrtwire::Settings{}, a, qa, 500, false);
			}
			if (got == 0) {
				SoapySDR_logf(SOAPY_SDR_INFO, "esdr: vrt using IPv%d", c.v6 ? 6 : 4);
				up = true;
				break;
			}
			lastFail = "no answer";
			shutdown();
		}
		if (!up) {
			throw std::runtime_error("esdr: no answer from " + label_ + " (" +
						 std::to_string(cands.size()) + " address(es) tried, IPv6 first; " +
						 lastFail + "; the stream_eth app listens on UDP port 4992)");
		}
		/*
		 * The queried centre includes the crystal correction's residual (under 500 Hz of the
		 * kHz plan): to a whole kHz, so that it does not drift at each open.
		 */
		if (qa.state.hasFreq && qa.state.freqHz >= (double)FREQ_MIN && qa.state.freqHz <= (double)FREQ_MAX) {
			wantFreq_ = (uint64_t)std::llround(qa.state.freqHz / 1000.0) * 1000ULL;
		}
		if (qa.state.hasRate && qa.state.rateHz >= 1.0 && qa.state.rateHz <= 1e9 &&
		    rateOk((uint32_t)std::lround(qa.state.rateHz))) {
			wantRate_ = (uint32_t)std::lround(qa.state.rateHz);
		}
		if (qa.state.hasGain && qa.state.gain >= -1 && qa.state.gain <= (int)GAIN_MAX) {
			wantGain_ = qa.state.gain;
		}
		if (wantFreq_ < FREQ_MIN || wantFreq_ > FREQ_MAX) {
			wantFreq_ = 2412000000ULL;
		}
		goodFreq_ = wantFreq_;
		goodRate_ = wantRate_;
		goodGain_ = wantGain_;
		goodEpoch_ = qa.state.epoch;
		goodValid_ = qa.state.hasEpoch;
		if (!qa.state.hasEpoch) {
			SoapySDR_logf(SOAPY_SDR_WARNING,
				      "esdr: %s reports no settings epoch: its firmware is too old for this module",
				      label_.c_str());
		}
		worker_ = std::thread(&VrtLink::workerLoop, this);
	}

	~VrtLink() override
	{
		stopStreaming();
		{
			std::lock_guard<std::mutex> g(cfgm_);

			stopW_ = true;
			wcv_.notify_all();
		}
		if (worker_.joinable()) {
			worker_.join();
		}
		shutdown();
	}

	int ctrlIn(uint8_t req, void *data, uint16_t len) override
	{
		uint8_t tmp[sizeof(esdr_info) + 64] = {0};
		size_t n = 0;

		if (req == ESDR_REQ_GET_INFO) {
			esdr_info i{};

			i.magic = ESDR_INFO_MAGIC;
			i.proto_version = ESDR_PROTO_VERSION;
			i.info_size = sizeof(i);
			snprintf(i.firmware, sizeof(i.firmware), "stream_eth vrt %s", label_.c_str());
			i.freq_min_hz = FREQ_MIN;
			i.freq_max_hz = FREQ_MAX;
			i.rx_rate_max_hz = RATES[0];
			i.rx_rate_count = 1;
			i.rx_gain_max = GAIN_MAX;
			i.tx_gain_max = 0;
			i.block_bytes = ESDR_BLOCK_BYTES;
			i.full_scale16 = 128;
			i.caps = ESDR_CAP_RATE_LIST;
			std::memcpy(tmp, &i, sizeof(i));
			n = sizeof(i);
		} else if (req == ESDR_REQ_GET_RATES) {
			std::memcpy(tmp, RATES, sizeof(RATES));
			n = sizeof(RATES);
		} else if (req == ESDR_REQ_GET_STATE) {
			esdr_state st{};
			std::lock_guard<std::mutex> g(cfgm_);

			st.mode = streaming_ ? ESDR_MODE_RX : ESDR_MODE_OFF;
			st.bits = 8;
			st.rx_gain = wantGain_ < 0 ? ESDR_GAIN_AUTO : (uint16_t)wantGain_;
			st.freq_hz = st.center_hz = wantFreq_;
			st.sample_rate_hz = wantRate_;
			st.bandwidth_hz = wantRate_;
			std::memcpy(tmp, &st, sizeof(st));
			n = sizeof(st);
		} else {
			return -ENOTSUP;
		}
		n = std::min<size_t>(n, len);
		std::memcpy(data, tmp, n);
		return (int)n;
	}

	int ctrlOut(uint8_t req, uint16_t value, const void *data, uint16_t len) override
	{
		vrtwire::Settings s;

		switch (req) {
		case ESDR_REQ_SET_MODE:
			if ((value & ~ESDR_MODE_F_DRAIN) == ESDR_MODE_OFF) {
				stopStreaming();
				return 0;
			}
			return (value == ESDR_MODE_RX) ? startStreaming() : -ENOTSUP;
		case ESDR_REQ_SET_FREQ: {
			uint64_t hz = 0;

			if (len < 8 || data == nullptr) {
				return -EINVAL;
			}
			std::memcpy(&hz, data, 8); /* little endian host */
			if (hz < FREQ_MIN || hz > FREQ_MAX) {
				SoapySDR_logf(SOAPY_SDR_WARNING,
					      "esdr: %llu Hz is outside the board's 2200 to 2900 MHz",
					      (unsigned long long)hz);
				return -EINVAL;
			}
			s.hasFreq = true;
			s.freqHz = (double)hz;
			return applySetting(s) < 0 ? -EIO : len;
		}
		case ESDR_REQ_SET_SAMPLE_RATE: {
			uint32_t hz = 0;

			if (len < 4 || data == nullptr) {
				return -EINVAL;
			}
			std::memcpy(&hz, data, 4);
			if (!rateOk(hz)) {
				return -EINVAL;
			}
			s.hasRate = true;
			s.rateHz = (double)hz;
			return applySetting(s) < 0 ? -EIO : len;
		}
		case ESDR_REQ_SET_RX_GAIN: {
			int g = value == ESDR_GAIN_AUTO ? -1 : (int)value;

			if (g > (int)GAIN_MAX) {
				return -EINVAL;
			}
			s.hasGain = true;
			s.gain = g;
			return applySetting(s) < 0 ? -EIO : 0;
		}
		case ESDR_REQ_SET_FORMAT:
			/* The board's samples are 8 bit I,Q, nothing else. */
			return value == 8 ? 0 : -EINVAL;
		default:
			/* Bandwidth (the board picks it), correction, digital gain, calibration, ... */
			return -ENOTSUP;
		}
	}

	int writeSamples(const uint8_t *, size_t, long) override { return -1; }

	void startRx(RxFn fn) override
	{
		std::lock_guard<std::mutex> g(rxm_);

		fn_ = std::move(fn);
		resetRun();
	}

	void stopRx() override
	{
		std::lock_guard<std::mutex> g(rxm_);

		fn_ = nullptr;
		blkN_ = 0;
	}

	std::string serial() const override { return "vrt:" + label_; }

	std::vector<std::string> sensorKeys() const override
	{
		return {"vrt_link_up",         "vrt_packets",        "vrt_lost_packets",
			"vrt_lost_samples",    "vrt_board_dropped_samples", "vrt_kernel_drops",
			"vrt_late_packets",    "vrt_restarts",       "vrt_rf_hz", "vrt_bad_packets"};
	}

	bool readSensor(const std::string &key, std::string &out) const override
	{
		uint64_t v;

		if (key == "vrt_link_up") {
			v = (streaming_ && !gone_ && !busy_) ? 1 : 0;
		} else if (key == "vrt_packets") {
			v = packets_;
		} else if (key == "vrt_lost_packets") {
			v = pktsLost_;
		} else if (key == "vrt_lost_samples") {
			v = netLossSamples_;
		} else if (key == "vrt_board_dropped_samples") {
			v = boardLossSamples_;
		} else if (key == "vrt_kernel_drops") {
			v = kernelDrops_;
		} else if (key == "vrt_late_packets") {
			v = late_;
		} else if (key == "vrt_restarts") {
			v = restarts_;
		} else if (key == "vrt_rf_hz") {
			v = ctxFreq_;
		} else if (key == "vrt_bad_packets") {
			v = bad_;
		} else {
			return false;
		}
		out = std::to_string(v);
		return true;
	}

	std::string statusLine() const override
	{
		char b[256];

		snprintf(b, sizeof(b),
			 "vrt: %llu packets, lost %llu packets / %llu samples, board dropped %llu "
			 "samples, kernel dropped %llu, late %llu, restarts %llu, bad %llu, link lost %llu times",
			 (unsigned long long)packets_, (unsigned long long)pktsLost_,
			 (unsigned long long)netLossSamples_, (unsigned long long)boardLossSamples_,
			 (unsigned long long)kernelDrops_, (unsigned long long)late_,
			 (unsigned long long)restarts_, (unsigned long long)bad_,
			 (unsigned long long)linkLost_);
		return b;
	}

private:
	/* The app's fixed values (apps/stream_eth/src/radio.c and README). */
	static constexpr uint64_t FREQ_MIN = 2200000000ULL, FREQ_MAX = 2900000000ULL;
	static constexpr unsigned GAIN_MAX = 71;
	static constexpr uint32_t RATES[15] = {40000000, 20000000, 16000000, 10000000, 8000000,
					       5000000,  4000000,  3200000,  2500000,  2000000,
					       1600000,  1250000,  1000000,  800000,    640000};
	static constexpr size_t MAX_PAIRS = ESDR_BLOCK_PAYLOAD / 2U;
	/* More than the module zero fills (1M samples): it reports the jump as a loss. */
	static constexpr uint64_t DISCONT = 1U << 20;
	static constexpr double SILENT_S = 5.0;

	using Clock = std::chrono::steady_clock;

	static int64_t nowNs()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			       Clock::now().time_since_epoch()).count();
	}

	static bool rateOk(uint32_t hz)
	{
		return std::find(std::begin(RATES), std::end(RATES), hz) != std::end(RATES);
	}

	void setupSocket()
	{
		/* As much as the system grants: 256 MiB down; Linux clamps to rmem_max (doubled). */
		for (int mb = 256; mb >= 1; mb /= 2) {
			int sz = mb << 20;

			if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz)) == 0 ||
			    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0) {
				break;
			}
		}
		socklen_t ol = sizeof(rcvbuf_);

		getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf_, &ol);
		int one = 1;

		setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
		timeval tv{0, 100000};

		setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		lastKernel_ = 0; /* the socket's drop count starts at 0 */
		SoapySDR_logf(SOAPY_SDR_INFO, "esdr: vrt %s, receive buffer %d KiB%s", label_.c_str(),
			      rcvbuf_ / 1024,
			      rcvbuf_ < (8 << 20) ? " (small: raise net.core.rmem_max for 20 MS/s and up)" : "");
	}

	void shutdown()
	{
		stop_ = true;
		if (thread_.joinable()) {
			thread_.join();
		}
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

	/* False when the kernel refused the datagram (no route, permission, size, ...). */
	bool sendRaw(const void *p, size_t n)
	{
		if (::send(fd_, p, n, 0) >= 0) {
			return true;
		}
		sendErr_ = errno;
		return false;
	}

	/*
	 * One command packet. With settings (needExec): the execution acknowledge and, unless it
	 * refuses, the query-state acknowledge with the epoch after the command; without (a query
	 * only): the query-state acknowledge. 0, -1 refused, -2 no (complete) answer, -3 the kernel
	 * refused to send it.
	 */
	int command(const vrtwire::Settings &s, vrtwire::Ack &execAck, vrtwire::Ack &qAck, int waitMs,
		    bool needExec)
	{
		std::lock_guard<std::mutex> cg(cmdm_);
		uint8_t pkt[vrtwire::CMD_MAX_BYTES];
		uint32_t id = ++msgId_;
		size_t n = vrtwire::buildCommand(pkt, sizeof(pkt), id, s);
		std::unique_lock<std::mutex> lk(ackm_);

		pendId_ = id;
		gotExec_ = gotQ_ = false;
		lastCmdNs_ = nowNs();
		lk.unlock();
		if (!sendRaw(pkt, n)) {
			lk.lock();
			pendId_ = 0;
			return -3;
		}
		lk.lock();
		auto refused = [&] { return gotExec_ && (ackE_.errors || !ackE_.executed); };
		bool done = ackcv_.wait_for(lk, std::chrono::milliseconds(waitMs), [&] {
			return needExec ? (gotExec_ && (refused() || gotQ_)) : gotQ_;
		});

		pendId_ = 0;
		if (!done) {
			return -2;
		}
		if (gotExec_) {
			execAck = ackE_;
		}
		if (gotQ_) {
			qAck = ackQ_;
		}
		/* The board marks a refusal by leaving out "executed" (ctl.c: errors only when asked for). */
		return refused() ? -1 : 0;
	}

	/*
	 * The settings, as the board's settings epoch lets them be known (no tolerance, no timing guess):
	 *
	 *  - want (wantFreq_ ...): what the application asked for last.
	 *  - good (goodFreq_ ..., goodEpoch_): what the board acknowledged for the last command, exactly
	 *    as sent, and the epoch its query-state acknowledge carried: the epoch of the run those
	 *    settings make. The board's epoch goes up by one at every accepted change that restarts the
	 *    ring, so equal epochs mean equal settings. goodValid_ is false when that is not known (a
	 *    reboot, the link gone, a command not acknowledged).
	 *  - in force (inForce_): good is valid and the last context carries good's epoch (compared mod
	 *    256, only for equality): its samples are those of the settings in good. Only then do
	 *    samples count.
	 *
	 * One worker thread sends the commands, never faster than 20 a second, the latest want winning
	 * (a drag does not queue), and again, until acknowledged, what was not. A call of the module sets
	 * the want and waits at most 0.5 s for its outcome; while the link is gone or the stream taken it
	 * only keeps the want, and the worker sends it when the link is back.
	 */
	struct Want {
		uint64_t freq;
		uint32_t rate;
		int gain;
		bool operator==(const Want &o) const { return freq == o.freq && rate == o.rate && gain == o.gain; }
		bool operator!=(const Want &o) const { return !(*this == o); }
	};

	/* All under cfgm_. */
	Want getWantL() const { return {wantFreq_, wantRate_, wantGain_}; }
	Want getGoodL() const { return {goodFreq_, goodRate_, goodGain_}; }

	Want getWant()
	{
		std::lock_guard<std::mutex> g(cfgm_);

		return getWantL();
	}

	/* The wanted fields of s (the module's request); the generation of the want. */
	uint64_t putWant(const vrtwire::Settings &s)
	{
		std::lock_guard<std::mutex> g(cfgm_);

		if (s.hasFreq) {
			wantFreq_ = (uint64_t)s.freqHz;
		}
		if (s.hasRate) {
			wantRate_ = (uint32_t)s.rateHz;
		}
		if (s.hasGain) {
			wantGain_ = s.gain;
		}
		return ++wantGen_;
	}

	void kickWorker()
	{
		std::lock_guard<std::mutex> g(cfgm_);

		wcv_.notify_all();
	}

	/* The settings of the board are not known (a reboot, a gap in the link): send them again. */
	void markGoodUnknown()
	{
		{
			std::lock_guard<std::mutex> g(cfgm_);

			goodValid_ = false;
		}
		kickWorker();
	}

	/* inForce_ from good and the last context (rxm_ held). */
	void recomputeInForceR()
	{
		std::lock_guard<std::mutex> g(cfgm_);

		inForce_ = goodValid_ && ctxHaveEpoch_ && ctxEpoch_ == goodEpoch_;
	}

	static std::string wefText(uint32_t code)
	{
		char b[48];

		snprintf(b, sizeof(b), "%s (0x%08x)",
			 code == 0x10000000U ? "out of range" : code == 0x80000000U ? "not executed"
			 : code == 0x40000000U ? "device failure" : code == 0x20000000U ? "not accepted"
			 : code == 0x04000000U ? "value invalid" : "error word", code);
		return b;
	}

	/* Say why the board refused: the fields it names (the request asked for them), what was sent. */
	void logRefusal(const vrtwire::Settings &s, const vrtwire::Ack &a)
	{
		std::string why;

		if (a.errFreq) {
			why += " frequency: " + wefText(a.codeFreq) + ";";
		}
		if (a.errGain) {
			why += " gain: " + wefText(a.codeGain) + ";";
		}
		if (a.errRate) {
			why += " sample rate: " + wefText(a.codeRate) + ";";
		}
		if (a.errFormat) {
			why += " payload format: " + wefText(a.codeFormat) + ";";
		}
		if (a.errOther) {
			why += " another field;";
		}
		if (why.empty()) {
			why = a.errors ? " errors flagged, no field named;" : " not executed, no error field;";
		}
		std::string sent;
		char b[64];

		if (s.hasFreq) {
			snprintf(b, sizeof(b), " %.0f Hz", s.freqHz);
			sent += b;
		}
		if (s.hasGain) {
			snprintf(b, sizeof(b), " gain %d", s.gain);
			sent += b;
		}
		if (s.hasRate) {
			snprintf(b, sizeof(b), " %.0f S/s", s.rateHz);
			sent += b;
		}
		SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: the board refused the settings (sent:%s):%s",
			      sent.c_str(), why.c_str());
	}

	vrtwire::Settings settingsOf(const Want &w) const
	{
		vrtwire::Settings s;

		s.hasFreq = s.hasGain = s.hasRate = true;
		s.freqHz = (double)w.freq;
		s.gain = w.gain;
		s.rateHz = (double)w.rate;
		return s;
	}

	vrtwire::Settings allSettings()
	{
		return settingsOf(getWant());
	}

	/*
	 * Send w and wait for both acknowledges: 0 (taken: good and its epoch are updated), -1 refused
	 * (logged), -2 no (complete) answer, -3 the kernel refused to send, -4 the board reports no
	 * epoch.
	 */
	int applyWant(const Want &w, int waitMs)
	{
		vrtwire::Ack e, q;
		vrtwire::Settings s = settingsOf(w);
		int r;

		cmdInFlight_ = true;
		r = command(s, e, q, waitMs, true);
		if (r == 0 && !q.state.hasEpoch) {
			r = -4;
		}
		if (r == 0) {
			{
				std::lock_guard<std::mutex> g(cfgm_);

				goodFreq_ = w.freq;
				goodRate_ = w.rate;
				goodGain_ = w.gain;
				goodEpoch_ = q.state.epoch;
				goodValid_ = true;
			}
			std::lock_guard<std::mutex> g(rxm_);

			seenGood_ = false;
			mismatchSinceNs_ = 0;
			recomputeInForceR();
		} else if (r == -1) {
			logRefusal(s, e);
		} else if (r == -2 || r == -3) {
			/*
			 * Not known whether the board took it (the acknowledge may be what was lost): what it
			 * runs is unknown until a command is acknowledged, and what is wanted is sent again.
			 */
			{
				std::lock_guard<std::mutex> g(cfgm_);

				goodValid_ = false;
			}
			std::lock_guard<std::mutex> g(rxm_);

			recomputeInForceR();
		}
		cmdInFlight_ = false;
		return r;
	}

	/*
	 * A refusal (or a command that could not be sent): the want goes back to good, unless the
	 * application has set something since (gen is the want that was sent): its set wins, nothing
	 * is undone under it. With good unknown the board is asked what it runs.
	 */
	void afterRefusal(uint64_t gen)
	{
		bool valid;
		{
			std::lock_guard<std::mutex> g(cfgm_);

			valid = goodValid_;
		}
		if (!valid) {
			vrtwire::Ack e, q;

			cmdInFlight_ = true;
			int r = command(vrtwire::Settings{}, e, q, 500, false);

			cmdInFlight_ = false;
			if (r == 0 && q.state.hasEpoch && q.state.hasFreq && q.state.hasRate && q.state.hasGain &&
			    q.state.freqHz >= (double)FREQ_MIN && q.state.freqHz <= (double)FREQ_MAX &&
			    q.state.rateHz >= 1.0 && q.state.rateHz <= 1e9 && q.state.gain >= -1 &&
			    q.state.gain <= (int)GAIN_MAX) {
				std::lock_guard<std::mutex> g(cfgm_);

				/* the board's centre to a whole kHz (the correction's residual is below 500 Hz) */
				goodFreq_ = (uint64_t)std::llround(q.state.freqHz / 1000.0) * 1000ULL;
				goodRate_ = (uint32_t)std::lround(q.state.rateHz);
				goodGain_ = q.state.gain;
				goodEpoch_ = q.state.epoch;
				goodValid_ = true;
			}
		}
		{
			std::lock_guard<std::mutex> g(cfgm_);

			if (goodValid_ && wantGen_ == gen) {
				wantFreq_ = goodFreq_;
				wantRate_ = goodRate_;
				wantGain_ = goodGain_;
			}
		}
		std::lock_guard<std::mutex> g(rxm_);

		seenGood_ = false;
		recomputeInForceR();
	}

	/*
	 * A change of the application: the want first, then the worker sends it. Waits at most 0.5 s
	 * for the outcome (an answer by the board); while the link is gone, taken by another host or the
	 * stream not running it only keeps the want. Refused: -EIO (the want is back at good).
	 */
	int applySetting(const vrtwire::Settings &s)
	{
		uint64_t gen = putWant(s);

		if (!streaming_ || gone_ || busy_) {
			return 0;
		}
		std::unique_lock<std::mutex> lk(cfgm_);

		wcv_.notify_all();
		wcv_.wait_for(lk, std::chrono::milliseconds(500),
			      [&] { return doneGen_ >= gen || refusedGen_ == gen || wantGen_ > gen || !streaming_; });
		return refusedGen_ == gen ? -EIO : 0;
	}

	bool workPendingL() const
	{
		return wantGen_ != doneGen_ || !goodValid_ || getWantL() != getGoodL();
	}

	/* The only sender of settings while the stream runs. */
	void workerLoop()
	{
		int64_t lastStart = 0;
		int fails = 0;
		std::unique_lock<std::mutex> lk(cfgm_);

		while (!stopW_) {
			wcv_.wait_for(lk, std::chrono::milliseconds(200),
				      [&] { return stopW_ || (workPendingL() && streaming_ && !busy_); });
			if (stopW_) {
				break;
			}
			if (!(workPendingL() && streaming_ && !busy_)) {
				continue;
			}
			/* At most 20 commands a second; what is set meanwhile is what goes. */
			int64_t wait = lastStart + 50000000LL - nowNs();

			if (wait > 0) {
				wcv_.wait_for(lk, std::chrono::nanoseconds(wait), [&] { return stopW_; });
				continue;
			}
			Want w = getWantL();
			uint64_t gen = wantGen_;

			if (goodValid_ && w == getGoodL()) {
				doneGen_ = gen;
				wcv_.notify_all();
				continue;
			}
			lastStart = nowNs();
			lk.unlock();
			int r = applyWant(w, 500);

			lk.lock();
			if (r == 0) {
				fails = 0;
				doneGen_ = gen;
			} else if (r == -2 || r == -3) {
				/*
				 * No (complete) answer, or not sent: the wanted value stays and is sent again
				 * (the command is idempotent), 3 tries a second at first, then every 2 s. Also
				 * while the link is gone: the board may only be waiting for the next request.
				 */
				if (r == -3) {
					SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: cannot send to %s: %s",
						      label_.c_str(), strerror(sendErr_));
				}
				if (++fails == 8) {
					SoapySDR_logf(SOAPY_SDR_WARNING,
						      "esdr: no acknowledge from %s: still trying", label_.c_str());
				}
				if (fails >= 8) {
					wcv_.wait_for(lk, std::chrono::milliseconds(1500), [&] { return stopW_; });
				}
			} else {
				fails = 0;
				if (r == -4) {
					SoapySDR_logf(SOAPY_SDR_WARNING,
						      "esdr: the board reports no settings epoch (firmware too old?)");
				}
				lk.unlock();
				afterRefusal(gen);
				lk.lock();
				doneGen_ = gen;
				if (wantGen_ == gen) {
					refusedGen_ = gen;
				}
			}
			wcv_.notify_all();
		}
	}

	/*
	 * "VRTS" + cookie (or "VRTT" to take over), "VRTS" alone while there is no cookie. Under
	 * subm_ with the stop: a renewal never follows the VRTX (the stop wins).
	 */
	bool sendSub(bool takeover = false)
	{
		std::lock_guard<std::mutex> g(subm_);
		uint8_t b[12];

		if (!subscribing_) {
			return true;
		}
		memcpy(b, takeover && haveCookie_ ? "VRTT" : "VRTS", 4);
		if (!haveCookie_) {
			return sendRaw(b, 4);
		}
		uint64_t ck = cookie_;

		memcpy(b + 4, &ck, 8);
		return sendRaw(b, 12);
	}

	/* No more renewals; "VRTX" + cookie if the subscription was ours. */
	void dropSub(bool sendStop)
	{
		std::lock_guard<std::mutex> g(subm_);
		bool was = subscribing_.exchange(false);

		if (was && sendStop && haveCookie_) {
			uint8_t b[12];
			uint64_t ck = cookie_;

			memcpy(b, "VRTX", 4);
			memcpy(b + 4, &ck, 8);
			sendRaw(b, 12);
		}
	}

	int startStreaming()
	{
		/* Subscribe: VRTS (-> VRTC), VRTS + cookie (-> VRTK, or VRTB when another host has it). */
		{
			std::lock_guard<std::mutex> g(rxm_);

			rate_ = 0;
			inForce_ = false;
			ctxHaveEpoch_ = false;
			seenGood_ = false;
		}
		subscribed_ = false;
		busy_ = false;
		subscribing_ = true;
		for (int k = 0; k < 60 && !subscribed_ && !busy_; k++) {
			if (k % 6 == 0 && !sendSub(takeover_ && haveCookie_)) {
				dropSub(false);
				SoapySDR_logf(SOAPY_SDR_ERROR, "esdr: cannot send to %s: %s", label_.c_str(),
					      strerror(sendErr_));
				return -EIO;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		if (busy_) {
			dropSub(false);
			SoapySDR_logf(SOAPY_SDR_ERROR,
				      "esdr: %s is busy: another host has the stream (VRTB); add vrt_takeover=1 "
				      "to the device arguments to take it over", label_.c_str());
			return -EBUSY;
		}
		if (!subscribed_) {
			dropSub(false);
			SoapySDR_logf(SOAPY_SDR_ERROR, "esdr: %s does not accept the subscription",
				      label_.c_str());
			return -EIO;
		}
		/*
		 * The settings (up to 4 tries of an acknowledge), then the context of the epoch that was
		 * acknowledged; the wait for it starts anew after each acknowledge, and the settings are
		 * sent again if the application set others meanwhile.
		 */
		int64_t t0 = nowNs();
		bool sentOnce = false;

		for (;;) {
			bool valid;
			Want g, w;
			{
				std::lock_guard<std::mutex> lk(cfgm_);

				valid = goodValid_;
				g = getGoodL();
				w = getWantL();
			}
			if (!sentOnce || !valid || w != g) {
				int r = -2;
				uint64_t gen;
				{
					std::lock_guard<std::mutex> lk(cfgm_);

					gen = wantGen_;
				}
				for (int k = 0; k < 4 && r == -2; k++) {
					r = applyWant(w, 1000);
				}
				if (r != 0) {
					if (r == -1 || r == -3 || r == -4) {
						afterRefusal(gen);
					}
					dropSub(true);
					if (r == -3) {
						SoapySDR_logf(SOAPY_SDR_ERROR, "esdr: cannot send to %s: %s",
							      label_.c_str(), strerror(sendErr_));
					} else if (r == -4) {
						SoapySDR_logf(SOAPY_SDR_ERROR,
							      "esdr: the board reports no settings epoch: its firmware is too old");
					} else {
						SoapySDR_logf(SOAPY_SDR_ERROR,
							      r == -1 ? "esdr: the board refused the settings (see above)"
								      : "esdr: no acknowledge of the settings from %s",
							      label_.c_str());
					}
					return -EIO;
				}
				sentOnce = true;
				t0 = nowNs();
			}
			{
				std::lock_guard<std::mutex> lk(rxm_);

				if (inForce_) {
					/* streaming_ and the last look at the want under one lock: a set made
					 * during the activation is the worker's from here on */
					std::lock_guard<std::mutex> lc(cfgm_);

					resetRun();
					lastRx_ = Clock::now();
					gone_ = false;
					streaming_ = true;
					wcv_.notify_all();
					return 0;
				}
			}
			if (nowNs() - t0 > 3000000000LL) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		dropSub(true);
		SoapySDR_logf(SOAPY_SDR_ERROR,
			      "esdr: the board's context does not show the settings epoch acknowledged");
		return -EIO;
	}

	void stopStreaming()
	{
		streaming_ = false;
		dropSub(true);
		kickWorker();
	}

	/* The statistics and the sample axis of a new run (rxm_ held). */
	void resetRun()
	{
		haveBase_ = rebase_ = haveCount_ = pendingGap_ = false;
		blkN_ = 0;
		packets_ = pktsLost_ = netLossSamples_ = boardLossSamples_ = late_ = restarts_ = 0;
		bad_ = 0;
		kernelDrops_ = 0;
		/* lastKernel_ goes on across runs: the socket's count starts at 0 when it opens */
	}

	void flushBlock()
	{
		if (blkN_ == 0) {
			return;
		}
		esdr_block_hdr h{};

		h.magic = ESDR_BLOCK_MAGIC;
		h.flags = blkGap_ ? ESDR_BLK_GAP : 0;
		h.samples = (uint16_t)blkN_;
		h.index = blkOut_;
		std::memcpy(blk_, &h, sizeof(h));
		blkN_ = 0;
		if (fn_) {
			fn_(blk_, ESDR_BLOCK_BYTES);
		}
	}

	void onData(const uint8_t *b, size_t len)
	{
		vrtwire::Data d;

		if (!streaming_ || !fn_ || rate_ == 0 || !inForce_) {
			return;
		}
		if (!vrtwire::parseData(b, len, d) || d.bodyBytes / 2 > MAX_PAIRS) {
			bad_++;
			return;
		}
		const uint64_t pairs = d.bodyBytes / 2;
		const uint64_t idx = vrtwire::sampleIndex(d, rate_);

		packets_++;
		/*
		 * The time is the authority: a jump in it is counted in packets (a lost run). The count
		 * (mod 16) is only a check for losses the time does not show.
		 */
		if (haveCount_ && haveBase_ && idx == nextIn_ && ((d.count - lastCount_) & 15U) != 1U) {
			unsigned miss = (d.count - lastCount_ - 1U) & 15U;

			pktsLost_ += miss != 0 ? miss : 16U;
		}
		lastCount_ = d.count;
		haveCount_ = true;
		if (!haveBase_) {
			base_ = (int64_t)idx;
			haveBase_ = true;
			rebase_ = false;
			nextIn_ = idx;
		} else if (rebase_) {
			flushBlock();
			base_ = (int64_t)idx - (int64_t)(nextOut_ + DISCONT);
			rebase_ = false;
			pendingGap_ = true;
			restarts_++;
			nextIn_ = idx;
		} else if (idx != nextIn_) {
			if (idx > nextIn_) {
				uint64_t gap = idx - nextIn_;

				flushBlock();
				pendingGap_ = true;
				(d.loss ? boardLossSamples_ : netLossSamples_) += gap;
				pktsLost_ += (gap + pairs / 2) / pairs;
			} else if (nextIn_ - idx <= rate_ / 10U) {
				/* Behind: a repeat or reordered packet, its samples are in already. */
				late_++;
				return;
			} else {
				/* Time went back by more than a reordering: the board began again. */
				flushBlock();
				base_ = (int64_t)idx - (int64_t)(nextOut_ + DISCONT);
				pendingGap_ = true;
				restarts_++;
				nextIn_ = idx;
			}
		}
		const uint8_t *src = d.body;
		uint64_t at = idx;

		for (uint64_t left = pairs; left > 0;) {
			if (blkN_ == 0) {
				blkOut_ = (uint64_t)((int64_t)at - base_);
				blkGap_ = pendingGap_;
				pendingGap_ = false;
			}
			size_t k = (size_t)std::min<uint64_t>(left, MAX_PAIRS - blkN_);

			std::memcpy(blk_ + sizeof(esdr_block_hdr) + 2 * blkN_, src, 2 * k);
			blkN_ += k;
			src += 2 * k;
			at += k;
			left -= k;
			if (blkN_ == MAX_PAIRS) {
				flushBlock();
			}
		}
		nextIn_ = idx + pairs;
		nextOut_ = (uint64_t)((int64_t)nextIn_ - base_);
	}

	void onContext(const uint8_t *b, size_t len)
	{
		vrtwire::Context c;

		if (!vrtwire::parseContext(b, len, c)) {
			bad_++;
			return;
		}
		if (c.state.hasFreq && c.state.freqHz >= 0.0 && c.state.freqHz < 1e11) {
			ctxFreq_ = (uint64_t)c.state.freqHz;
		}
		if (!c.state.hasRate || !(c.state.rateHz >= 1.0) || !(c.state.rateHz <= 1e9)) {
			/* not a context of this board's kind: ignored, what is in force stays */
			bad_++;
			return;
		}
		uint32_t nr = (uint32_t)std::lround(c.state.rateHz);

		if (subscribing_) {
			subscribed_ = true; /* it streams to us: as good as VRTK */
		}
		if (nr != rate_) {
			if (rate_ != 0) {
				SoapySDR_logf(SOAPY_SDR_INFO, "esdr: the board's rate is now %u S/s", nr);
			}
			if (rate_ != 0 && haveBase_) {
				flushBlock();
				rebase_ = true;
			}
			rate_ = nr;
		}
		ctxHaveEpoch_ = c.state.hasEpoch;
		ctxEpoch_ = c.state.epoch;
		bool gv;
		uint8_t ge;
		{
			std::lock_guard<std::mutex> g(cfgm_);

			gv = goodValid_;
			ge = goodEpoch_;
		}
		if (gv && ctxHaveEpoch_) {
			if (ctxEpoch_ == ge) {
				seenGood_ = true;
				mismatchSinceNs_ = 0;
			} else if (streaming_ && !cmdInFlight_ && mismatchSinceNs_ != 0 &&
				   nowNs() - mismatchSinceNs_ > 1500000000LL) {
				/* A safety net, whatever seenGood_ says: the settings are not those we know. */
				SoapySDR_logf(SOAPY_SDR_WARNING,
					      "esdr: the board's settings epoch is %u, not the acknowledged %u, for "
					      "over 1.5 s: sending the settings again", ctxEpoch_, ge);
				seenGood_ = false;
				mismatchSinceNs_ = 0;
				markGoodUnknown();
			} else if (mismatchSinceNs_ == 0 && !cmdInFlight_) {
				mismatchSinceNs_ = nowNs();
			}
			if (ctxEpoch_ != ge && seenGood_ && !cmdInFlight_ && streaming_) {
				/*
				 * The run we had has been replaced without a command of ours: the board
				 * restarted (its epoch is 0 again) or lost them. Its settings are not known
				 * to be ours: send them again.
				 */
				SoapySDR_logf(SOAPY_SDR_WARNING,
					      "esdr: the board's settings epoch moved from %u to %u without a "
					      "command of ours: sending the settings again", ge, ctxEpoch_);
				seenGood_ = false;
				markGoodUnknown();
			}
		}
		recomputeInForceR();
	}

	void onAck(const uint8_t *b, size_t len)
	{
		vrtwire::Ack a;

		if (!vrtwire::parseAck(b, len, a)) {
			bad_++;
			return;
		}
		std::lock_guard<std::mutex> g(ackm_);

		if (pendId_ == 0 || a.messageId != pendId_) {
			return;
		}
		if (a.query && !gotQ_) {
			ackQ_ = a;
			gotQ_ = true;
			ackcv_.notify_all();
		} else if (a.execution && !a.query && !gotExec_) {
			ackE_ = a;
			gotExec_ = true;
			ackcv_.notify_all();
		}
	}

	/* The board's subscription messages: "VRTC" + cookie, "VRTK", "VRTB" (data never starts 'V'). */
	void onProto(const uint8_t *b, size_t len)
	{
		if (b[3] == 'C' && len == 12) {
			uint64_t ck;

			std::memcpy(&ck, b + 4, 8);
			cookie_ = ck;
			haveCookie_ = true;
			/* The answer to a plain VRTS or a stale cookie: subscribe with the new one now. */
			if (subscribing_) {
				sendSub(takeover_ && !subscribed_);
			}
		} else if (b[3] == 'K' && subscribing_) {
			bool again = subscribed_;

			subscribed_ = true;
			busy_ = false;
			/* A new subscription while streaming: the board forgot us. Its settings are not ours. */
			if (streaming_ && again) {
				/* The board forgot us (a reboot): its settings are its defaults. */
				seenGood_ = false;
				markGoodUnknown();
				recomputeInForceR();
			}
		} else if (b[3] == 'B') {
			if (streaming_ && !busy_) {
				SoapySDR_logf(SOAPY_SDR_WARNING,
					      "esdr: another host has the board's stream (VRTB); not taking it back");
			}
			busy_ = true;
		}
	}

	void handle(const uint8_t *b, size_t len)
	{
		if (len >= 4 && std::memcmp(b, "VRT", 3) == 0) {
			onProto(b, len);
			return;
		}
		switch (vrtwire::classify(b, len)) {
		case vrtwire::Kind::Data:
			alive_ = true;
			onData(b, len);
			break;
		case vrtwire::Kind::Context:
			alive_ = true;
			onContext(b, len);
			break;
		case vrtwire::Kind::Ack:
			onAck(b, len);
			break;
		default:
			bad_++;
		}
	}

	void rxLoop()
	{
		constexpr int BATCH = 64;
		constexpr size_t PKT = 2048;
		std::vector<uint8_t> bufs((size_t)BATCH * PKT);
		std::vector<std::array<char, 64>> ctrl(BATCH);
		mmsghdr m[BATCH];
		iovec iov[BATCH];
		auto lastRenew = Clock::now();

		for (int k = 0; k < BATCH; k++) {
			iov[k].iov_base = &bufs[(size_t)k * PKT];
			iov[k].iov_len = PKT;
		}
		while (!stop_) {
			for (int k = 0; k < BATCH; k++) {
				std::memset(&m[k].msg_hdr, 0, sizeof(m[k].msg_hdr));
				m[k].msg_hdr.msg_iov = &iov[k];
				m[k].msg_hdr.msg_iovlen = 1;
				m[k].msg_hdr.msg_control = ctrl[k].data();
				m[k].msg_hdr.msg_controllen = ctrl[k].size();
			}
			int n = recvmmsg(fd_, m, BATCH, MSG_WAITFORONE, nullptr);
			auto now = Clock::now();

			if (n < 0) {
				n = 0;
				if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
				    errno != ECONNREFUSED) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}
			{
				std::lock_guard<std::mutex> g(rxm_);

				for (int k = 0; k < n; k++) {
					for (cmsghdr *c = CMSG_FIRSTHDR(&m[k].msg_hdr); c != nullptr;
					     c = CMSG_NXTHDR(&m[k].msg_hdr, c)) {
						if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
							uint32_t d;

							std::memcpy(&d, CMSG_DATA(c), 4);
							/* a count since the socket opened: its rise is what was lost */
							kernelDrops_ += (uint32_t)(d - lastKernel_);
							lastKernel_ = d;
						}
					}
					if ((m[k].msg_hdr.msg_flags & MSG_TRUNC) != 0) {
						bad_++;
						continue;
					}
					handle(&bufs[(size_t)k * PKT], m[k].msg_len);
				}
				if (n > 0) {
					/* A batch is what came together: no sample waits for a block to fill. */
					flushBlock();
				}
				/* Life is data or context: a VRTB or VRTC is not a stream. */
				if (alive_) {
					alive_ = false;
					lastRx_ = now;
					busy_ = false;
					if (gone_.exchange(false)) {
						SoapySDR_logf(SOAPY_SDR_INFO, "esdr: the board answers again");
						if (streaming_) {
							/* It may have rebooted meanwhile: its settings are not known. */
							rebase_ = haveBase_;
							seenGood_ = false;
							markGoodUnknown();
							recomputeInForceR();
						}
					}
				} else if (streaming_ && !gone_ &&
					   std::chrono::duration<double>(now - lastRx_).count() > SILENT_S) {
					gone_ = true;
					linkLost_++;
					SoapySDR_logf(SOAPY_SDR_WARNING,
						      "esdr: nothing from %s for %.0f s: link lost (renewals go on)",
						      label_.c_str(), SILENT_S);
				}
			}
			if (subscribing_ && now - lastRenew >= std::chrono::milliseconds(250)) {
				lastRenew = now;
				sendSub();
			}
		}
	}

	std::string label_;
	int fd_ = -1, rcvbuf_ = 0;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> streaming_{false}, gone_{false};

	/* The settings: want and good (see the comment at Want) under cfgm_; one command at a time (cmdm_). */
	std::mutex cfgm_, cmdm_, ackm_;
	std::condition_variable wcv_;
	uint64_t wantFreq_ = 2412000000ULL;
	uint32_t wantRate_ = 20000000;
	int wantGain_ = -1;
	uint64_t goodFreq_ = 2412000000ULL;
	uint32_t goodRate_ = 20000000;
	int goodGain_ = -1;
	uint8_t goodEpoch_ = 0;
	bool goodValid_ = false;
	uint64_t wantGen_ = 0, doneGen_ = 0, refusedGen_ = ~0ULL;
	bool stopW_ = false;
	std::thread worker_;
	std::atomic<bool> cmdInFlight_{false};
	const bool takeover_;
	std::atomic<bool> subscribing_{false}, subscribed_{false}, busy_{false}, haveCookie_{false};
	std::atomic<uint64_t> cookie_{0};
	std::mutex subm_;
	std::atomic<uint32_t> msgId_{0};
	std::atomic<int> sendErr_{0};
	std::condition_variable ackcv_;
	uint32_t pendId_ = 0;
	bool gotExec_ = false, gotQ_ = false;
	vrtwire::Ack ackE_, ackQ_;
	std::atomic<int64_t> lastCmdNs_{0};

	/* The receive thread's state, under rxm_ (which startRx, stopRx and SET_MODE take too). */
	std::mutex rxm_;
	RxFn fn_;
	Clock::time_point lastRx_{};
	uint32_t rate_ = 0;
	bool alive_ = false;
	/* good is valid and the last context carries its epoch: samples count (rxm_, set by recomputeInForceR()). */
	bool inForce_ = false;
	bool ctxHaveEpoch_ = false, seenGood_ = false;
	int64_t mismatchSinceNs_ = 0;
	uint8_t ctxEpoch_ = 0;
	bool haveBase_ = false, rebase_ = false, haveCount_ = false, pendingGap_ = false;
	unsigned lastCount_ = 0;
	int64_t base_ = 0;
	uint64_t nextIn_ = 0, nextOut_ = 0, blkOut_ = 0;
	bool blkGap_ = false;
	size_t blkN_ = 0;
	uint8_t blk_[ESDR_BLOCK_BYTES];
	uint32_t lastKernel_ = 0;

	/* Counters of the run (read by the sensors without a lock: they are only for show). */
	std::atomic<uint64_t> packets_{0}, pktsLost_{0}, netLossSamples_{0}, boardLossSamples_{0},
		kernelDrops_{0}, late_{0}, restarts_{0}, bad_{0}, linkLost_{0}, ctxFreq_{0};
};

class SoapyEsdr : public SoapySDR::Device {
public:
	explicit SoapyEsdr(const SoapySDR::Kwargs &args)
	{
		if (args.count("vrt")) {
			link_ = std::make_unique<VrtLink>(args.at("vrt"), args.count("vrt_port")
								  ? args.at("vrt_port") : "",
							  args.count("vrt_takeover") && args.at("vrt_takeover") != "0");
			/* int8 items only; no 4-bit wire format, no transmit. */
			narrow_ = true;
			vrt_ = true;
		} else if (args.count("port")) {
			link_ = std::make_unique<SerialLink>(
				args.at("port"),
				args.count("baud") ? std::stoul(args.at("baud")) : 0UL);
			/* About 500 kB/s each way: 250 kS/s fits as 8-bit items only. */
			narrow_ = true;
		} else if (args.count("badge")) {
			link_ = std::make_unique<FramedLink>(
				args.at("badge"),
				args.count("baud") ? std::stoul(args.at("baud")) : 5000000UL,
				!args.count("fast") || args.at("fast") != "0");
			narrow_ = true;
		} else {
			link_ = std::make_unique<UsbLink>(args.count("serial") ? args.at("serial") : "");
		}
		struct esdr_info info{};

		if (link_->ctrlIn(ESDR_REQ_GET_INFO, &info, sizeof(info)) == (int)sizeof(info) &&
		    info.magic == ESDR_INFO_MAGIC) {
			freqMinHz_ = (double)info.freq_min_hz;
			freqMaxHz_ = (double)info.freq_max_hz;
			rateMaxHz_ = (double)info.rx_rate_max_hz;
			rateSteps_ = info.rx_rate_count;
			rxGainMax_ = info.rx_gain_max;
			txGainMax_ = info.tx_gain_max;
		}
		/* max >> k, or what the device lists between them (ESDR_CAP_RATE_LIST). */
		for (unsigned k = 0; k < rateSteps_; k++) {
			rates_.push_back(rateMaxHz_ / (double)(1u << k));
		}
		if ((info.caps & ESDR_CAP_RATE_LIST) != 0U) {
			uint32_t list[ESDR_RATES_MAX];
			int got = link_->ctrlIn(ESDR_REQ_GET_RATES, list, sizeof(list));

			if (got >= 4) {
				rates_.assign(list, list + got / 4);
			}
		}
		if (rates_.empty()) {
			rates_.push_back(rateMaxHz_);
		}
		/* Start from what the device runs with. */
		struct esdr_state st{};

		if (link_->ctrlIn(ESDR_REQ_GET_STATE, &st, sizeof(st)) >=
		    (int)ESDR_STATE_MIN_SIZE) {
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
	}

	/* ---- identification ---- */
	std::string getDriverKey(void) const override { return "esdr"; }
	std::string getHardwareKey(void) const override
	{
		return vrt_ ? "ESP-SDR stream_eth (VITA 49.2 over UDP)" : "ESP-SDR osmosdr";
	}

	SoapySDR::Kwargs getHardwareInfo(void) const override
	{
		SoapySDR::Kwargs info;

		info["serial"] = link_->serial();
		return info;
	}

	/* ---- channels ---- */
	/* A device with no transmit gain does not transmit. */
	size_t getNumChannels(const int direction) const override
	{
		return direction == SOAPY_SDR_TX && txGainMax_ == 0 ? 0 : 1;
	}
	bool getFullDuplex(const int, const size_t) const override { return false; }

	/* ---- streaming ---- */
	std::vector<std::string> getStreamFormats(const int, const size_t) const override
	{
		return {SOAPY_SDR_CS8, SOAPY_SDR_CS16, SOAPY_SDR_CF32};
	}

	std::string getNativeStreamFormat(const int, const size_t, double &fullScale) const override
	{
		fullScale = narrow_ ? 127 : 32767;
		return narrow_ ? SOAPY_SDR_CS8 : SOAPY_SDR_CS16;
	}

	SoapySDR::Stream *setupStream(const int direction, const std::string &format,
				      const std::vector<size_t> &channels,
				      const SoapySDR::Kwargs &args) override
	{
		if (!channels.empty() && (channels.size() != 1 || channels[0] != 0)) {
			throw std::runtime_error("esdr: one channel only");
		}
		if (format != SOAPY_SDR_CS8 && format != SOAPY_SDR_CS16 && format != SOAPY_SDR_CF32) {
			throw std::runtime_error("esdr: format must be CS8, CS16 or CF32");
		}
		/* CF32 travels as 16-bit items, or 8-bit ones on the serial link. */
		int floatBits = narrow_ ? 8 : 16;

		if (direction == SOAPY_SDR_TX) {
			txFloat_ = format == SOAPY_SDR_CF32;
			txBits_ = format == SOAPY_SDR_CS8 ? 8 : format == SOAPY_SDR_CS16 ? 16 : floatBits;
			return reinterpret_cast<SoapySDR::Stream *>(&txTag_);
		}
		float_ = format == SOAPY_SDR_CF32;
		cs16_ = format == SOAPY_SDR_CS16;
		bits_ = format == SOAPY_SDR_CS8 ? 8 : format == SOAPY_SDR_CS16 ? 16 : floatBits;
		/* bits=4: 4-bit items on the wire where the device offers them (half the bytes of
		 * 8-bit ones: twice the rate a USB full-speed link carries), unpacked to CS8. */
		want4_ = args.count("bits") && args.at("bits") == "4";
		trigRatio_ = args.count("trigger") ? std::stod(args.at("trigger")) : 0.0;
		trigPost_ = args.count("post") ? (uint32_t)std::stoul(args.at("post"))
					       : ESDR_TRIG_SAMPLES / 2U;
		return reinterpret_cast<SoapySDR::Stream *>(&rxTag_);
	}

	void closeStream(SoapySDR::Stream *stream) override
	{
		deactivateStream(stream, 0, 0);
	}

	size_t getStreamMTU(SoapySDR::Stream *) const override { return 4096; }

	/* The receive stream's arguments (gr-soapy refuses any not listed). */
	SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t) const override
	{
		SoapySDR::ArgInfoList list;

		if (direction != SOAPY_SDR_RX || vrt_) {
			return list;
		}

		SoapySDR::ArgInfo bits, trigger, post;

		bits.key = "bits";
		bits.name = "Wire bits";
		bits.description = "4: 4-bit items on the wire where the device offers them";
		bits.type = SoapySDR::ArgInfo::STRING;
		bits.value = "";
		list.push_back(bits);
		trigger.key = "trigger";
		trigger.name = "Trigger ratio";
		trigger.description = "Triggered 80 MS/s snapshots of bursts this many times the "
				      "quiet power (a radio with the trigger mode); 0: a stream";
		trigger.type = SoapySDR::ArgInfo::FLOAT;
		trigger.value = "0";
		list.push_back(trigger);
		post.key = "post";
		post.name = "Samples after the trigger";
		post.type = SoapySDR::ArgInfo::INT;
		post.value = std::to_string(ESDR_TRIG_SAMPLES / 2U);
		list.push_back(post);
		return list;
	}

	/*
	 * Half duplex: starting one direction stops the other. GNU Radio calls this from a block's
	 * start(): a throw there ends the block's thread before the scheduler's start barrier, and
	 * top_block::start() never returns, which is a frozen gqrx. So a refusal is an error code.
	 */
	int activateStream(SoapySDR::Stream *stream, const int, const long long, const size_t) override
	{
		try {
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
		} catch (const std::exception &e) {
			SoapySDR_logf(SOAPY_SDR_ERROR, "%s", e.what());
			return SOAPY_SDR_STREAM_ERROR;
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
	 * Blocking write: the device holds the data back while its backlog is
	 * full, which paces the caller to the sample rate.
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
		done = link_->writeSamples(o, n * item, timeoutUs);
		if (done < 0) {
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
			if (bits_ == 8) {
				const int8_t *c = reinterpret_cast<const int8_t *>(conv_.data());

				for (size_t k = 0; k < 2 * n; k++) {
					out[k] = (float)c[k] * (1.0f / 128.0f);
				}
			} else {
				for (size_t k = 0; k < 2 * n; k++) {
					out[k] = (float)conv_[k] * (1.0f / 32768.0f);
				}
			}
		} else if (cs16_ && bits_ == 8) {
			size_t want = std::min(numElems, conv_.size() / 2);
			int16_t *out = reinterpret_cast<int16_t *>(buffs[0]);

			n = ring_.pop(reinterpret_cast<uint8_t *>(conv_.data()), want, (int)timeoutUs,
				      overflow);

			const int8_t *c = reinterpret_cast<const int8_t *>(conv_.data());

			for (size_t k = 0; k < 2 * n; k++) {
				out[k] = (int16_t)(c[k] * 256);
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
		if (link_->ctrlOut(ESDR_REQ_SET_RX_GAIN, automatic ? ESDR_GAIN_AUTO : (uint16_t)rxGain_,
				   nullptr, 0) < 0 && vrt_) {
			SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: gain mode refused");
			return;
		}
		rxAuto_ = automatic;
	}

	/*
	 * The VRT link's settings are the link's: it rolls them back on a refusal, also one that comes
	 * later and when the want was adopted from the board. The cache follows (every path).
	 */
	void syncFromLink() const
	{
		auto *self = const_cast<SoapyEsdr *>(this);
		struct esdr_state st{};

		if (vrt_ && !txRunning_ &&
		    link_->ctrlIn(ESDR_REQ_GET_STATE, &st, sizeof(st)) >= (int)ESDR_STATE_MIN_SIZE) {
			self->freqHz_ = (double)st.freq_hz;
			self->rateHz_ = (double)st.sample_rate_hz;
			self->rxAuto_ = st.rx_gain == ESDR_GAIN_AUTO;
			if (!self->rxAuto_) {
				self->rxGain_ = st.rx_gain; /* in automatic mode the manual value stays */
			}
		}
	}

	bool getGainMode(const int direction, const size_t) const override
	{
		syncFromLink();
		return direction == SOAPY_SDR_RX && rxAuto_;
	}

	void setGain(const int direction, const size_t channel, const double value) override
	{
		setGain(direction, channel, direction == SOAPY_SDR_RX ? "RF" : "TX", value);
	}

	void setGain(const int direction, const size_t, const std::string &, const double value) override
	{
		if (direction == SOAPY_SDR_RX) {
			int g = (int)std::max(0.0, std::min((double)rxGainMax_, value));

			if (link_->ctrlOut(ESDR_REQ_SET_RX_GAIN, (uint16_t)g, nullptr, 0) < 0 && vrt_) {
				SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: gain %d refused", g);
				return;
			}
			rxAuto_ = false;
			rxGain_ = g;
		} else {
			txGain_ = (int)std::max(0.0, std::min((double)txGainMax_, value));
			link_->ctrlOut(ESDR_REQ_SET_TX_GAIN, (uint16_t)txGain_, nullptr, 0);
		}
	}

	double getGain(const int direction, const size_t) const override
	{
		syncFromLink();
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
				setFreq((uint64_t)frequency);
			}
			return;
		}
		if (!txRunning_ && setFreq((uint64_t)frequency) < 0 && vrt_) {
			/* The VRT link refuses what the board refuses: the old frequency stays. */
			SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: %.0f Hz refused, staying at %.0f Hz", frequency,
				      freqHz_);
			return;
		}
		freqHz_ = frequency;
	}

	double getFrequency(const int direction, const size_t) const override
	{
		syncFromLink();
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
				setU32(ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)actual);
			}
			return;
		}
		double before = rateHz_;

		if (!txRunning_ && setU32(ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)actual) < 0 && vrt_) {
			SoapySDR_logf(SOAPY_SDR_WARNING, "esdr: %.0f S/s refused, staying at %.0f S/s", actual,
				      before);
			return;
		}
		rateHz_ = actual;
		/*
		 * The backlog is in time: a faster stream needs more room. Snapshots have no rate,
		 * and a reader tells them apart only by counting: a reset would cut one.
		 */
		if (rxRunning_ && trigRatio_ <= 0 && actual != before) {
			/*
			 * On the VRT link the board's restart for the new rate can reach the ring (and
			 * flag its discontinuity) before this reset, while the command waits for its
			 * acknowledge: the flag must survive it.
			 */
			ring_.reset(bits_ == 16 ? 4 : 2, rateHz_);
			if (vrt_) {
				/* The board's restart for the new rate is a discontinuity: always said. */
				ring_.flagLoss();
			}
		}
	}

	/* The device's rate nearest to @p rate on a log scale (its own rounding, esdr_dev.h). */
	double deviceRate(double rate) const
	{
		for (size_t k = 0; k + 1 < rates_.size(); k++) {
			if (rate * rate >= rates_[k] * rates_[k + 1]) {
				return rates_[k];
			}
		}
		return rates_.back();
	}

	double getSampleRate(const int direction, const size_t) const override
	{
		syncFromLink();
		return direction == SOAPY_SDR_TX ? txRateHz_ : rateHz_;
	}

	std::vector<double> listSampleRates(const int, const size_t) const override
	{
		return rates_;
	}

	/* The rates themselves, slowest first: nothing between them is taken as asked. */
	SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override
	{
		SoapySDR::RangeList ranges;

		for (auto r = rates_.rbegin(); r != rates_.rend(); ++r) {
			ranges.push_back(SoapySDR::Range(*r, *r));
		}
		return ranges;
	}

	void setBandwidth(const int, const size_t, const double bw) override
	{
		if (vrt_) {
			return; /* the board picks its bandwidth: nothing to cache that it refuses */
		}
		bwHz_ = bw;
		setU32(ESDR_REQ_SET_BANDWIDTH, (uint32_t)bw);
	}

	double getBandwidth(const int, const size_t) const override { return bwHz_; }

	bool hasFrequencyCorrection(const int direction, const size_t) const override
	{
		return direction == SOAPY_SDR_RX && !vrt_;
	}

	void setFrequencyCorrection(const int direction, const size_t, const double value) override
	{
		if (direction != SOAPY_SDR_RX || vrt_) {
			return;
		}
		corrPpm_ = value;
		setU32(ESDR_REQ_SET_FREQ_CORR, (uint32_t)(int32_t)(value * 1000.0));
	}

	double getFrequencyCorrection(const int, const size_t) const override { return corrPpm_; }

	/* Sensors: the radio chip's die temperature and its own crystal calibration (the
	 * library's, under the correction above), where the device has them. */
	std::vector<std::string> listSensors(void) const override
	{
		std::vector<std::string> keys = {"corr_ppb", "lo_residual_hz"};
		uint8_t b[2];

		/* The classic ESP32 has no sensor: asking for what is not listed fails. */
		if (link_->ctrlIn(ESDR_REQ_GET_TEMP, b, sizeof(b)) == (int)sizeof(b)) {
			keys.insert(keys.begin(), "temperature");
		}
		if (vrt_) {
			/* The board's crystal calibration is not on the Ethernet link. */
			keys = link_->sensorKeys();
		}
		return keys;
	}

	SoapySDR::ArgInfo getSensorInfo(const std::string &key) const override
	{
		SoapySDR::ArgInfo info;

		info.key = key;
		if (key.compare(0, 4, "vrt_") == 0) {
			info.name = "VRT link: " + key.substr(4);
			info.type = SoapySDR::ArgInfo::INT;
		} else if (key == "temperature") {
			info.name = "Die temperature";
			info.units = "C";
			info.type = SoapySDR::ArgInfo::FLOAT;
		} else if (key == "corr_ppb") {
			info.name = "Crystal correction in force on the device";
			info.units = "ppb";
			info.type = SoapySDR::ArgInfo::INT;
		} else if (key == "lo_residual_hz") {
			info.name = "LO offset the device's correction leaves";
			info.units = "Hz";
			info.type = SoapySDR::ArgInfo::INT;
		}
		return info;
	}

	std::string readSensor(const std::string &key) const override
	{
		std::string v;

		if (link_->readSensor(key, v)) {
			return v;
		}
		if (key == "temperature") {
			uint8_t b[2];
			char t[16];

			if (link_->ctrlIn(ESDR_REQ_GET_TEMP, b, sizeof(b)) != (int)sizeof(b)) {
				throw std::runtime_error("esdr: no temperature sensor");
			}
			snprintf(t, sizeof(t), "%.1f", (int16_t)(b[0] | b[1] << 8) / 10.0);
			return t;
		}
		struct esdr_cal c{};

		if (link_->ctrlIn(ESDR_REQ_GET_CAL, &c, sizeof(c)) != (int)sizeof(c)) {
			throw std::runtime_error("esdr: no crystal calibration on this device");
		}
		if (key == "corr_ppb") {
			return std::to_string(c.corr_ppb);
		}
		if (key == "lo_residual_hz") {
			return std::to_string(c.lo_residual_hz);
		}
		throw std::runtime_error("esdr: unknown sensor " + key);
	}

private:
	int setU32(uint8_t req, uint32_t v)
	{
		uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};

		return link_->ctrlOut(req, 0, b, 4);
	}

	int setFreq(uint64_t hz)
	{
		uint8_t b[8];

		for (int i = 0; i < 8; i++) {
			b[i] = (uint8_t)(hz >> (8 * i));
		}
		return link_->ctrlOut(ESDR_REQ_SET_FREQ, 0, b, 8);
	}

	bool isTx(SoapySDR::Stream *stream) const
	{
		return reinterpret_cast<const char *>(stream) == &txTag_;
	}

	void startTx()
	{
		link_->ctrlOut(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		setFreq((uint64_t)txFreqHz_);
		setU32(ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)txRateHz_);
		link_->ctrlOut(ESDR_REQ_SET_TX_GAIN, (uint16_t)txGain_, nullptr, 0);
		link_->ctrlOut(ESDR_REQ_SET_FORMAT, (uint16_t)txBits_, nullptr, 0);
		if (link_->ctrlOut(ESDR_REQ_SET_MODE, ESDR_MODE_TX, nullptr, 0) < 0) {
			throw std::runtime_error("esdr: the radio does not transmit");
		}
		txRunning_ = true;
	}

	void stopTx()
	{
		link_->ctrlOut(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		txRunning_ = false;
	}

	void startRx()
	{
		syncFromLink(); /* what the link wants, not what a refusal left in the cache */
		if (float_ || cs16_) {
			/* CF32 and CS16 travel as 16-bit items up to 250 kS/s, else as 8-bit ones
			 * widened here: the C6 offers 16 bit only up to there, and USB full speed
			 * carries 500 kS/s of 8-bit items, not of 16-bit ones. */
			bits_ = narrow_ || rateHz_ > 250000 ? 8 : 16;
		}
		if (trigRatio_ > 0) {
			/* Snapshots are 8-bit items whatever the format. */
			bits_ = 8;
		}
		wire4_ = want4_ && bits_ == 8 && trigRatio_ <= 0 && !vrt_;

		size_t itemBytes = bits_ == 16 ? 4 : 2;

		ring_.reset(itemBytes, rateHz_);
		expect_ = 0;
		haveExpect_ = false;
		synced_ = false;
		carryLen_ = 0;
		rxSamples_ = gaps_ = gapZeros_ = gapSkipped_ = resyncs_ = snaps_ = 0;
		link_->ctrlOut(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		setFreq((uint64_t)freqHz_);
		if (trigRatio_ > 0) {
			uint8_t le[4] = {(uint8_t)trigPost_, (uint8_t)(trigPost_ >> 8),
					 (uint8_t)(trigPost_ >> 16), (uint8_t)(trigPost_ >> 24)};

			link_->ctrlOut(ESDR_REQ_SET_RX_GAIN,
				       rxAuto_ ? ESDR_GAIN_AUTO : (uint16_t)rxGain_, nullptr, 0);
			if (link_->ctrlOut(ESDR_REQ_SET_TRIGGER, (uint16_t)std::lrint(trigRatio_ * 16),
					   le, 4) < 0 ||
			    link_->ctrlOut(ESDR_REQ_SET_MODE, ESDR_MODE_TRIGGER, nullptr, 0) < 0) {
				throw std::runtime_error("esdr: the radio has no trigger mode");
			}
			rxRunning_ = true;
			link_->startRx([this](const uint8_t *p, size_t n) { onBytes(p, n); });
			return;
		}
		setU32(ESDR_REQ_SET_SAMPLE_RATE, (uint32_t)rateHz_);
		if (bwHz_ > 0) {
			setU32(ESDR_REQ_SET_BANDWIDTH, (uint32_t)bwHz_);
		}
		link_->ctrlOut(ESDR_REQ_SET_RX_GAIN, rxAuto_ ? ESDR_GAIN_AUTO : (uint16_t)rxGain_,
			nullptr, 0);
		link_->ctrlOut(ESDR_REQ_SET_FORMAT, (uint16_t)(wire4_ ? 4 : bits_), nullptr, 0);
		if (link_->ctrlOut(ESDR_REQ_SET_MODE, ESDR_MODE_RX, nullptr, 0) < 0) {
			if (vrt_) {
				/* A refusal rolled the link's settings back: ours must tell what is asked now. */
				struct esdr_state st{};

				if (link_->ctrlIn(ESDR_REQ_GET_STATE, &st, sizeof(st)) >= (int)ESDR_STATE_MIN_SIZE) {
					freqHz_ = (double)st.freq_hz;
					rateHz_ = (double)st.sample_rate_hz;
					rxAuto_ = st.rx_gain == ESDR_GAIN_AUTO;
					if (!rxAuto_) {
						rxGain_ = st.rx_gain;
					}
				}
			}
			throw std::runtime_error(vrt_ ? "esdr: the board does not start the stream (see the log above)"
						      : "esdr: the radio refuses to receive (osmosdr_hs needs "
							"a high-speed USB link)");
		}

		rxRunning_ = true;
		link_->startRx([this](const uint8_t *p, size_t n) { onBytes(p, n); });
	}

	void stopRx()
	{
		/* The device first: with no transfers queued it would only fill and overflow. */
		link_->ctrlOut(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, nullptr, 0);
		link_->stopRx();
		rxRunning_ = false;
		SoapySDR_logf(SOAPY_SDR_INFO,
			      "esdr: rx stopped: %llu samples, %llu snapshots, %llu gaps (%llu zero "
			      "filled, %llu skipped), %llu dropped by a slow reader, %llu resync steps",
			      (unsigned long long)rxSamples_, (unsigned long long)snaps_,
			      (unsigned long long)gaps_, (unsigned long long)gapZeros_,
			      (unsigned long long)gapSkipped_, (unsigned long long)ring_.dropped(),
			      (unsigned long long)resyncs_);
		std::string line = link_->statusLine();

		if (!line.empty()) {
			SoapySDR_logf(SOAPY_SDR_INFO, "esdr: %s", line.c_str());
		}
	}

	/*
	 * The receive stream is split into whole EBLK blocks (the device never
	 * splits one across transfers) and converted into the ring. A gap in the
	 * device's sample index is filled with zeros, bounded, so GRC/gqrx see a
	 * continuous time axis instead of a jump. A longer one, which would push out
	 * what the ring holds, is counted and reported as SOAPY_SDR_END_BURST.
	 */
	/* A plausible header of a block kind this module decodes. */
	static bool headerOk(const uint8_t *p)
	{
		return esdr_block_hdr_ok(p, ESDR_BLK_GAP | ESDR_BLK_OVERFLOW | ESDR_BLK_16BIT |
					    ESDR_BLK_TEST | ESDR_BLK_4BIT | ESDR_BLK_TRIGGER);
	}

	void onBlock(const uint8_t *p)
	{
		struct esdr_block_hdr h{};

		std::memcpy(&h, p, sizeof(h));
		if ((h.flags & ESDR_BLK_TRIGGER) != 0U) {
			/* Snapshots back to back: no zeros between them. */
			snaps_ += (h.flags & ESDR_BLK_GAP) != 0U;
			ring_.push(p + sizeof(h), h.samples);
			rxSamples_ += h.samples;
			return;
		}
		if (haveExpect_ && h.index > expect_) {
			uint64_t missing = h.index - expect_;

			gaps_++;
			if (missing < 1000000U) {
				/* Missing samples become zeros: the time axis stays continuous. */
				static const uint8_t zeros[4 * 1024] = {0};
				size_t item = bits_ == 16 ? 4 : 2;

				for (uint64_t z = missing; z > 0;) {
					uint64_t k = std::min<uint64_t>(z, sizeof(zeros) / item);

					ring_.push(zeros, (size_t)k);
					z -= k;
				}
				gapZeros_ += missing;
			} else {
				gapSkipped_ += missing;
				ring_.flagLoss();
			}
		}
		if ((h.flags & ESDR_BLK_4BIT) != 0U) {
			/* Each nibble sign extended, times 16: CS8 items. */
			uint8_t wide[2 * ESDR_BLOCK_PAYLOAD];

			for (unsigned int k = 0; k < h.samples; k++) {
				uint8_t v = p[sizeof(h) + k];

				wide[2 * k] = (uint8_t)(v << 4);
				wide[2 * k + 1] = (uint8_t)(v & 0xf0);
			}
			ring_.push(wide, h.samples);
		} else {
			ring_.push(p + sizeof(h), h.samples);
		}
		rxSamples_ += h.samples;
		expect_ = h.index + h.samples;
		haveExpect_ = true;
	}

	/*
	 * Bulk data into whole blocks. Alignment is lost when an earlier session
	 * stopped mid-block; it is found again on the device's 64-byte packets.
	 */
	void onBytes(const uint8_t *p, size_t n)
	{
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

	std::unique_ptr<Link> link_;

	double freqMinHz_ = 100e6, freqMaxHz_ = 6000e6, rateMaxHz_ = 250e3;
	unsigned rateSteps_ = 5;
	/* The device's receive rates, fastest first (filled when it opens). */
	std::vector<double> rates_;
	unsigned rxGainMax_ = 82, txGainMax_ = 17;

	double freqHz_ = 100e6, rateHz_ = 250e3, bwHz_ = 0, corrPpm_ = 0;
	int rxGain_ = 0, txGain_ = 0;
	bool rxAuto_ = true;
	/* Serial link: CF32 and the native format travel as 8-bit items. */
	bool narrow_ = false;
	/* The VRT link: the board's Ethernet stream. */
	bool vrt_ = false;
	int bits_ = 16;
	bool want4_ = false, wire4_ = false;
	/* trigger=<ratio>: triggered snapshots (0: a stream), post=<samples> after the trigger. */
	double trigRatio_ = 0;
	uint32_t trigPost_ = ESDR_TRIG_SAMPLES / 2U;
	uint64_t snaps_ = 0;
	bool float_ = false, cs16_ = false;
	/* Stream handles: addresses of these tags. */
	char rxTag_ = 0, txTag_ = 0;
	/* Transmit: own frequency and rate (the device has one of each, set at activation). */
	double txFreqHz_ = 2450e6, txRateHz_ = 250e3;
	int txBits_ = 16;
	bool txFloat_ = false, txRunning_ = false;
	std::vector<uint8_t> txBuf_ = std::vector<uint8_t>(16 * ESDR_BLOCK_BYTES);
	/* CF32 staging: CS16 items popped from the ring. */
	std::vector<int16_t> conv_ = std::vector<int16_t>(2 * 65536);

	bool rxRunning_ = false;
	/* Receive parser state and counters (the link's receive thread). */
	uint64_t rxSamples_ = 0, gaps_ = 0, gapZeros_ = 0, gapSkipped_ = 0, resyncs_ = 0;
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

	/* The board's Ethernet stream is named, not found: nothing is sent here. */
	if (args.count("vrt")) {
		SoapySDR::Kwargs dev;

		dev["driver"] = "esdr";
		dev["vrt"] = args.at("vrt");
		for (const char *k : {"vrt_port", "vrt_takeover"}) {
			if (args.count(k)) {
				dev[k] = args.at(k);
			}
		}
		dev["label"] = "ESP-SDR stream_eth, VITA 49.2 over UDP (" + args.at("vrt") + ")";
		result.push_back(dev);
		return result;
	}
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
	/* The MCH2022 badge with apps/mch2022_coproc on its RP2040. */
	std::string badge;

	if (!args.count("port") &&
	    openCoproc(ctx, args.count("badge") ? args.at("badge") : "", nullptr, &badge)) {
		SoapySDR::Kwargs dev;

		dev["driver"] = "esdr";
		dev["badge"] = badge;
		dev["label"] = "ESP-SDR osmosdr_serial, MCH2022 badge (" + badge + ")";
		result.push_back(dev);
	}
	libusb_exit(ctx);
	/* The serial link (osmosdr_serial) is not probed: name its port. */
	if (args.count("port")) {
		SoapySDR::Kwargs dev;

		dev["driver"] = "esdr";
		dev["port"] = args.at("port");
		dev["label"] = "ESP-SDR osmosdr_serial (" + args.at("port") + ")";
		result.push_back(dev);
	}
	return result;
}

static SoapySDR::Device *makeEsdr(const SoapySDR::Kwargs &args)
{
	return new SoapyEsdr(args);
}

static SoapySDR::Registry registerEsdr("esdr", &findEsdr, &makeEsdr, SOAPY_SDR_ABI_VERSION);
