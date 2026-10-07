# Pure-C D&D initiative kernel (no Go, no cgo).
CC     ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -DNDEBUG

.PHONY: all test clean

all: c-dnd-test c-dnd

c-dnd-test: test_dnd.c dnd_initiative.c dnd_initiative.h
	$(CC) $(CFLAGS) test_dnd.c dnd_initiative.c -o $@

c-dnd: c_dnd_main.c dnd_initiative.c dnd_initiative.h
	$(CC) $(CFLAGS) c_dnd_main.c dnd_initiative.c -o $@

test: c-dnd-test c-dnd
	./c-dnd-test
	printf '%s\n' \
	  'start campaign-1 ambush-1 user-1' \
	  'add goblin-1 Goblin 12 15 15' \
	  'add rogue-1 Rogue 18 20 20' \
	  'advance' \
	  'advance' \
	  'damage goblin-1 7' \
	  'condition_add goblin-1 prone' \
	  'summary' \
	  'dump' \
	  'end' \
	  | ./c-dnd | tee /tmp/c-dnd-cli.out
	grep -q 'OK start' /tmp/c-dnd-cli.out
	grep -q 'OK advance' /tmp/c-dnd-cli.out
	grep -q 'OK end' /tmp/c-dnd-cli.out
	@echo ALL PASS c-dnd including CLI

clean:
	rm -f c-dnd-test c-dnd
