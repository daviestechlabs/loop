CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-format-truncation -DNDEBUG
.PHONY: all test clean
all: c-dice-test c-dice
c-dice-test: test_dice.c dice.c dice.h
	$(CC) $(CFLAGS) test_dice.c dice.c -o $@
c-dice: c_dice_main.c dice.c dice.h
	$(CC) $(CFLAGS) c_dice_main.c dice.c -o $@
test: c-dice-test c-dice
	./c-dice-test
	./c-dice parse '2d20kh1+5' | grep -Fq '"expression":"2d20kh1+5"'
	./c-dice parse '101d6'; test $$? -eq 1
	@echo ALL PASS c-dice
clean:
	rm -f c-dice-test c-dice
