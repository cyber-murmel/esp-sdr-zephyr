/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Burst I/Q pipeline across both cores: CPU 0 captures, CPU 1 processes.
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>

#include "stream.h"
#include "transport.h"
#include "tx_rx.h"
#include "usb.h"

LOG_MODULE_REGISTER(rx_stream, LOG_LEVEL_INF);

#define CAPTURE_CPU 0
#define PROCESS_CPU 1
#define CAPTURE_STACK_SIZE 2048
#define PROCESS_STACK_SIZE 3072

/* Bursts live above the capture bank: frees low DRAM for the USB buffers. */
K_MEM_SLAB_DEFINE_IN_SECT(burst_slab, Z_GENERIC_SECTION(.esp_sdr_high), sizeof(struct burst),
			  CONFIG_APP_BURSTS, 4);
K_MSGQ_DEFINE(burst_q, sizeof(struct burst *), CONFIG_APP_BURSTS, 4);

K_THREAD_STACK_DEFINE(capture_stack, CAPTURE_STACK_SIZE);
K_THREAD_STACK_DEFINE(process_stack, PROCESS_STACK_SIZE);
static struct k_thread capture_thread, process_thread;

static atomic_t capture_cpu = ATOMIC_INIT(-1), process_cpu = ATOMIC_INIT(-1);
static atomic_t stalls;
/* Decimation factor, 1 = raw bursts at the capture rate. */
static atomic_t rx_decim = ATOMIC_INIT(1);

/* Decimated bursts must fit the burst buffer. */
BUILD_ASSERT(ESP_SDR_SAMPLES_MAX / RX_DECIM_MIN <= CONFIG_APP_BURST_SAMPLES + 3U);
BUILD_ASSERT(sizeof(struct iq16) == sizeof(struct esp_sdr_iq16));

int rx_set_rate(uint32_t hz)
{
	uint32_t fs = esp_sdr_rate_hz(CONFIG_APP_RATE), m;

	if (hz == 0U || fs % hz != 0U) {
		return -EINVAL;
	}
	m = fs / hz;
	if (m != 1U && (m < RX_DECIM_MIN || m > ESP_SDR_DECIM_MAX)) {
		return -EINVAL;
	}
	atomic_set(&rx_decim, (atomic_val_t)m);
	return 0;
}

uint32_t rx_get_rate(void)
{
	return esp_sdr_rate_hz(CONFIG_APP_RATE) / (uint32_t)atomic_get(&rx_decim);
}

static void capture(void *p1, void *p2, void *p3)
{
	enum esp_sdr_rate rate = CONFIG_APP_RATE;
	uint32_t seq = 0;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	ret = esp_sdr_init();
	if (ret == 0) {
		ret = esp_sdr_set_frequency(CONFIG_APP_FREQ_MHZ);
	}
	if (ret != 0) {
		LOG_ERR("radio setup failed (%d)", ret);
		return;
	}

	for (;;) {
		struct esp_sdr_burst raw;
		struct burst *b;

		if (k_mem_slab_alloc(&burst_slab, (void **)&b, K_NO_WAIT) != 0) {
			/* Process side is behind: wait, which also lets it run on one CPU. */
			atomic_inc(&stalls);
			(void)k_mem_slab_alloc(&burst_slab, (void **)&b, K_FOREVER);
		}
		uint32_t m = (uint32_t)atomic_get(&rx_decim);

		if (m > 1U) {
			/* Whole bank, decimated on the capture core while it still holds the bank. */
			size_t n;
			uint64_t first;

			ret = esp_sdr_capture_decimated(rate, ESP_SDR_SAMPLES_MAX / m * m, m,
							(struct esp_sdr_iq16 *)b->iq,
							ARRAY_SIZE(b->iq), &n, &first);
			b->count = n;
			b->timestamp_ns = first;
			b->rate_hz = esp_sdr_rate_hz(rate) / m;
			b->full_scale = 32768;
		} else {
			ret = esp_sdr_capture(rate, CONFIG_APP_BURST_SAMPLES, &raw);
			if (ret == 0) {
				b->count = raw.count;
				/* Time of the first sample: the burst ended just now. */
				b->timestamp_ns = k_cyc_to_ns_floor64(k_cycle_get_64()) -
						  (uint64_t)raw.count * NSEC_PER_SEC /
							  esp_sdr_rate_hz(rate);
				b->rate_hz = esp_sdr_rate_hz(rate);
				b->full_scale = 512;
				for (size_t j = 0; j < raw.count; j++) {
					b->iq[j].i = esp_sdr_i(raw.words[j]);
					b->iq[j].q = esp_sdr_q(raw.words[j]);
				}
			}
		}
		if (ret != 0) {
			LOG_WRN("capture failed (%d)", ret);
			k_mem_slab_free(&burst_slab, b);
			k_sleep(K_MSEC(100));
			continue;
		}
		b->seq = seq++;
		b->freq_mhz = esp_sdr_get_frequency();
		b->gain = esp_sdr_get_gain();
		atomic_set(&capture_cpu, arch_curr_cpu()->id);
		k_msgq_put(&burst_q, &b, K_FOREVER);

		if (CONFIG_APP_BURST_INTERVAL_MS > 0) {
			k_sleep(K_MSEC(CONFIG_APP_BURST_INTERVAL_MS));
		}
	}
}

