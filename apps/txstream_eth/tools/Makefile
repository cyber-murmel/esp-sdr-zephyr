# SPDX-License-Identifier: GPL-3.0-or-later
# Host tools of the transmit stream: vrt_tx (sender), tx_fake (a stand-in board on the loopback)
# and test_tx (unit test of the protocol and the pacing against a simulated board).
#   make            build vrt_tx tx_fake
#   make test       build test_tx with -fsanitize=address,undefined and run it, then loopback_test.sh
# Plain gcc, no library but libc, libm and pthreads. OUT=dir puts the binaries elsewhere.
OUT ?= .
CC ?= gcc
WARN = -Wall -Wextra -Wshadow
CFLAGS ?= -O2
SRC = ../src
TXS = ../../../lib/esp_sdr/src/txs
INC = -I$(SRC) -I. -Istub -I$(TXS)
HDRS = $(SRC)/tx_proto.h tx_pace.h tx_board.h tx_src.h

all: $(OUT)/vrt_tx $(OUT)/tx_fake

$(OUT)/vrt_tx: vrt_tx.c $(HDRS)
	$(CC) $(CFLAGS) $(WARN) -pthread $(INC) -o $@ vrt_tx.c -lm

$(OUT)/tx_fake: tx_fake.c $(HDRS) $(TXS)/txs_wire.c
	$(CC) $(CFLAGS) $(WARN) -pthread $(INC) -o $@ tx_fake.c $(TXS)/txs_wire.c -lm

$(OUT)/test_tx: test_tx.c $(HDRS) $(TXS)/txs_wire.c $(TXS)/txs_credit.c
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread $(INC) \
		-o $@ test_tx.c $(TXS)/txs_wire.c $(TXS)/txs_credit.c -lm

test: $(OUT)/test_tx $(OUT)/vrt_tx $(OUT)/tx_fake
	$(OUT)/test_tx
	./loopback_test.sh $(OUT)/vrt_tx $(OUT)/tx_fake

clean:
	rm -f $(OUT)/vrt_tx $(OUT)/tx_fake $(OUT)/test_tx

.PHONY: all test clean
