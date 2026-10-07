CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-format-truncation -DNDEBUG
.PHONY: all test clean
all: c-iaevents-test c-iaevents
c-iaevents-test: test_iaevents.c iaevents.c iaevents.h
	$(CC) $(CFLAGS) test_iaevents.c iaevents.c -o $@
c-iaevents: c_iaevents_main.c iaevents.c iaevents.h
	$(CC) $(CFLAGS) c_iaevents_main.c iaevents.c -o $@
test: c-iaevents-test c-iaevents
	./c-iaevents-test
	./c-iaevents subject tool_call | grep -Fq 'analytics.events.interaction.tool_call'
	./c-iaevents validate-envelope e1 tool_call interaction.analytics.events.v1 1
	@echo ALL PASS c-iaevents
clean:
	rm -f c-iaevents-test c-iaevents
