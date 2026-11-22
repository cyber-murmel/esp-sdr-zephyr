// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * soapy_capture: a SoapySDR C API client to test the esdr module against tests/vrt_sim.c.
 *
 *   soapy_capture <device args> [-r rate] [-f Hz] [-g gain|auto] [-t seconds] [-F format CS8|CS16|CF32]
 *                 [-c cycles] [-R rate2] [-T Hz2] [-s]
 *
 * Opens the device, sets rate, frequency and gain, then c times (default 1): activates a stream,
 * reads for t seconds, deactivates. With -R / -T the rate / frequency is changed half way while
 * streaming. It counts samples, read timeouts, END_BURST (overflow) returns, and checks the
 * samples against vrt_sim's ramp (a 16 bit counter, I low byte, Q high byte; CS8 and CS16 and
 * CF32 are converted back to it): runs of zero samples are what the module zero filled, breaks
 * are jumps in the counter. With -s the sensors are printed at the end. Prints the sample rate
 * measured over the capture. Exits 1 on a failure to open or stream.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Logger.h>

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

struct x_args {
	SoapySDRDevice *dev;
	double freq, delay_ms, base;
	int storm;
	int sets;
	double max_ms;
};

/* A set made during the activation (-X, after -Y ms), or a storm of sets for a second (-Z). */
static void *xthread(void *arg)
{
	struct x_args *a = arg;

	if (!a->storm) {
		usleep((useconds_t)(a->delay_ms * 1000.0));
		SoapySDRDevice_setFrequency(a->dev, SOAPY_SDR_RX, 0, a->freq, NULL);
		return NULL;
	}
	double t_end = now_s() + 1.0;

	while (now_s() < t_end) {
		double t = now_s();

		SoapySDRDevice_setFrequency(a->dev, SOAPY_SDR_RX, 0, a->base + (a->sets & 1 ? 1000.0 : 0.0), NULL);
		double ms = (now_s() - t) * 1000.0;

		if (ms > a->max_ms) {
			a->max_ms = ms;
		}
		a->sets++;
		usleep(5000);
	}
	return NULL;
}

struct sw_args {
	SoapySDRDevice *dev;
	double rate2, freq2, freq3, gain2, t0, wait3;
	int unauto;
};

static void *switcher(void *arg)
{
	struct sw_args *a = arg;

	if (a->rate2 > 0) {
		SoapySDRDevice_setSampleRate(a->dev, SOAPY_SDR_RX, 0, a->rate2);
	}
	if (a->freq2 > 0) {
		SoapySDRDevice_setFrequency(a->dev, SOAPY_SDR_RX, 0, a->freq2, NULL);
	}
	if (a->wait3 > 0) {
		usleep((useconds_t)(a->wait3 * 1e6));
	}
	if (a->unauto) { /* manual gain again: the value set before the automatic mode must go out */
		SoapySDRDevice_setGainMode(a->dev, SOAPY_SDR_RX, 0, 0);
	}
	if (a->freq3 > 0) { /* straight back: A to B to A */
		SoapySDRDevice_setFrequency(a->dev, SOAPY_SDR_RX, 0, a->freq3, NULL);
		if (a->wait3 > 0) {
			sleep(1); /* the worker sends it while the link is gone: let it */
		}
	}
	if (a->gain2 > -2) {
		SoapySDRDevice_setGain(a->dev, SOAPY_SDR_RX, 0, a->gain2);
	}
	printf("switched at %.2f s: rate %.0f, freq %.0f\n", a->t0,
	       SoapySDRDevice_getSampleRate(a->dev, SOAPY_SDR_RX, 0),
	       SoapySDRDevice_getFrequency(a->dev, SOAPY_SDR_RX, 0));
	return NULL;
}

