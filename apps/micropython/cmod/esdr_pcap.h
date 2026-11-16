/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esdr_pcap.c's functions for the esdr module's globals (modesdr.c, esdr_radio.c).
 */

#ifndef ESDR_PCAP_H_
#define ESDR_PCAP_H_

#include "py/obj.h"

#include "pcap_proto.h"

extern const mp_obj_fun_builtin_var_t esdr_pcap_new_obj, esdr_pcap_tune_obj, esdr_pcap_feed_obj,
	esdr_pcap_flush_obj, esdr_pcapng_epb_obj, esdr_pcap_take_obj;
extern const mp_obj_fun_builtin_fixed_t esdr_pcap_stats_obj, esdr_pcapng_head_obj,
	esdr_pcap_pending_obj, esdr_pcap_dissect_obj;

/*
 * The engine (esdr_pcap.c): its detector and decoder threads read the radio's samples where
 * they land. ESDR_PCAP_ENGINE_STAGE: the ring stage in internal RAM (the S3 in multiapp, the
 * S31). ESDR_PCAP_ENGINE_LINK: the receive ring in the heap that esdr_remote.c's parser fills
 * (the P4, its C6 behind the link: only esdr_remote.c has the reader), pcap_start() takes it.
 */
#if defined(CONFIG_ESP_SDR_RING_STAGE)
#define ESDR_PCAP_ENGINE       1
#define ESDR_PCAP_ENGINE_STAGE 1
#elif defined(CONFIG_SOC_SERIES_ESP32P4)
#define ESDR_PCAP_ENGINE      1
#define ESDR_PCAP_ENGINE_LINK 1
/* Samples the ring takes at least: some 65 ms at 2 MS/s, 64 of the link's blocks. */
#define ESDR_PCAP_RING_MIN    131072
#endif

#ifdef ESDR_PCAP_ENGINE
extern const mp_obj_fun_builtin_var_t esdr_pcap_start_obj, esdr_pcap_take_pkts_obj;
extern const mp_obj_fun_builtin_fixed_t esdr_pcap_stop_obj;

/* The engine stops before the heap goes (a soft reset: esdr_port_deinit()). */
void esdr_pcap_deinit(void);

#ifdef ESDR_PCAP_ENGINE_LINK
/* pcap_start(state, ring) needs a ring of at least this many samples. */
#define ESDR_PCAP_RING_GLOBALS                                                                    \
	{MP_ROM_QSTR(MP_QSTR_PCAP_RING_MIN), MP_ROM_INT(ESDR_PCAP_RING_MIN)},
#else
#define ESDR_PCAP_RING_GLOBALS
#endif

#define ESDR_PCAP_ENGINE_GLOBALS                                                                  \
	{MP_ROM_QSTR(MP_QSTR_pcap_start), MP_ROM_PTR(&esdr_pcap_start_obj)},                       \
	{MP_ROM_QSTR(MP_QSTR_pcap_stop), MP_ROM_PTR(&esdr_pcap_stop_obj)},                         \
	{MP_ROM_QSTR(MP_QSTR_pcap_take_pkts), MP_ROM_PTR(&esdr_pcap_take_pkts_obj)},               \
	ESDR_PCAP_RING_GLOBALS
#else
#define ESDR_PCAP_ENGINE_GLOBALS
#endif

#define ESDR_PCAP_GLOBALS                                                                         \
	{MP_ROM_QSTR(MP_QSTR_pcap_new), MP_ROM_PTR(&esdr_pcap_new_obj)},                           \
	{MP_ROM_QSTR(MP_QSTR_pcap_tune), MP_ROM_PTR(&esdr_pcap_tune_obj)},                         \
	{MP_ROM_QSTR(MP_QSTR_pcap_feed), MP_ROM_PTR(&esdr_pcap_feed_obj)},                         \
	{MP_ROM_QSTR(MP_QSTR_pcap_flush), MP_ROM_PTR(&esdr_pcap_flush_obj)},                       \
	{MP_ROM_QSTR(MP_QSTR_pcap_stats), MP_ROM_PTR(&esdr_pcap_stats_obj)},                       \
	{MP_ROM_QSTR(MP_QSTR_pcapng_head), MP_ROM_PTR(&esdr_pcapng_head_obj)},                     \
	{MP_ROM_QSTR(MP_QSTR_pcapng_epb), MP_ROM_PTR(&esdr_pcapng_epb_obj)},                       \
	{MP_ROM_QSTR(MP_QSTR_pcap_take), MP_ROM_PTR(&esdr_pcap_take_obj)},                         \
	{MP_ROM_QSTR(MP_QSTR_pcap_pending), MP_ROM_PTR(&esdr_pcap_pending_obj)},                   \
	{MP_ROM_QSTR(MP_QSTR_pcap_dissect), MP_ROM_PTR(&esdr_pcap_dissect_obj)},                   \
	ESDR_PCAP_ENGINE_GLOBALS                                                                   \
	{MP_ROM_QSTR(MP_QSTR_PCAP_DEC_DEFAULT), MP_ROM_INT(PCAP_DEC_DEFAULT)},                     \
	{MP_ROM_QSTR(MP_QSTR_PCAP_DEC_BAD_CRC), MP_ROM_INT(PCAP_DEC_BAD_CRC)},

#endif /* ESDR_PCAP_H_ */