#if defined(CONFIG_SCHED_CPU_MASK) && CONFIG_APP_NET_CPU >= 0
/*
 * The threads that would delay the DAC leader on the other CPU go there:
 * cooperative ones (it cannot preempt them) and those above its priority.
 * Measured before: sysworkq, net_mgmt and the like caused 2 to 9 ms stalls,
 * i.e. DAC restarts. Lower priority threads (shell, logging, VITA transmit)
 * stay free, so a busy capture core (decimation) cannot starve them.
 */
static const char *const net_thread_prefixes[] = {
	"tx_q[", "rx_q[", "usbd", "usb_otg", "net_mgmt", "sysworkq", "esp32_wifi_event",
};
struct net_threads {
	k_tid_t tid[16];
	size_t n;
};

static void find_net_thread(const struct k_thread *thread, void *user_data)
{
	struct net_threads *found = user_data;
	const char *name = k_thread_name_get((k_tid_t)thread);

	if (name == NULL || found->n == ARRAY_SIZE(found->tid)) {
		return;
	}
	ARRAY_FOR_EACH(net_thread_prefixes, i) {
		if (strncmp(name, net_thread_prefixes[i], strlen(net_thread_prefixes[i])) == 0) {
			found->tid[found->n++] = (k_tid_t)thread;
			return;
		}
	}
}

/* Pinning only takes while a thread is not runnable: retry until it waits. */
static void pin_net_threads(void)
{
	struct net_threads found = {0};
	unsigned int pinned = 0;

	k_thread_foreach(find_net_thread, &found);
	for (size_t t = 0; t < found.n; t++) {
		for (int tries = 0; tries < 100; tries++) {
			if (k_thread_cpu_pin(found.tid[t], CONFIG_APP_NET_CPU) == 0) {
				pinned++;
				break;
			}
			k_sleep(K_MSEC(1));
		}
	}
	LOG_INF("%u of %u network and USB threads pinned to cpu %d", pinned,
		(unsigned int)found.n, CONFIG_APP_NET_CPU);
}
#endif

static void process(void *p1, void *p2, void *p3)
{
	uint32_t bursts = 0, samples = 0;
	int64_t window_start = k_uptime_get();
	float power = 0.0f;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct burst *b;
		uint64_t sum = 0;
		int64_t now;

		k_msgq_get(&burst_q, &b, K_FOREVER);
		atomic_set(&process_cpu, arch_curr_cpu()->id);

		/* Integer energy: at most 2 * 32768^2 per sample, fits 32 bits unsigned. */
		for (size_t j = 0; j < b->count; j++) {
			int32_t i = b->iq[j].i, q = b->iq[j].q;

			sum += (uint32_t)(i * i) + (uint32_t)(q * q);
		}
		power = (float)sum / (float)b->count /
			((float)b->full_scale * (float)b->full_scale);
		stream_burst(b);
		bursts++;
		samples += b->count;
		k_mem_slab_free(&burst_slab, b);

		now = k_uptime_get();
		if (now - window_start >= 1000) {
			float secs = (float)(now - window_start) / 1000.0f;

			LOG_DBG("capture cpu %ld, process cpu %ld, %u bursts/s, "
				"%.2f MS/s, %.1f dBFS, stalls %ld",
				atomic_get(&capture_cpu), atomic_get(&process_cpu),
				(unsigned int)((float)bursts / secs),
				(double)((float)samples / secs / 1e6f),
				(double)(10.0f * log10f(MAX(power, 1e-12f))), atomic_get(&stalls));
			stream_report();
			bursts = 0;
			samples = 0;
			window_start = now;
		}
	}
}

int main(void)
{
	LOG_INF("cpus %u, %u MHz, %u-sample bursts", arch_num_cpus(), CONFIG_APP_FREQ_MHZ,
		CONFIG_APP_BURST_SAMPLES);

#if defined(CONFIG_SCHED_CPU_MASK) && CONFIG_APP_NET_CPU >= 0
	/*
	 * Before USB starts: its bring-up runs on the system work queue, and
	 * the controller interrupt lands on the CPU that allocates it.
	 */
	if (arch_num_cpus() > 1) {
		pin_net_threads();
	}
#endif
	if (usb_init() != 0) {
		LOG_ERR("usb init failed");
	}
	if (stream_init() != 0) {
		LOG_ERR("stream init failed");
		return 0;
	}
	if (IS_ENABLED(CONFIG_APP_TX) && tx_rx_init() != 0) {
		LOG_ERR("tx init failed");
	}

	if (!IS_ENABLED(CONFIG_APP_SDR)) {
		/* USB bring-up test: a small datagram per second instead of samples. */
		static const char beacon[] = "rx stream: beacon\n";

		for (;;) {
			(void)transport_send(beacon, sizeof(beacon) - 1);
			k_sleep(K_SECONDS(1));
		}
	}

	k_thread_create(&capture_thread, capture_stack, K_THREAD_STACK_SIZEOF(capture_stack),
			capture, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_FOREVER);
	k_thread_create(&process_thread, process_stack, K_THREAD_STACK_SIZEOF(process_stack),
			process, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_FOREVER);
	k_thread_name_set(&capture_thread, "sdr_capture");
	k_thread_name_set(&process_thread, "sdr_process");
#if defined(CONFIG_SCHED_CPU_MASK)
	if (arch_num_cpus() > 1) {
		k_thread_cpu_pin(&capture_thread, CAPTURE_CPU);
		k_thread_cpu_pin(&process_thread, PROCESS_CPU);
#if CONFIG_APP_NET_CPU >= 0
		/* Again for the threads started since (usbd, UDC). */
		pin_net_threads();
#endif
	}
#endif
	k_thread_start(&capture_thread);
	k_thread_start(&process_thread);
	return 0;
}
