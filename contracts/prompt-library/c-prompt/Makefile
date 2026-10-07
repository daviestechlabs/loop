CC ?= cc
PORTABILITY_WARNINGS := -Wshadow -Wconversion -Wsign-conversion -Wformat=2 -Wcast-align \
	-Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Wstrict-prototypes \
	-Wmissing-prototypes -Wundef -Wvla
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-format-truncation -DNDEBUG \
	$(PORTABILITY_WARNINGS)
.PHONY: all test sanitize valgrind clean
all: c-prompt-test c-prompt
c-prompt-test: test_prompt.c prompt.c prompt.h
	$(CC) $(CFLAGS) test_prompt.c prompt.c -o $@
c-prompt: c_prompt_main.c prompt.c prompt.h
	$(CC) $(CFLAGS) c_prompt_main.c prompt.c -o $@
test: c-prompt-test c-prompt
	./c-prompt-test
	@./c-prompt family rag.foo | grep -Fxq rag
	@./c-prompt load-text rag.context_fallback_assistant | grep -q provided
	@echo ALL PASS c-prompt
sanitize:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) -B CC='clang -fsanitize=address,undefined -fno-omit-frame-pointer' test
valgrind: test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all \
		./c-prompt-test
clean:
	rm -f c-prompt-test c-prompt
