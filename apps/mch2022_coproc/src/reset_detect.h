/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What DTR and RTS on the first CDC ACM port ask of the ESP32. Plain logic, unit tested on
 * native_sim (tests/unit/mch2022_coproc); esp32_ctl.c acts on it.
 */

#ifndef RESET_DETECT_H_
#define RESET_DETECT_H_

#include <stdbool.h>
#include <stdint.h>

struct reset_detect {
	uint8_t mode;
	/* The stock detectors. */
	bool prev_dtr, prev_rts;
	uint8_t download_state, app_state;
};

enum reset_action {
	RESET_ACTION_NONE,
	/* Reset into the application. */
	RESET_ACTION_APP,
	/* Reset into download mode (a set reset lock makes it an application reset). */
	RESET_ACTION_DOWNLOAD,
};

/* What the circuit mode wants of the lines. */
struct reset_lines {
	bool en_low, io0_low;
};

void reset_detect_init(struct reset_detect *d, uint8_t mode);

/* The detectors start over: a reset just happened. Keeps the lines' last state. */
void reset_detect_clear(struct reset_detect *d);

/*
 * COPROC_RESET_STOCK: a new state of the lines, as the stock firmware reads it: each line ORed
 * with its last state, so that two changes close together still pass through a state where both
 * are up. Ignored (the last state kept) while @p reset_active.
 */
enum reset_action reset_detect_stock(struct reset_detect *d, bool dtr, bool rts, bool reset_active);

/*
 * COPROC_RESET_CIRCUIT: the usual two transistors from DTR and RTS to EN and IO0: RTS alone holds
 * EN low, DTR alone holds IO0 low, both up or both down hold neither.
 */
struct reset_lines reset_detect_circuit(bool dtr, bool rts);

#endif /* RESET_DETECT_H_ */
