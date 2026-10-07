CC ?= cc
PORTABILITY_WARNINGS := -Wshadow -Wconversion -Wsign-conversion -Wformat=2 -Wcast-align \
	-Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Wstrict-prototypes \
	-Wmissing-prototypes -Wundef -Wvla
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-format-truncation -DNDEBUG $(PORTABILITY_WARNINGS)
CFLAGS += -I. -I../c-dice -I../c-toolstore -I../c-toolpolicy -I../c-dnd -I../c-campaign -D_GNU_SOURCE
JSONDIR = ../../../product/companions-frontend/c-companions
COMMONDIR = ../../../voice/c-runtime/common
WIREDIR = ../../../voice/c-runtime/wire
CFLAGS += -I$(JSONDIR) -I$(COMMONDIR) -I$(WIREDIR)
LDFLAGS ?= -lpthread
STORE_LIBS = -lsqlite3

DICE = ../c-dice/dice.c
TS = ../c-toolstore/toolstore.c
TP = ../c-toolpolicy/toolpolicy.c
DND = ../c-dnd/dnd_initiative.c
CAMP = ../c-campaign/dnd_campaign.c

SRCS_COMMON = pt_store.c pt_worker.c pt_manager.c pt_artifact.c pt_pb.c pt_dnd_state.c pt_encounter.c pt_campaign.c \
	$(DICE) $(TS) $(TP) $(DND) $(CAMP) $(JSONDIR)/cmp_json.c $(COMMONDIR)/utf8.c $(WIREDIR)/pb_min.c
SRCS_HTTP = pt_http.c $(JSONDIR)/cmp_http_headers.c $(COMMONDIR)/voice_auth.c
SRCS_ENCOUNTER_TEST = $(DND) $(CAMP) pt_campaign.c $(DICE) $(TS) $(JSONDIR)/cmp_json.c $(COMMONDIR)/utf8.c $(WIREDIR)/pb_min.c
SRCS_CAMPAIGN_TEST = $(CAMP) $(TS) $(JSONDIR)/cmp_json.c $(COMMONDIR)/utf8.c
SRCS_STATE_TEST = pt_store.c pt_artifact.c pt_encounter.c pt_campaign.c $(DND) $(CAMP) $(DICE) $(TS) $(JSONDIR)/cmp_json.c $(COMMONDIR)/utf8.c $(WIREDIR)/pb_min.c

.PHONY: all test clean sanitize thread-sanitize fuzz-smoke

all: c-ptools c-ptools-test c-ptools-store-test

c-ptools: main.c $(SRCS_HTTP) $(SRCS_COMMON)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(STORE_LIBS) -lcrypto -o $@

c-ptools-test: test_ptools.c $(SRCS_COMMON)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(STORE_LIBS) -lcrypto -o $@

c-ptools-store-test: test_pt_store.c pt_store.c $(TS) $(JSONDIR)/cmp_json.c $(COMMONDIR)/utf8.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(STORE_LIBS) -o $@

c-ptools-encounter-fixture: encounter_fixture.c state_fixture.h $(SRCS_COMMON)
	$(CC) $(CFLAGS) encounter_fixture.c $(filter-out pt_encounter.c,$(SRCS_COMMON)) \
		$(LDFLAGS) $(STORE_LIBS) -lcrypto -Wl,--wrap=fsync -Wl,--wrap=rename -Wl,--wrap=unlink -Wl,--wrap=getrandom -o $@

c-ptools-campaign-fixture: campaign_fixture.c state_fixture.h $(SRCS_COMMON)
	$(CC) $(CFLAGS) campaign_fixture.c $(filter-out pt_campaign.c,$(SRCS_COMMON)) \
		$(LDFLAGS) $(STORE_LIBS) -lcrypto -Wl,--wrap=fsync -Wl,--wrap=rename -Wl,--wrap=unlink -Wl,--wrap=getrandom -o $@

# The production I/O-free campaign adapter also runs on macOS.
c-ptools-campaign-contract: campaign_contract_fixture.c pt_campaign.c $(SRCS_CAMPAIGN_TEST)
	$(CC) $(filter-out -Wno-format-truncation,$(CFLAGS)) $^ -o $@