int main(int argc, char **argv)
{
	struct sw_args sw;
	pthread_t swt;
	int have_swt = 0;

	double rate = 20e6, freq = 2412e6, gain = -1, secs = 2, rate2 = 0, freq2 = 0, freq3 = 0, gain2 = -2;
	const char *fmt = SOAPY_SDR_CS8;
	int cycles = 1, opt, sensors = 0, keepgoing = 0, storm = 0, automode = 0;
	double wait3 = 0;
	double xfreq = 0, xdelay = 0;
	const char *dev_args;

	if (argc < 2) {
		fprintf(stderr, "usage: soapy_capture <args> [options]\n");
		return 2;
	}
	dev_args = argv[1];
	optind = 2;
	while ((opt = getopt(argc, argv, "r:f:g:t:F:c:R:T:B:G:sKX:Y:ZW:M")) != -1) {
		switch (opt) {
		case 'r': rate = atof(optarg); break;
		case 'f': freq = atof(optarg); break;
		case 'g': gain = strcmp(optarg, "auto") == 0 ? -1 : atof(optarg); break;
		case 't': secs = atof(optarg); break;
		case 'F': fmt = optarg; break;
		case 'c': cycles = atoi(optarg); break;
		case 'R': rate2 = atof(optarg); break;
		case 'T': freq2 = atof(optarg); break;
		case 'B': freq3 = atof(optarg); break;
		case 'G': gain2 = atof(optarg); break;
		case 's': sensors = 1; break;
		case 'K': keepgoing = 1; break;
		case 'X': xfreq = atof(optarg); break;
		case 'Y': xdelay = atof(optarg); break;
		case 'Z': storm = 1; break;
		case 'W': wait3 = atof(optarg); break;
		case 'M': automode = 1; break;
		default: return 2;
		}
	}
	SoapySDRDevice *dev = SoapySDRDevice_makeStrArgs(dev_args);

	if (dev == NULL) {
		fprintf(stderr, "soapy_capture: cannot open: %s\n", SoapySDRDevice_lastError());
		return 1;
	}
	size_t item = strcmp(fmt, SOAPY_SDR_CF32) == 0 ? 8 : strcmp(fmt, SOAPY_SDR_CS16) == 0 ? 4 : 2;
	size_t nbuf = 1 << 16;
	uint8_t *buf = malloc(nbuf * item);
	size_t chan = 0;
	int rc = 0;

	if (SoapySDRDevice_setSampleRate(dev, SOAPY_SDR_RX, 0, rate) != 0) {
		fprintf(stderr, "setSampleRate: %s\n", SoapySDRDevice_lastError());
	}
	if (SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, freq, NULL) != 0) {
		fprintf(stderr, "setFrequency: %s\n", SoapySDRDevice_lastError());
	}
	if (gain < 0) {
		SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, 1);
	} else {
		SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, 0);
		SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain);
	}
	if (automode) {
		SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, 0);
		SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, 40);
		SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, 1);
	}
	printf("device: rate %.0f, freq %.0f, gain %.0f%s\n", SoapySDRDevice_getSampleRate(dev, SOAPY_SDR_RX, 0),
	       SoapySDRDevice_getFrequency(dev, SOAPY_SDR_RX, 0), SoapySDRDevice_getGain(dev, SOAPY_SDR_RX, 0),
	       SoapySDRDevice_getGainMode(dev, SOAPY_SDR_RX, 0) ? " (auto)" : "");
	for (int c = 0; c < cycles; c++) {
		SoapySDRStream *st = SoapySDRDevice_setupStream(dev, SOAPY_SDR_RX, fmt, &chan, 1, NULL);
		struct x_args xa = {dev, xfreq, xdelay, freq, 0, 0, 0};
		pthread_t xt;
		int have_xt = 0;

		if (xfreq > 0) {
			pthread_create(&xt, NULL, xthread, &xa);
			have_xt = 1;
		}
		uint64_t total = 0, timeouts = 0, startup_timeouts = 0, bursts = 0, zeros = 0, breaks = 0, errs = 0;
		uint32_t prev = 0;
		int have = 0, switched = 0;
		double t0, tend, tfirst = 0;

		if (st == NULL || SoapySDRDevice_activateStream(dev, st, 0, 0, 0) != 0) {
			fprintf(stderr, "stream: %s\n", SoapySDRDevice_lastError());
			rc = 1;
			if (keepgoing) {
				printf("cycle %d: activation failed; now rate %.0f, freq %.0f, gain %.0f\n", c,
				       SoapySDRDevice_getSampleRate(dev, SOAPY_SDR_RX, 0),
				       SoapySDRDevice_getFrequency(dev, SOAPY_SDR_RX, 0),
				       SoapySDRDevice_getGain(dev, SOAPY_SDR_RX, 0));
				continue;
			}
			break;
		}
		t0 = now_s();
		tend = t0 + secs + 5; /* the capture proper is secs from the first sample */
		while (now_s() < tend) {
			if (storm && !have_xt && total > 0 && now_s() - tfirst > secs / 4) {
				/* by time, also when the samples have stopped (a stream taken by another host) */
				xa.storm = 1;
				pthread_create(&xt, NULL, xthread, &xa);
				have_xt = 1;
			}
			void *b[1] = {buf};
			int flags = 0;
			long long tns = 0;
			int n = SoapySDRDevice_readStream(dev, st, b, nbuf, &flags, &tns, 200000);

			if (n == SOAPY_SDR_TIMEOUT) {
				/* before the first sample it is the start (load, a first context): counted apart */
				if (total == 0) {
					startup_timeouts++;
				} else {
					timeouts++;
				}
				continue;
			}
			if (n < 0) {
				errs++;
				fprintf(stderr, "readStream: %s\n", SoapySDR_errToStr(n));
				break;
			}
			if (flags & SOAPY_SDR_END_BURST) {
				bursts++;
			}
			if (total == 0) {
				tfirst = now_s();
				tend = tfirst + secs;
			}
			if (!switched && (rate2 > 0 || freq2 > 0 || gain2 > -2 || automode) && total > 0 &&
			    now_s() - tfirst > secs / 2) {
				/* the control calls in a thread of their own, as an application has: reading goes on */
				switched = 1;
				sw.dev = dev;
				sw.rate2 = rate2;
				sw.freq2 = freq2;
				sw.freq3 = freq3;
				sw.gain2 = gain2;
				sw.t0 = now_s() - tfirst;
				sw.wait3 = wait3;
				sw.unauto = automode;
				pthread_create(&swt, NULL, switcher, &sw);
				have_swt = 1;
			}
			for (int k = 0; k < n; k++) {
				uint32_t v;

				if (item == 2) {
					v = (uint32_t)buf[2 * k] | (uint32_t)buf[2 * k + 1] << 8;
				} else if (item == 4) {
					const int16_t *p = (const int16_t *)buf;

					v = (uint32_t)(uint8_t)(p[2 * k] >> 8) | (uint32_t)(uint8_t)(p[2 * k + 1] >> 8) << 8;
				} else {
					const float *p = (const float *)buf;

					v = (uint32_t)(uint8_t)(int8_t)lrintf(p[2 * k] * 128.0f) |
					    (uint32_t)(uint8_t)(int8_t)lrintf(p[2 * k + 1] * 128.0f) << 8;
				}
				if (v == 0 && (prev + 1) % 65536U != 0) {
					zeros++; /* the module's fill: counts as continuing */
					prev = (prev + 1) & 0xffffU;
				} else {
					if (have && v != ((prev + 1) & 0xffffU)) {
						breaks++;
					}
					prev = v;
				}
				have = 1;
			}
			total += (uint64_t)n;
		}
		if (have_swt) {
			pthread_join(swt, NULL);
			have_swt = 0;
		}
		if (have_xt) {
			pthread_join(xt, NULL);
			if (storm) {
				printf("storm: %d sets, max set call %.0f ms\n", xa.sets, xa.max_ms);
			}
		}
		double dt = now_s() - (tfirst > 0 ? tfirst : t0);

		if (sensors && c == cycles - 1) {
			size_t n;
			char **keys = SoapySDRDevice_listSensors(dev, &n);

			for (size_t k = 0; k < n; k++) {
				char *v = SoapySDRDevice_readSensor(dev, keys[k]);

				printf("sensor %s = %s\n", keys[k], v ? v : "?");
				free(v);
			}
			SoapySDRStrings_clear(&keys, n);
		}
		SoapySDRDevice_deactivateStream(dev, st, 0, 0);
		SoapySDRDevice_closeStream(dev, st);
		printf("cycle %d: %llu samples in %.2f s = %.3f MS/s, timeouts %llu, END_BURST %llu, zero "
		       "samples %llu, breaks %llu%s (first sample after %.2f s, %llu timeouts before)\n",
		       c, (unsigned long long)total, dt, (double)total / dt / 1e6,
		       (unsigned long long)timeouts, (unsigned long long)bursts,
		       (unsigned long long)zeros, (unsigned long long)breaks, errs ? ", READ ERROR" : "",
		       tfirst > 0 ? tfirst - t0 : -1.0, (unsigned long long)startup_timeouts);
		if (errs) {
			rc = 1;
		}
	}
	free(buf);
	SoapySDRDevice_unmake(dev);
	return rc;
}
