CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-format-truncation -DNDEBUG
.PHONY: all test clean
all: c-toolstore-test c-toolstore
c-toolstore-test: test_toolstore.c toolstore.c toolstore.h
	$(CC) $(CFLAGS) test_toolstore.c toolstore.c -o $@
c-toolstore: c_toolstore_main.c toolstore.c toolstore.h
	$(CC) $(CFLAGS) c_toolstore_main.c toolstore.c -o $@
test: c-toolstore-test c-toolstore
	./c-toolstore-test
	./c-toolstore bounded-path /ws foo/bar | grep -Fxq /ws/foo/bar
	./c-toolstore edit-mode 1 | grep -Fxq dry-ran
	./c-toolstore artifact-ext application/json | grep -Fxq .json
	@echo ALL PASS c-toolstore
clean:
	rm -f c-toolstore-test c-toolstore