.PHONY: test-scene-observations
test-scene-observations: c-ptools-campaign-contract
	python3 test_scene_observations.py

test: test-scene-observations

test: c-ptools-test c-ptools-store-test c-ptools c-ptools-encounter-fixture c-ptools-campaign-fixture
	./c-ptools-test
	PT_STORE_HISTORY_TEST_CALLS=10000 ./c-ptools-store-test
	@if strings c-ptools 2>/dev/null | grep -E '^CONNECT \{|nats://' >/dev/null; then \
	  echo "FAIL NATS protocol strings in c-ptools"; exit 1; \
	fi
	python3 test_http.py
	python3 test_encounter.py
	python3 test_encounter_recovery.py
	python3 test_campaign.py
	python3 test_state_recovery.py
	python3 test_state_authority.py
	python3 test_roster.py
	python3 test_scene_state.py
	python3 test_initiative.py
	@echo ALL PASS c-ptools host

sanitize:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) -B CC='clang -fsanitize=address,undefined -fno-omit-frame-pointer' \
		LDFLAGS='-lpthread -fsanitize=address,undefined' test

thread-sanitize:
	TSAN_OPTIONS=halt_on_error=1:exitcode=66 \
		$(MAKE) -B CC='gcc -fsanitize=thread -fno-omit-frame-pointer' \
		LDFLAGS='-lpthread -fsanitize=thread' test

fuzz-pt-store: fuzz_pt_store.c pt_store.c $(TS) $(JSONDIR)/cmp_json.c $(COMMONDIR)/utf8.c
	clang $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer $^ $(STORE_LIBS) -o $@

fuzz-pt-encounter: fuzz_pt_encounter.c pt_encounter.c $(SRCS_ENCOUNTER_TEST)
	clang $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		fuzz_pt_encounter.c $(SRCS_ENCOUNTER_TEST) -lcrypto -o $@

fuzz-pt-campaign: fuzz_pt_campaign.c pt_campaign.c $(SRCS_CAMPAIGN_TEST)
	clang $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		fuzz_pt_campaign.c $(SRCS_CAMPAIGN_TEST) -o $@

fuzz-pt-state: fuzz_pt_state.c pt_dnd_state.c $(SRCS_STATE_TEST)
	clang $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		fuzz_pt_state.c $(SRCS_STATE_TEST) -lcrypto $(STORE_LIBS) -o $@

fuzz-pt-http: fuzz_pt_http.c $(SRCS_HTTP) $(SRCS_COMMON)
	clang $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		fuzz_pt_http.c $(filter-out pt_http.c,$(SRCS_HTTP)) $(SRCS_COMMON) -lpthread -lcrypto $(STORE_LIBS) -o $@

fuzz-smoke: fuzz-pt-store fuzz-pt-encounter fuzz-pt-campaign fuzz-pt-state fuzz-pt-http
	@set -eu; work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT; \
		cp fuzz-corpus/store/* "$$work/"; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./fuzz-pt-store -runs=20000 "$$work"
	@set -eu; work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT; \
		cp fuzz-corpus/encounter/* "$$work/"; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./fuzz-pt-encounter -runs=20000 "$$work"
	@set -eu; work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT; \
		cp fuzz-corpus/campaign/* "$$work/"; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./fuzz-pt-campaign -runs=20000 "$$work"
	@set -eu; work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT; \
		cp fuzz-corpus/state/* "$$work/"; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./fuzz-pt-state -runs=20000 "$$work"
	@set -eu; work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT; \
		cp fuzz-corpus/http/* "$$work/"; \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./fuzz-pt-http -runs=20000 "$$work"

clean:
	rm -f c-ptools-campaign-contract
	rm -f c-ptools c-ptools-test c-ptools-store-test c-ptools-encounter-fixture c-ptools-campaign-fixture fuzz-pt-store fuzz-pt-encounter fuzz-pt-campaign fuzz-pt-state fuzz-pt-http
