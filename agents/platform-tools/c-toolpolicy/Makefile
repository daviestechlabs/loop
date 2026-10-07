CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-format-truncation -DNDEBUG
.PHONY: all test clean
all: c-toolpolicy-test c-toolpolicy
c-toolpolicy-test: test_toolpolicy.c toolpolicy.c toolpolicy.h
	$(CC) $(CFLAGS) test_toolpolicy.c toolpolicy.c -o $@
c-toolpolicy: c_toolpolicy_main.c toolpolicy.c toolpolicy.h
	$(CC) $(CFLAGS) c_toolpolicy_main.c toolpolicy.c -o $@
test: c-toolpolicy-test c-toolpolicy
	./c-toolpolicy-test
	@echo ALL PASS c-toolpolicy
clean:
	rm -f c-toolpolicy-test c-toolpolicy
