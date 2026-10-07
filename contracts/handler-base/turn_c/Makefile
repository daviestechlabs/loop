CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-format-truncation -DNDEBUG
SRCS = turn_frame.c turn_ndjson.c turn_budget.c turn_error.c
.PHONY: all test clean
all: c-turnstream c-turnstream-test
c-turnstream: c_turnstream_main.c $(SRCS) turn_frame.h turn_ndjson.h turn_budget.h turn_error.h
	$(CC) $(CFLAGS) c_turnstream_main.c $(SRCS) -o $@
c-turnstream-test: test_turnstream.c $(SRCS) turn_frame.h turn_ndjson.h turn_budget.h turn_error.h
	$(CC) $(CFLAGS) test_turnstream.c $(SRCS) -o $@
test: c-turnstream-test c-turnstream
	./c-turnstream-test
	@./c-turnstream event-name 10 | grep -Fxq completed
	@./c-turnstream event-type-id completed | grep -Fxq 10
	@./c-turnstream is-terminal 10; test $$? -eq 0
	@./c-turnstream is-terminal 4; test $$? -eq 1
	@./c-turnstream agent-event-name 5 | grep -Fxq agent_task_completed
	@./c-turnstream accepted r1 subj 0 1234 | grep -q '"type":"accepted"'
	@./c-turnstream budget-extract 100000 turn_budget_ms 500 | grep -Fq 'budget_ms=500'
	@./c-turnstream err-classify-msg boom | grep -Fq 'internal'
	@echo ALL PASS c-turnstream
clean:
	rm -f c-turnstream c-turnstream-test
