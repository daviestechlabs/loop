CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS += -I. -D_POSIX_C_SOURCE=200809L

.PHONY: all test clean

all: c-pb c-pb-test

c-pb: c_pb_main.c pb_cli_codec.c pb_wire.c pb_msg.c pb_wire.h pb_msg.h pb_dnd_request.h pb_dnd_campaign.h pb_dnd_action.h
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

c-pb-test: test_pb.c pb_wire.c pb_msg.c pb_wire.h pb_msg.h pb_dnd_request.h pb_dnd_campaign.h pb_dnd_action.h
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

test: c-pb-test c-pb
	./c-pb-test
	./c-pb selftest
	@echo ALL PASS c-pb

clean:
	rm -f c-pb c-pb-test
