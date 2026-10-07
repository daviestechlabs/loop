# Pure-C voice runtime: VBus, six services, and product math kernels.
.DEFAULT_GOAL := all
ROOT   := $(abspath ../..)
DSP    := $(ROOT)/voice/audio-processor/dsp
RCLASS := $(ROOT)/voice/cascade-router/internal/routeclassifier
PHATIC := $(ROOT)/voice/cascade-router/internal/cascade
SYSTEMONE := $(ROOT)/voice/cascade-router/internal/systemone
TELEM  := $(ROOT)/contracts/telemetry-c
REFLEX := $(ROOT)/voice/voice-session-gateway/reflex-c
PCM    := $(ROOT)/voice/tts-module/pcm
SPEECH := $(ROOT)/contracts/handler-base/speechsegment
TURN   := $(ROOT)/contracts/handler-base/turn_c
TOOLS  := $(ROOT)/agents/platform-tools
PRODUCT_JSON := $(ROOT)/product/companions-frontend/c-companions
PRODUCT_BOOKS := $(ROOT)/product/companions-frontend/c-entitlements
PROMPTS := $(ROOT)/contracts/prompt-library
ANALYTICS := $(ROOT)/contracts/interaction-analytics/c-iaevents

CC     ?= cc
RESEARCH_PYTHON ?= python3
AUTH_LDLIBS ?= -lcrypto
VOICE_SOURCE_REVISION ?= $(shell git -C '$(ROOT)' rev-parse --verify HEAD 2>/dev/null || printf unknown)
PORTABILITY_WARNINGS := -Wshadow -Wconversion -Wsign-conversion -Wformat=2 -Wcast-align \
	-Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Wstrict-prototypes \
	-Wmissing-prototypes -Wundef -Wvla
CFLAGS ?= -std=c11 -O3 -Wall -Wextra -Werror -DNDEBUG $(PORTABILITY_WARNINGS)
# Runtime math validates domains before evaluation and never consumes libm
# errno. Let both GCC and Clang emit the target's exact hardware operations.
override CFLAGS += -fno-math-errno -ffunction-sections -fdata-sections
CPPFLAGS ?=
override CPPFLAGS += -I. -Ibus -Iwire -Icommon -Iservices -I$(DSP) -I$(RCLASS) -I$(PHATIC) -I$(SYSTEMONE) -I$(TELEM)
override CPPFLAGS += -I$(REFLEX) -I$(PCM) -I$(SPEECH) -I$(TURN)
override CPPFLAGS += -I$(TOOLS)/c-dice -I$(TOOLS)/c-toolstore -I$(TOOLS)/c-dnd -I$(PRODUCT_JSON)
override CPPFLAGS += -I$(PRODUCT_BOOKS)
override CPPFLAGS += -I$(ROOT)/contracts/handler-base/c-pb
override CPPFLAGS += -I$(PROMPTS)/c-prompt
override CPPFLAGS += -I$(ANALYTICS)
override CPPFLAGS += -DVOICE_SOURCE_REVISION=\"$(VOICE_SOURCE_REVISION)\"
UNAME_M := $(shell uname -m)
UNAME_S := $(shell uname -s)
LDFLAGS ?= -lm
ifeq ($(UNAME_S),Darwin)
override CPPFLAGS += -D_DARWIN_C_SOURCE
ANALYTICS_ASAN_DETECT_LEAKS := 0
DEAD_STRIP_LDFLAGS := -Wl,-dead_strip
else
ANALYTICS_ASAN_DETECT_LEAKS := 1
DEAD_STRIP_LDFLAGS := -Wl,--gc-sections
endif
override LDFLAGS += $(DEAD_STRIP_LDFLAGS)
OBJ_DIR := .build/release
obj_for = $(OBJ_DIR)/$(if $(filter /%,$(1)),root/$(patsubst $(ROOT)/%,%,$(1:.c=.o)),local/$(1:.c=.o))
# AVX2 RMS is a separate TU so the rest of the binary stays correct
# without -mavx2. Runtime CPUID/XGETBV in pcm_rms.c selects it.
RMS_SRC := $(DSP)/pcm_rms.c
ifeq ($(UNAME_M),x86_64)
AVX2_SRC := $(DSP)/pcm_condition_avx2.c
AVX2_OBJ := $(call obj_for,$(AVX2_SRC))
else
AVX2_SRC :=
AVX2_OBJ :=
endif

COMMON_SRCS := \
	bus/vbus.c \
	wire/pb_min.c \
	common/service.c \
	common/rag_select.c \
	common/http_min.c \
	common/openai_min.c \
	common/pcm_frame.c \
	common/stage_json.c \
	common/utf8.c \
	common/vbus_subject.c
AUTH_SRC := common/voice_auth.c
MODEL_CAPTURE_SRCS := tests/model_request_capture_test.c common/model_request_capture.c $(AUTH_SRC)
.PHONY: test-model-request-capture sanitize-model-request-capture
.build/model-request-capture/test: $(MODEL_CAPTURE_SRCS) common/model_request_capture.h
	@mkdir -p .build/model-request-capture
	$(CC) $(CFLAGS) $(CPPFLAGS) $(MODEL_CAPTURE_SRCS) $(AUTH_LDLIBS) -o $@
test-model-request-capture: .build/model-request-capture/test
	./.build/model-request-capture/test
sanitize-model-request-capture:
	@mkdir -p .build/model-request-capture
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(MODEL_CAPTURE_SRCS) $(AUTH_LDLIBS) -o .build/model-request-capture/sanitize
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/model-request-capture/sanitize
test: test-model-request-capture
sanitize: sanitize-model-request-capture
DND_SRCS := common/dnd_tools.c common/dnd_encounter.c common/dnd_scene.c common/dnd_pending.c common/dnd_dialogue.c $(TOOLS)/c-dnd/dnd_initiative.c $(TOOLS)/c-dice/dice.c $(TOOLS)/c-toolstore/toolstore.c $(PRODUCT_JSON)/cmp_json.c
DND_SCENE_TEST_SRCS := tests/dnd_scene_test.c common/dnd_scene.c common/utf8.c $(PRODUCT_JSON)/cmp_json.c
DND_PENDING_TEST_SRCS := tests/dnd_pending_test.c common/dnd_pending.c common/utf8.c
DND_DIALOGUE_TEST_SRCS := tests/dnd_dialogue_test.c common/dnd_dialogue.c common/utf8.c $(PRODUCT_JSON)/cmp_json.c

.PHONY: test-dnd-dialogue sanitize-dnd-dialogue
.build/dnd-pending/dialogue-test: $(DND_DIALOGUE_TEST_SRCS) common/dnd_dialogue.h common/dnd_pending.h
	@mkdir -p .build/dnd-pending
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DND_DIALOGUE_TEST_SRCS) -o $@
test-dnd-dialogue: .build/dnd-pending/dialogue-test
	./.build/dnd-pending/dialogue-test
test: test-dnd-dialogue
sanitize: sanitize-dnd-dialogue
sanitize-dnd-dialogue:
	@mkdir -p .build/dnd-pending
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(DND_DIALOGUE_TEST_SRCS) -o .build/dnd-pending/dialogue-sanitize
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-pending/dialogue-sanitize

.PHONY: test-dnd-pending test-dnd-pending-mutations sanitize-dnd-pending fuzz-dnd-pending
.build/dnd-pending/test: $(DND_PENDING_TEST_SRCS) common/dnd_pending.h common/utf8.h
	@mkdir -p .build/dnd-pending
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DND_PENDING_TEST_SRCS) -o $@
test-dnd-pending: .build/dnd-pending/test
	./.build/dnd-pending/test
test: test-dnd-pending
test: test-dnd-pending-mutations
test-dnd-pending-mutations:
	python3 tests/test_dnd_pending_mutations.py
sanitize: sanitize-dnd-pending
sanitize-dnd-pending:
	@mkdir -p .build/dnd-pending
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(DND_PENDING_TEST_SRCS) -o .build/dnd-pending/sanitize
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-pending/sanitize
fuzz: fuzz-dnd-pending
fuzz-dnd-pending:
	@mkdir -p .build/dnd-pending
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		tests/dnd_pending_fuzz.c common/dnd_pending.c common/utf8.c -o .build/dnd-pending/fuzz
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		.build/dnd-pending/fuzz -runs=20000 -max_len=512 -print_funcs=0

.PHONY: test-dnd-scene test-dnd-scene-contract clean-dnd-scene
c-dnd-scene-test: $(DND_SCENE_TEST_SRCS) common/dnd_tools.h common/utf8.h wire/pb_min.h $(ROOT)/contracts/handler-base/c-pb/pb_dnd_campaign.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DND_SCENE_TEST_SRCS) -o $@
clean: clean-dnd-scene
clean-dnd-scene:
	rm -f c-dnd-scene-test
test-dnd-scene: c-dnd-scene-test
	./c-dnd-scene-test
test-dnd-scene-contract: test-dnd-scene
	$(MAKE) -C $(TOOLS)/c-ptools c-ptools-campaign-contract
	python3 tests/test_dnd_scene_contract.py
test: test-dnd-scene
test: test-dnd-scene-contract
.PHONY: fuzz-dnd-scene sanitize-dnd-scene
fuzz: fuzz-dnd-scene
fuzz-dnd-scene:
	@mkdir -p .build/dnd-scene
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		tests/dnd_scene_fuzz.c $(filter-out tests/dnd_scene_test.c,$(DND_SCENE_TEST_SRCS)) -o .build/dnd-scene/fuzz
	@set -eu; corpus=$$(mktemp -d /tmp/c-dnd-scene.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/dnd-scene/* "$$corpus/"; \
		ASAN_OPTIONS=$${FUZZ_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} UBSAN_OPTIONS=halt_on_error=1 \
		.build/dnd-scene/fuzz -runs=20000 -max_len=8192 -print_funcs=0 "$$corpus"
sanitize: sanitize-dnd-scene
sanitize-dnd-scene:
	@mkdir -p .build/dnd-scene
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(DND_SCENE_TEST_SRCS) -o .build/dnd-scene/sanitize
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-scene/sanitize
	$(MAKE) -C $(TOOLS)/c-ptools c-ptools-campaign-contract
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		DND_SCENE_BIN=$(abspath .build/dnd-scene/sanitize) python3 tests/test_dnd_scene_contract.py
DND_RAG_SRCS := common/dnd_retrieval.c common/dnd_source_index.c common/dnd_source_artifact.c common/dnd_source_passage.c common/dnd_source_page.c wire/pb_min.c common/openai_min.c $(PRODUCT_JSON)/cmp_json.c $(PRODUCT_BOOKS)/ent_books.c common/utf8.c
LOOP_SCENE_SRCS := common/loop_scene.c
STT_VOCABULARY_SRCS := common/stt_vocabulary.c $(LOOP_SCENE_SRCS) common/dnd_scene.c $(PRODUCT_JSON)/cmp_json.c
.PHONY: test-stt-vocabulary
test-stt-vocabulary: c-audio-processor c-session-manager vbus-broker
	$(RESEARCH_PYTHON) tests/test_stt_vocabulary_process.py
test: test-stt-vocabulary
GROUNDING_SRCS := common/dnd_grounding.c common/response_style.c common/loop_rules.c $(PROMPTS)/c-prompt/prompt.c

LOOP_CONTEXT_TEST_SRCS := tests/loop_context_test.c wire/pb_min.c common/openai_min.c common/utf8.c common/dnd_scene.c $(LOOP_SCENE_SRCS) common/stt_vocabulary.c $(GROUNDING_SRCS) $(PRODUCT_JSON)/cmp_json.c
.PHONY: test-loop-context sanitize-loop-context
.build/loop-context/test: $(LOOP_CONTEXT_TEST_SRCS) common/openai_min.h common/response_style.h common/loop_scene.h wire/pb_min.h
	@mkdir -p .build/loop-context
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LOOP_CONTEXT_TEST_SRCS) -o $@
test-loop-context: .build/loop-context/test
	./.build/loop-context/test
test: test-loop-context
sanitize-loop-context:
	@mkdir -p .build/loop-context
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(LOOP_CONTEXT_TEST_SRCS) -o .build/loop-context/sanitize
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/loop-context/sanitize
sanitize: sanitize-loop-context
DND_RAG_TEST_SRCS := $(DND_RAG_SRCS) $(GROUNDING_SRCS) common/turn_response.c common/turn_response_json.c common/stage_json.c common/base64.c
WT_CORE_SRCS := services/gateway_webtransport_core.c wire/pb_min.c common/utf8.c \
	$(PRODUCT_JSON)/cmp_json.c $(PRODUCT_JSON)/cmp_turn_metadata.c
RESPONSE_SRC := common/turn_response.c
HTTP_JSON_SRC := common/turn_response_json.c
BASE64_SRC := common/base64.c
RUNTIME_IDENTITY_SRC := common/runtime_identity.c
ANALYTICS_SRCS := common/product_analytics.c $(ANALYTICS)/iaevents.c $(ANALYTICS)/ia_transport.c $(ROOT)/contracts/handler-base/c-pb/pb_wire.c
# dynbuf lives under KERNEL_SRCS ($(DSP)/dynbuf.c) — single object, not dual-linked.

KERNEL_SRCS := \
	$(DSP)/dynbuf.c \
	$(DSP)/pcm_condition.c \
	$(DSP)/audio_engine.c \
	$(DSP)/transcript_filler.c \
	$(RCLASS)/route_classifier.c \
	$(PHATIC)/phatic_policy.c \
	$(SYSTEMONE)/systemone.c \
	$(TELEM)/telemetry.c \
	$(REFLEX)/reflex_session.c \
	$(PCM)/pcm_parse.c \
	$(SPEECH)/speech_sanitize.c \
	$(SPEECH)/speech_markup.c \
	$(SPEECH)/speech_segment.c \
	$(SPEECH)/speech_seg_host.c \
	$(SPEECH)/speech_display_stream.c \
	$(TURN)/turn_budget.c \
	$(TURN)/turn_error.c \
	$(TURN)/turn_frame.c \
	$(TURN)/turn_ndjson.c

SERVICES := \
	c-audio-processor \
	c-cascade-router \
	c-voice-session-gateway \
	c-session-manager \
	c-tts-module \
	c-rag-gateway
ROLLBACK_SERVICES := c-conversation-orchestrator

.PHONY: all test bench webtransport check-webtransport check-webtransport-browser check-webtransport-zero-audit fuzz fuzz-gateway-http-fields fuzz-webtransport fuzz-openai-sse fuzz-tts-segment sanitize thread-sanitize valgrind check-linked-surface check-no-nats check-no-go clean

all: $(SERVICES) vbus-broker c-runtime-test c-bus-test c-dynbuf-test c-http-test c-auth-test c-dnd-tools-test c-webtransport-core-test c-pb-interop c-service-e2e c-vbus-tx-bench c-vbus-process-latency-bench c-vbus-wildcard-bench c-vbus-queue-bench c-vbus-storage-bench c-vbus-rx-window-bench c-deadline-index-bench c-session-index-bench c-worker-queue-bench c-nonce-cache-bench c-webtransport-queue-bench c-http-queue-bench c-base64-bench c-stage-json-bench c-response-json-bench c-turn-decode-bench c-openai-request-bench c-tts-request-bench c-speech-segment-bench

SERVICE_SRCS := \
	services/audio_processor_main.c \
	services/cascade_router_main.c \
	services/conversation_orchestrator_main.c \
	services/voice_session_gateway_main.c \
	services/session_manager_main.c \
	services/tts_module_main.c \
	services/rag_gateway_main.c \
	services/gateway_http.c \
	services/gateway_webtransport_core.c
TEST_SRCS := tests/runtime_test.c tests/bus_test.c tests/dynbuf_test.c tests/http_test.c tests/auth_test.c tests/webtransport_core_test.c tests/pb_interop_fixture.c tests/service_e2e.c
OTHER_SRCS := bus/vbus_broker_main.c tests/webtransport_e2e_peer.c tests/bench_vbus_tx.c tests/bench_vbus_process_latency.c tests/bench_vbus_wildcards.c tests/bench_vbus_queue_groups.c tests/bench_vbus_storage.c tests/bench_vbus_rx_windows.c tests/bench_deadline_indexes.c tests/bench_session_indexes.c tests/bench_worker_queues.c tests/bench_nonce_cache.c tests/bench_webtransport_queues.c tests/bench_http_queues.c tests/bench_base64.c tests/bench_stage_json.c tests/bench_response_json.c tests/bench_turn_decode.c tests/bench_openai_request.c tests/bench_tts_request.c tests/bench_speech_segment.c
ALL_OBJECT_SRCS := $(sort $(COMMON_SRCS) common/model_request_capture.c $(AUTH_SRC) $(DND_SRCS) tests/dnd_tools_test.c $(RESPONSE_SRC) $(HTTP_JSON_SRC) $(BASE64_SRC) $(RUNTIME_IDENTITY_SRC) $(RMS_SRC) $(KERNEL_SRCS) $(SERVICE_SRCS) $(TEST_SRCS) $(OTHER_SRCS))
ALL_OBJECT_SRCS += $(DND_RAG_SRCS) tests/dnd_retrieval_test.c
ALL_OBJECT_SRCS := $(sort $(ALL_OBJECT_SRCS) $(WT_CORE_SRCS) $(GROUNDING_SRCS) $(LOOP_SCENE_SRCS) $(STT_VOCABULARY_SRCS))
ALL_OBJECT_SRCS := $(sort $(ALL_OBJECT_SRCS) $(ANALYTICS_SRCS) tests/product_analytics_test.c)
ALL_OBJECTS := $(foreach src,$(ALL_OBJECT_SRCS),$(call obj_for,$(src)))

all: c-dnd-retrieval-test
ANALYTICS_TEST_SRCS := tests/product_analytics_test.c $(ANALYTICS_SRCS) common/http_min.c common/service.c common/utf8.c bus/vbus.c $(PRODUCT_JSON)/cmp_json.c $(DSP)/dynbuf.c wire/pb_min.c services/gateway_http.c common/voice_auth.c common/turn_response.c common/turn_response_json.c common/stage_json.c common/base64.c common/runtime_identity.c
.PHONY: test-product-analytics sanitize-product-analytics thread-sanitize-product-analytics clean-product-analytics
all: c-product-analytics-test
c-product-analytics-test: $(foreach src,$(ANALYTICS_TEST_SRCS),$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@
test: test-product-analytics
test-product-analytics: c-product-analytics-test
	./c-product-analytics-test
sanitize: sanitize-product-analytics
sanitize-product-analytics:
	@mkdir -p .build/product-analytics-sanitize
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(ANALYTICS_TEST_SRCS) $(LDFLAGS) -lpthread $(AUTH_LDLIBS) -o .build/product-analytics-sanitize/test
	ASAN_OPTIONS=detect_leaks=$(ANALYTICS_ASAN_DETECT_LEAKS):halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/product-analytics-sanitize/test
thread-sanitize: thread-sanitize-product-analytics
thread-sanitize-product-analytics:
	@mkdir -p .build/product-analytics-sanitize
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
		$(ANALYTICS_TEST_SRCS) $(LDFLAGS) -lpthread $(AUTH_LDLIBS) -o .build/product-analytics-sanitize/thread-test
	TSAN_OPTIONS=halt_on_error=1:exitcode=66 .build/product-analytics-sanitize/thread-test
clean: clean-product-analytics
clean-product-analytics:
	rm -f c-product-analytics-test c-product-microphone-peer c-loop-recorded-peer
c-dnd-retrieval-test: $(foreach src,tests/dnd_retrieval_test.c $(DND_RAG_TEST_SRCS),$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

test: test-dnd-retrieval
.PHONY: test-dnd-retrieval
test-dnd-retrieval: c-dnd-retrieval-test
	./c-dnd-retrieval-test

test: test-retrieval-spans
.PHONY: test-retrieval-spans
test-retrieval-spans: c-pb-interop
	python3 tests/test_retrieval_spans.py ./c-pb-interop

.PHONY: test-speech-markup sanitize-speech-markup fuzz-speech-markup
.PHONY: bench-speech-markup
bench-speech-markup: .build/speech-markup/bench
	.build/speech-markup/bench

.build/speech-markup/bench: tests/bench_speech_markup.c $(SPEECH)/speech_markup.c $(SPEECH)/speech_markup.h
	@mkdir -p .build/speech-markup
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/bench_speech_markup.c $(SPEECH)/speech_markup.c -o $@

test: test-speech-markup
test-speech-markup: .build/speech-markup/test
	.build/speech-markup/test

.build/speech-markup/test: $(SPEECH)/speech_markup_test.c $(SPEECH)/speech_markup.c $(SPEECH)/speech_markup.h
	@mkdir -p .build/speech-markup
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SPEECH)/speech_markup_test.c $(SPEECH)/speech_markup.c -o $@

sanitize: sanitize-speech-markup
sanitize-speech-markup:
	@mkdir -p .build/speech-markup
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $(SPEECH)/speech_markup_test.c $(SPEECH)/speech_markup.c -o .build/speech-markup/sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/speech-markup/sanitize

fuzz: fuzz-speech-markup
fuzz-speech-markup:
	@mkdir -p .build/speech-markup
	clang $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=fuzzer,address,undefined \
		$(SPEECH)/speech_markup_fuzz.c $(SPEECH)/speech_markup.c -o .build/speech-markup/fuzz
	@set -eu; corpus=$$(mktemp -d /tmp/c-speech-markup-fuzz.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/speech-markup/* "$$corpus/"; \
		ASAN_OPTIONS="$${FUZZ_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 \
		.build/speech-markup/fuzz -runs=$${FUZZ_RUNS:-20000} -max_len=2047 -print_funcs=0 "$$corpus"

.PHONY: fuzz-speech-segment
fuzz: fuzz-speech-segment
fuzz-speech-segment:
	@mkdir -p .build/speech-segment
	clang $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=fuzzer,address,undefined \
		tests/speech_segment_fuzz.c $(SPEECH)/speech_segment.c -o .build/speech-segment/fuzz
	@set -eu; corpus=$$(mktemp -d /tmp/c-speech-segment-fuzz.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/speech-segment/* "$$corpus/"; \
		ASAN_OPTIONS="$${FUZZ_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 \
		.build/speech-segment/fuzz -runs=$${FUZZ_RUNS:-20000} -max_len=1024 -print_funcs=0 "$$corpus"

DND_SOURCE_INDEX_SRCS := common/dnd_source_index.c common/utf8.c $(PRODUCT_BOOKS)/ent_books.c
DND_SOURCE_INDEX_HEADERS := common/dnd_source_select_internal.h common/dnd_source_index.h common/dnd_source_spell.h common/dnd_retrieval.h common/dnd_retrieval_types.h common/utf8.h $(PRODUCT_BOOKS)/ent_books.h $(PRODUCT_JSON)/cmp_json.h
DND_SOURCE_PAGE_SRCS := common/dnd_source_page.c common/utf8.c
DND_SOURCE_PAGE_HEADERS := common/dnd_source_page.h $(DND_SOURCE_INDEX_HEADERS)

.PHONY: test-dnd-source-page sanitize-dnd-source-page fuzz-dnd-source-page
test: test-dnd-source-page
test-dnd-source-page: .build/dnd-source-page/test
	.build/dnd-source-page/test
	python3 tests/test_dnd_source_page.py

.build/dnd-source-page/test: tests/dnd_source_page_test.c $(DND_SOURCE_PAGE_HEADERS) $(DND_SOURCE_PAGE_SRCS)
	@mkdir -p .build/dnd-source-page
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/dnd_source_page_test.c $(DND_SOURCE_PAGE_SRCS) -o $@

sanitize: sanitize-dnd-source-page
sanitize-dnd-source-page:
	@mkdir -p .build/dnd-source-page
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/dnd_source_page_test.c $(DND_SOURCE_PAGE_SRCS) -o .build/dnd-source-page/sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-source-page/sanitize

fuzz: fuzz-dnd-source-page
fuzz-dnd-source-page:
	@mkdir -p .build/dnd-source-page
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		tests/dnd_source_page_fuzz.c $(DND_SOURCE_PAGE_SRCS) -o .build/dnd-source-page/fuzz
	@set -eu; corpus=$$(mktemp -d /tmp/c-dnd-source-page-fuzz.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/dnd-source-page/* "$$corpus/"; \
		ASAN_OPTIONS="$${FUZZ_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 \
		.build/dnd-source-page/fuzz -runs=$${FUZZ_RUNS:-20000} -max_len=65536 -print_funcs=0 "$$corpus"

DND_SOURCE_ARTIFACT_SRCS := common/dnd_source_passage.c common/dnd_source_page.c common/dnd_source_artifact.c $(PRODUCT_JSON)/cmp_json.c $(DND_SOURCE_INDEX_SRCS)
DND_SOURCE_ARTIFACT_HEADERS := common/dnd_source_passage.h $(DND_SOURCE_PAGE_HEADERS) common/dnd_source_artifact.h tests/dnd_source_artifact_fixture.h tests/dnd_source_fixture.h $(DND_SOURCE_INDEX_HEADERS)
DND_SOURCE_COMPILER_SRCS := common/dnd_source_compile.c common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_SRCS)
DND_SOURCE_COMPILER_HEADERS := common/dnd_source_compile.h common/dnd_source_encode.h $(DND_SOURCE_PAGE_HEADERS) $(DND_SOURCE_ARTIFACT_HEADERS)

.PHONY: test-dnd-source-sections sanitize-dnd-source-sections
test: test-dnd-source-sections
test-dnd-source-sections: .build/dnd-source-compiler/section-test
	.build/dnd-source-compiler/section-test

.build/dnd-source-compiler/section-test: tests/dnd_source_section_test.c $(DND_SOURCE_COMPILER_HEADERS) $(DND_SOURCE_COMPILER_SRCS)
	@mkdir -p .build/dnd-source-compiler
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/dnd_source_section_test.c $(DND_SOURCE_COMPILER_SRCS) $(AUTH_LDLIBS) -o $@

sanitize: sanitize-dnd-source-sections
sanitize-dnd-source-sections:
	@mkdir -p .build/dnd-source-compiler
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tests/dnd_source_section_test.c $(DND_SOURCE_COMPILER_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-compiler/section-sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-source-compiler/section-sanitize

.PHONY: dnd-source-compiler test-dnd-source-compiler sanitize-dnd-source-compiler fuzz-dnd-source-compiler
dnd-source-compiler: .build/dnd-source-compiler/compiler
.build/dnd-source-compiler/compiler: tools/dnd_source_compiler.c $(DND_SOURCE_COMPILER_HEADERS) $(DND_SOURCE_COMPILER_SRCS)
	@mkdir -p .build/dnd-source-compiler
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/dnd_source_compiler.c $(DND_SOURCE_COMPILER_SRCS) $(AUTH_LDLIBS) -o $@

test: test-dnd-source-compiler
test-dnd-source-compiler: dnd-source-compiler .build/dnd-source-compiler/encode-test
	.build/dnd-source-compiler/encode-test
	python3 tests/test_dnd_source_compiler.py .build/dnd-source-compiler/compiler

.build/dnd-source-compiler/encode-test: tests/dnd_source_encode_test.c $(DND_SOURCE_COMPILER_HEADERS) $(DND_SOURCE_COMPILER_SRCS)
	@mkdir -p .build/dnd-source-compiler
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/dnd_source_encode_test.c common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o $@

sanitize: sanitize-dnd-source-compiler
sanitize-dnd-source-compiler:
	@mkdir -p .build/dnd-source-compiler
	clang $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined \
		tools/dnd_source_compiler.c $(DND_SOURCE_COMPILER_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-compiler/sanitize
	clang $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined \
		tests/dnd_source_encode_test.c common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-compiler/encode-sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-source-compiler/encode-sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		python3 tests/test_dnd_source_compiler.py .build/dnd-source-compiler/sanitize

fuzz: fuzz-dnd-source-compiler
fuzz-dnd-source-compiler:
	@mkdir -p .build/dnd-source-compiler
	clang $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
		tests/dnd_source_compiler_fuzz.c $(DND_SOURCE_COMPILER_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-compiler/fuzz
	@set -eu; corpus=$$(mktemp -d /tmp/c-dnd-source-compiler-fuzz.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/dnd-source-compiler/* "$$corpus/"; \
		ASAN_OPTIONS="$${FUZZ_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 \
		.build/dnd-source-compiler/fuzz -runs=$${FUZZ_RUNS:-20000} -max_len=7000 -print_funcs=0 "$$corpus"

.PHONY: dnd-source-bundle test-dnd-source-bundle sanitize-dnd-source-bundle
dnd-source-bundle: .build/dnd-source-bundle/bundle
.build/dnd-source-bundle/bundle: tools/dnd_source_bundle.c $(DND_SOURCE_ARTIFACT_HEADERS) $(DND_SOURCE_ARTIFACT_SRCS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/dnd_source_bundle.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o $@

test: test-dnd-source-bundle
test-dnd-source-bundle: dnd-source-bundle dnd-source-compiler
	python3 tests/test_dnd_source_bundle.py .build/dnd-source-bundle/bundle .build/dnd-source-compiler/compiler

sanitize: sanitize-dnd-source-bundle
sanitize-dnd-source-bundle: dnd-source-compiler
	@mkdir -p .build/dnd-source-bundle
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tools/dnd_source_bundle.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-bundle/sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		python3 tests/test_dnd_source_bundle.py .build/dnd-source-bundle/sanitize .build/dnd-source-compiler/compiler

.PHONY: test-dnd-source-artifact sanitize-dnd-source-artifact fuzz-dnd-source-artifact
test: test-dnd-source-artifact
test-dnd-source-artifact: .build/dnd-source-artifact/test
	$<

.build/dnd-source-artifact/test: tests/dnd_source_artifact_test.c $(DND_SOURCE_ARTIFACT_HEADERS) $(DND_SOURCE_ARTIFACT_SRCS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/dnd_source_artifact_test.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o $@

sanitize: sanitize-dnd-source-artifact
sanitize-dnd-source-artifact:
	@mkdir -p .build/dnd-source-artifact
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/dnd_source_artifact_test.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-artifact/sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-source-artifact/sanitize

fuzz: fuzz-dnd-source-artifact
fuzz-dnd-source-artifact:
	@mkdir -p .build/dnd-source-artifact
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		tests/dnd_source_artifact_fuzz.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-artifact/fuzz
	@set -eu; corpus=$$(mktemp -d /tmp/c-dnd-source-artifact.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/dnd-source-artifact/* "$$corpus/"; \
		ASAN_OPTIONS="$${FUZZ_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 \
		.build/dnd-source-artifact/fuzz -runs=$${FUZZ_RUNS:-20000} -max_len=8192 "$$corpus"

.PHONY: test-dnd-source-index sanitize-dnd-source-index fuzz-dnd-source-index
test: test-dnd-source-index
test-dnd-source-index: .build/dnd-source-index/test
	$<

.build/dnd-source-index/test: tests/dnd_source_index_test.c tests/dnd_source_fixture.h $(DND_SOURCE_INDEX_HEADERS) $(DND_SOURCE_INDEX_SRCS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/dnd_source_index_test.c $(DND_SOURCE_INDEX_SRCS) $(AUTH_LDLIBS) -o $@

sanitize: sanitize-dnd-source-index
sanitize-dnd-source-index:
	@mkdir -p .build/dnd-source-index
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/dnd_source_index_test.c $(DND_SOURCE_INDEX_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-index/sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/dnd-source-index/sanitize

fuzz: fuzz-dnd-source-index
fuzz-dnd-source-index:
	@mkdir -p .build/dnd-source-index
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		tests/dnd_source_index_fuzz.c $(DND_SOURCE_INDEX_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-index/fuzz
	@set -eu; corpus=$$(mktemp -d /tmp/c-dnd-source-index.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/dnd-source-index/* "$$corpus/"; \
		ASAN_OPTIONS="$${FUZZ_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 \
		.build/dnd-source-index/fuzz -runs=$${FUZZ_RUNS:-20000} -max_len=2047 "$$corpus"

.PHONY: test-rag-bm25
test: test-rag-bm25
test-rag-bm25: c-rag-gateway vbus-broker
	python3 tests/test_rag_bm25.py

.PHONY: test-rag-source
test: test-rag-source
test-rag-source: c-rag-gateway vbus-broker .build/dnd-source-artifact/emit
	python3 tests/test_rag_source.py

.build/dnd-source-artifact/emit: tests/dnd_source_artifact_emit.c $(DND_SOURCE_ARTIFACT_HEADERS) $(DND_SOURCE_ARTIFACT_SRCS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/dnd_source_artifact_emit.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o $@

.PHONY: sanitize-dnd-retrieval
sanitize-dnd-retrieval:
	@mkdir -p .build/dnd-retrieval-sanitize
	clang $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/dnd_retrieval_test.c $(DND_RAG_TEST_SRCS) -lm $(AUTH_LDLIBS) -o .build/dnd-retrieval-sanitize/test
	ASAN_OPTIONS=$${RETRIEVAL_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} UBSAN_OPTIONS=halt_on_error=1 \
		.build/dnd-retrieval-sanitize/test

clean: clean-dnd-retrieval
.PHONY: clean-dnd-retrieval
clean-dnd-retrieval:
	rm -f c-dnd-retrieval-test
COMMON_OBJS := $(foreach src,$(COMMON_SRCS),$(call obj_for,$(src)))
KERNEL_OBJS := $(foreach src,$(KERNEL_SRCS),$(call obj_for,$(src)))
RMS_OBJ := $(call obj_for,$(RMS_SRC))
CORE_OBJS := $(COMMON_OBJS) $(KERNEL_OBJS)

$(OBJ_DIR):
	mkdir -p $@

define compile_template
$(call obj_for,$(1)): $(1) | $(OBJ_DIR)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach src,$(ALL_OBJECT_SRCS),$(eval $(call compile_template,$(src))))

ifneq ($(AVX2_SRC),)
$(AVX2_OBJ): $(AVX2_SRC) $(DSP)/pcm_condition.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -mavx2 -c $< -o $@
endif

-include $(ALL_OBJECTS:.o=.d) $(AVX2_OBJ:.o=.d)

vbus-broker: $(call obj_for,bus/vbus_broker_main.c) $(call obj_for,bus/vbus.c) $(call obj_for,$(DSP)/dynbuf.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-audio-processor: $(call obj_for,services/audio_processor_main.c) $(CORE_OBJS) $(foreach src,$(STT_VOCABULARY_SRCS),$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) -lpthread -o $@

c-cascade-router: $(call obj_for,services/cascade_router_main.c) $(call obj_for,common/model_request_capture.c) $(CORE_OBJS) $(foreach src,$(DND_SRCS) $(DND_RAG_SRCS) $(GROUNDING_SRCS) $(LOOP_SCENE_SRCS) $(AUTH_SRC),$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

c-dnd-tools-test: $(foreach src,tests/dnd_tools_test.c $(DND_SRCS) $(AUTH_SRC) common/http_min.c common/utf8.c wire/pb_min.c,$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

all: c-dnd-tools-test

c-conversation-orchestrator: $(call obj_for,services/conversation_orchestrator_main.c) $(CORE_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

c-voice-session-gateway: $(call obj_for,services/voice_session_gateway_main.c) $(call obj_for,services/gateway_http.c) $(call obj_for,$(AUTH_SRC)) $(call obj_for,$(RESPONSE_SRC)) $(call obj_for,$(HTTP_JSON_SRC)) $(call obj_for,$(BASE64_SRC)) $(call obj_for,$(RUNTIME_IDENTITY_SRC)) $(CORE_OBJS) $(foreach src,$(ANALYTICS_SRCS),$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

c-session-manager: $(call obj_for,services/session_manager_main.c) $(CORE_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

c-tts-module: $(call obj_for,services/tts_module_main.c) $(CORE_OBJS)
	$(CC) $^ $(LDFLAGS) -lpthread -o $@

c-rag-gateway: $(call obj_for,services/rag_gateway_main.c) $(CORE_OBJS) $(foreach src,$(DND_RAG_SRCS),$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

# LSQUIC must use the same AWS-LC headers and static libraries that built it.
# The default build stays dependency-light; the image build supplies these paths.
LSQUIC_CPPFLAGS ?=
LSQUIC_LDLIBS ?= -llsquic -lssl -lcrypto -lz -lpthread -lm
WEBTRANSPORT_SRCS := \
	common/model_request_capture.c common/base64.c \
	$(ANALYTICS_SRCS) common/http_min.c \
	services/gateway_webtransport_main.c \
	$(WT_CORE_SRCS) \
	common/turn_response.c \
	common/voice_auth.c \
	common/service.c \
	bus/vbus.c \
	$(DSP)/dynbuf.c \
	$(TURN)/turn_frame.c

c-webtransport-gateway: $(WEBTRANSPORT_SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LSQUIC_CPPFLAGS) $^ $(LDFLAGS) $(LSQUIC_LDLIBS) -o $@

c-webtransport-gateway-zero-audit: $(WEBTRANSPORT_SRCS)
	$(CC) $(CPPFLAGS) -DWT_VERIFY_ZEROED_FREE_SLOTS=1 $(CFLAGS) $(LSQUIC_CPPFLAGS) $^ $(LDFLAGS) $(LSQUIC_LDLIBS) -o $@

webtransport: c-webtransport-gateway

check-webtransport: webtransport vbus-broker
	@if nm -g c-webtransport-gateway 2>/dev/null | grep -E 'runtime\.|cgo\.|go\.' >/dev/null; then \
	  echo "FAIL Go symbols in c-webtransport-gateway"; exit 1; \
	fi
	@if strings c-webtransport-gateway 2>/dev/null | grep -E '^CONNECT \{|nats://' >/dev/null; then \
	  echo "FAIL NATS protocol strings in c-webtransport-gateway"; exit 1; \
	fi
	@if readelf -d c-webtransport-gateway 2>/dev/null | grep -F 'libstdc++.so' >/dev/null; then \
	  echo "FAIL C++ runtime dependency in c-webtransport-gateway"; exit 1; \
	fi
	bash tests/test_webtransport_startup.sh ./c-webtransport-gateway ./vbus-broker
	@echo "OK pure-C WebTransport gateway with zero NATS protocol or C++ runtime"

c-webtransport-e2e-peer: $(call obj_for,tests/webtransport_e2e_peer.c) $(call obj_for,$(AUTH_SRC)) $(foreach src,$(WT_CORE_SRCS),$(call obj_for,$(src))) $(call obj_for,bus/vbus.c) $(call obj_for,$(DSP)/dynbuf.c) $(call obj_for,$(TURN)/turn_frame.c)
	$(CC) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

check-webtransport-browser: webtransport vbus-broker c-webtransport-e2e-peer
	bash tests/test_webtransport_browser.sh ./c-webtransport-gateway ./vbus-broker ./c-webtransport-e2e-peer

c-product-microphone-peer: tests/product_microphone_peer.c wire/pb_min.c common/utf8.c bus/vbus.c $(DSP)/dynbuf.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

c-loop-recorded-peer: tests/loop_recorded_peer.c wire/pb_min.c common/utf8.c common/model_request_capture.c $(AUTH_SRC) bus/vbus.c $(DSP)/dynbuf.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

check-webtransport-zero-audit: c-webtransport-gateway-zero-audit vbus-broker c-webtransport-e2e-peer
	bash tests/test_webtransport_browser.sh ./c-webtransport-gateway-zero-audit ./vbus-broker ./c-webtransport-e2e-peer

FUZZ_DIR := .build/fuzz
FUZZ_CC ?= clang
GW_WT_FUZZ_MAX_LEN ?= 17000
OPENAI_SSE_FUZZ_MAX_LEN ?= 20000
TTS_SEGMENT_FUZZ_MAX_LEN ?= 8192
GATEWAY_HTTP_FIELDS_FUZZ_MAX_LEN ?= 64
HTTP_RESPONSE_HEADER_FUZZ_MAX_LEN ?= 64
FUZZ_ASAN_OPTIONS ?= detect_leaks=1:halt_on_error=1
FUZZ_RUNS ?= 20000
FUZZ_ARTIFACT_ARG = $(if $(FUZZ_ARTIFACT_DIR),-artifact_prefix=$(FUZZ_ARTIFACT_DIR)/,)
FUZZ_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
	-fsanitize=fuzzer,address,undefined

$(FUZZ_DIR):
	mkdir -p $@

.PHONY: fuzz-product-analytics
fuzz: fuzz-product-analytics
$(FUZZ_DIR)/product-analytics-fuzz: tests/product_analytics_fuzz.c $(ANALYTICS_SRCS) common/http_min.c common/service.c common/utf8.c bus/vbus.c $(DSP)/dynbuf.c $(PRODUCT_JSON)/cmp_json.c | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ -lpthread -lcrypto -o $@
fuzz-product-analytics: $(FUZZ_DIR)/product-analytics-fuzz
	@fuzz_corpus="$$(mktemp -d /tmp/c-product-analytics-fuzz.XXXXXXXX)"; \
	trap 'find "$$fuzz_corpus" -depth -delete' EXIT; \
	ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$< "$$fuzz_corpus" -runs=20000 -max_len=4096

$(FUZZ_DIR)/gateway-http-fields-fuzz: tests/gateway_http_fields_fuzz.c services/gateway_http_fields.h services/gateway_http_header_name.h | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $< -o $@

fuzz-gateway-http-fields: $(FUZZ_DIR)/gateway-http-fields-fuzz
	@fuzz_corpus="$$(mktemp -d /tmp/c-gateway-http-fields-fuzz.XXXXXXXX)"; \
	trap 'find "$$fuzz_corpus" -depth -delete' EXIT; \
	ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$< "$$fuzz_corpus" -runs=$(FUZZ_RUNS) $(FUZZ_ARTIFACT_ARG) -max_len=$(GATEWAY_HTTP_FIELDS_FUZZ_MAX_LEN)

$(FUZZ_DIR)/http-response-header-name-fuzz: tests/http_response_header_name_fuzz.c common/http_response_header_name.h | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $< -o $@

fuzz-http-response-header-name: $(FUZZ_DIR)/http-response-header-name-fuzz
	@fuzz_corpus="$$(mktemp -d /tmp/c-http-response-header-fuzz.XXXXXXXX)"; \
	trap 'find "$$fuzz_corpus" -depth -delete' EXIT; \
	ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$< "$$fuzz_corpus" -runs=$(FUZZ_RUNS) $(FUZZ_ARTIFACT_ARG) -max_len=$(HTTP_RESPONSE_HEADER_FUZZ_MAX_LEN)

$(FUZZ_DIR)/webtransport-core-fuzz: tests/webtransport_core_fuzz.c $(WT_CORE_SRCS) common/turn_response.c | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ -o $@

fuzz-webtransport: $(FUZZ_DIR)/webtransport-core-fuzz
	@fuzz_corpus="$$(mktemp -d /tmp/c-webtransport-fuzz.XXXXXXXX)"; \
	trap 'find "$$fuzz_corpus" -depth -delete' EXIT; \
	cp -a tests/fuzz-corpus/webtransport/. "$$fuzz_corpus"/; \
	ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$< "$$fuzz_corpus" -runs=$(FUZZ_RUNS) $(FUZZ_ARTIFACT_ARG) -max_len=$(GW_WT_FUZZ_MAX_LEN)

$(FUZZ_DIR)/openai-sse-fuzz: tests/openai_sse_fuzz.c common/openai_min.c common/utf8.c | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ -o $@

fuzz-openai-sse: $(FUZZ_DIR)/openai-sse-fuzz
	@fuzz_corpus="$$(mktemp -d /tmp/c-openai-sse-fuzz.XXXXXXXX)"; \
	trap 'find "$$fuzz_corpus" -depth -delete' EXIT; \
	cp -a tests/fuzz-corpus/openai-sse/. "$$fuzz_corpus"/; \
	ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$< "$$fuzz_corpus" -runs=$(FUZZ_RUNS) $(FUZZ_ARTIFACT_ARG) -max_len=$(OPENAI_SSE_FUZZ_MAX_LEN)

$(FUZZ_DIR)/tts-segment-fuzz: tests/tts_segment_fuzz.c wire/pb_min.c | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ -lm -o $@

fuzz-tts-segment: $(FUZZ_DIR)/tts-segment-fuzz
	@fuzz_corpus="$$(mktemp -d /tmp/c-tts-segment-fuzz.XXXXXXXX)"; \
	trap 'find "$$fuzz_corpus" -depth -delete' EXIT; \
	ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$< "$$fuzz_corpus" -runs=$(FUZZ_RUNS) $(FUZZ_ARTIFACT_ARG) -max_len=$(TTS_SEGMENT_FUZZ_MAX_LEN)

$(FUZZ_DIR)/dnd-tools-fuzz: tests/dnd_tools_fuzz.c $(DND_SRCS) $(AUTH_SRC) common/http_min.c common/utf8.c wire/pb_min.c | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ $(AUTH_LDLIBS) -o $@

.PHONY: fuzz-dnd-tools
fuzz-dnd-tools: $(FUZZ_DIR)/dnd-tools-fuzz
	@fuzz_corpus="$$(mktemp -d /tmp/c-dnd-tools-fuzz.XXXXXXXX)"; \
	trap 'find "$$fuzz_corpus" -depth -delete' EXIT; \
	cp -a tests/fuzz-corpus/dnd-tools/. "$$fuzz_corpus"/; \
	protoc --proto_path=$(ROOT)/contracts/handler-base/proto \
	  --encode=messages.v1.DndInitiativeRequest messages/v1/messages.proto \
	  < tests/initiative-request.textproto > "$$fuzz_corpus/initiative-request.pb"; \
	protoc --proto_path=$(ROOT)/contracts/handler-base/proto \
	  --encode=messages.v1.DndCampaignRequest messages/v1/messages.proto \
	  < tests/campaign-request.textproto > "$$fuzz_corpus/campaign-request.pb"; \
	protoc --proto_path=$(ROOT)/contracts/handler-base/proto \
	  --encode=messages.v1.DndEncounterAction messages/v1/messages.proto \
	  < tests/encounter-action.textproto > "$$fuzz_corpus/encounter-action.pb"; \
	ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$< "$$fuzz_corpus" -runs=$(FUZZ_RUNS) $(FUZZ_ARTIFACT_ARG) -max_len=131072

fuzz: fuzz-gateway-http-fields fuzz-http-response-header-name fuzz-webtransport fuzz-openai-sse fuzz-tts-segment fuzz-dnd-tools

$(FUZZ_DIR)/dnd-retrieval-fuzz: tests/dnd_retrieval_fuzz.c $(DND_RAG_TEST_SRCS) | $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $^ -lm $(AUTH_LDLIBS) -o $@

.PHONY: fuzz-dnd-retrieval
fuzz-dnd-retrieval: $(FUZZ_DIR)/dnd-retrieval-fuzz c-pb-interop
	@set -eu; corpus=$$(mktemp -d /tmp/c-dnd-retrieval-fuzz.XXXXXXXX); \
		trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/fuzz-corpus/dnd-retrieval/* "$$corpus/"; \
		./c-pb-interop rag-request > "$$corpus/request.pb"; \
		./c-pb-interop rag-response > "$$corpus/response.pb"; \
		./c-pb-interop citation-event > "$$corpus/citation-event.pb"; \
		ASAN_OPTIONS=$(FUZZ_ASAN_OPTIONS) UBSAN_OPTIONS=halt_on_error=1 \
		$< "$$corpus" -runs=$(FUZZ_RUNS) $(FUZZ_ARTIFACT_ARG) -max_len=65535

fuzz: fuzz-dnd-retrieval

RUNTIME_TEST_OBJS := $(foreach src,tests/runtime_test.c wire/pb_min.c common/rag_select.c common/http_min.c common/openai_min.c common/pcm_frame.c common/stage_json.c common/turn_response.c common/turn_response_json.c common/utf8.c common/vbus_subject.c common/base64.c common/runtime_identity.c,$(call obj_for,$(src))) $(KERNEL_OBJS) $(RMS_OBJ) $(AVX2_OBJ)
c-runtime-test: $(RUNTIME_TEST_OBJS)
	$(CC) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

BUS_TEST_OBJS := $(foreach src,tests/bus_test.c bus/vbus.c wire/pb_min.c,$(call obj_for,$(src))) $(KERNEL_OBJS)
c-bus-test: $(BUS_TEST_OBJS)
	$(CC) $^ $(LDFLAGS) -lpthread -o $@

c-dynbuf-test: $(call obj_for,tests/dynbuf_test.c) $(call obj_for,$(DSP)/dynbuf.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-http-test: $(call obj_for,tests/http_test.c) $(call obj_for,common/http_min.c)
	$(CC) $^ $(LDFLAGS) -lpthread -o $@

c-auth-test: $(call obj_for,tests/auth_test.c) $(call obj_for,$(AUTH_SRC))
	$(CC) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

c-webtransport-core-test: $(call obj_for,tests/webtransport_core_test.c) $(foreach src,$(WT_CORE_SRCS),$(call obj_for,$(src))) $(call obj_for,$(RESPONSE_SRC))
	$(CC) $^ $(LDFLAGS) -o $@

c-pb-interop: $(call obj_for,tests/pb_interop_fixture.c) $(call obj_for,wire/pb_min.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-service-e2e: $(call obj_for,tests/service_e2e.c) $(call obj_for,bus/vbus.c) $(call obj_for,wire/pb_min.c) $(call obj_for,$(DSP)/dynbuf.c) $(call obj_for,common/service.c) $(call obj_for,$(AUTH_SRC)) $(foreach src,$(DND_SRCS) common/http_min.c common/utf8.c common/model_request_capture.c,$(call obj_for,$(src))) | $(SERVICES) vbus-broker
	$(CC) $^ $(LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

c-vbus-tx-bench: $(call obj_for,tests/bench_vbus_tx.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-vbus-process-latency-bench: $(call obj_for,tests/bench_vbus_process_latency.c) $(call obj_for,bus/vbus.c) $(call obj_for,$(DSP)/dynbuf.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-vbus-wildcard-bench: $(call obj_for,tests/bench_vbus_wildcards.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-vbus-queue-bench: $(call obj_for,tests/bench_vbus_queue_groups.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-vbus-storage-bench: $(call obj_for,tests/bench_vbus_storage.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-vbus-rx-window-bench: $(call obj_for,tests/bench_vbus_rx_windows.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-deadline-index-bench: $(call obj_for,tests/bench_deadline_indexes.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-session-index-bench: $(call obj_for,tests/bench_session_indexes.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-worker-queue-bench: $(call obj_for,tests/bench_worker_queues.c)
	$(CC) $^ $(LDFLAGS) -lpthread -o $@

c-nonce-cache-bench: $(call obj_for,tests/bench_nonce_cache.c) $(call obj_for,$(AUTH_SRC))
	$(CC) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

c-webtransport-queue-bench: $(call obj_for,tests/bench_webtransport_queues.c) $(foreach src,$(WT_CORE_SRCS),$(call obj_for,$(src)))
	$(CC) $^ $(LDFLAGS) $(AUTH_LDLIBS) -o $@

c-http-queue-bench: $(call obj_for,tests/bench_http_queues.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-base64-bench: $(call obj_for,tests/bench_base64.c) $(call obj_for,$(BASE64_SRC))
	$(CC) $^ $(LDFLAGS) -o $@

c-stage-json-bench: $(call obj_for,tests/bench_stage_json.c) $(call obj_for,common/stage_json.c) $(call obj_for,wire/pb_min.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-response-json-bench: $(call obj_for,tests/bench_response_json.c) $(call obj_for,common/turn_response.c) $(call obj_for,common/turn_response_json.c) $(call obj_for,common/base64.c) $(call obj_for,common/stage_json.c) $(call obj_for,common/utf8.c) $(call obj_for,wire/pb_min.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-turn-decode-bench: $(call obj_for,tests/bench_turn_decode.c) $(call obj_for,wire/pb_min.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-openai-request-bench: $(call obj_for,tests/bench_openai_request.c) $(call obj_for,common/openai_min.c) $(call obj_for,common/utf8.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-tts-request-bench: $(call obj_for,tests/bench_tts_request.c) $(call obj_for,common/openai_min.c) $(call obj_for,common/utf8.c) $(call obj_for,$(SPEECH)/speech_sanitize.c)
	$(CC) $^ $(LDFLAGS) -o $@

c-speech-segment-bench: $(call obj_for,tests/bench_speech_segment.c) $(call obj_for,$(SPEECH)/speech_segment.c) $(call obj_for,$(SPEECH)/speech_seg_host.c)
	$(CC) $^ $(LDFLAGS) -o $@

bench: c-vbus-tx-bench c-vbus-process-latency-bench c-vbus-wildcard-bench c-vbus-queue-bench c-vbus-storage-bench c-vbus-rx-window-bench c-deadline-index-bench c-session-index-bench c-worker-queue-bench c-nonce-cache-bench c-webtransport-queue-bench c-http-queue-bench c-base64-bench c-stage-json-bench c-response-json-bench c-turn-decode-bench c-openai-request-bench c-tts-request-bench c-speech-segment-bench
	./c-vbus-tx-bench
	./c-vbus-process-latency-bench
	./c-vbus-wildcard-bench
	./c-vbus-queue-bench
	./c-vbus-storage-bench
	./c-vbus-rx-window-bench
	./c-deadline-index-bench
	./c-session-index-bench
	./c-worker-queue-bench
	./c-nonce-cache-bench
	./c-webtransport-queue-bench
	./c-http-queue-bench
	./c-base64-bench
	./c-stage-json-bench
	./c-response-json-bench
	./c-turn-decode-bench
	./c-openai-request-bench
	./c-tts-request-bench
	./c-speech-segment-bench

.PHONY: test-dnd-tools test-dnd-tools-oracle tool-host-for-tests sanitize-dnd-tools
tool-host-for-tests:
	$(MAKE) -C $(TOOLS)/c-ptools c-ptools

test-dnd-tools: c-service-e2e tool-host-for-tests
	SERVICE_E2E_DND_TOOLS=1 ./c-service-e2e

.PHONY: test-dnd-citations
test-dnd-citations: c-service-e2e c-voice-session-gateway vbus-broker
	SERVICE_E2E_DND_CITATIONS=1 ./c-service-e2e

test: test-dnd-citations

.PHONY: test-dnd-audio-metadata
test-dnd-audio-metadata: c-service-e2e c-voice-session-gateway vbus-broker
	SERVICE_E2E_DND_AUDIO_METADATA=1 ./c-service-e2e

test: test-dnd-audio-metadata

.PHONY: test-speech-markup-process sanitize-speech-markup-process
test: test-speech-markup-process
test-speech-markup-process: c-service-e2e
	SERVICE_E2E_SPEECH_MARKUP=1 ./c-service-e2e

# Independent protocol oracle; the normal process gate remains native C.
test-dnd-tools-oracle: c-dnd-tools-test tool-host-for-tests
	python3 tests/test_dnd_tools.py
	python3 tests/test_dnd_initiative.py

.PHONY: test-initiative-wire test-dnd-product
test-initiative-wire:
	python3 tests/test_initiative_wire.py

# Full checkout proof. The runtime image does not contain the product host.
test-dnd-product: all tool-host-for-tests .build/dnd-source-artifact/emit
	# Git archives can give the tracked product binary and source identical mtimes.
	$(MAKE) -B -C $(PRODUCT_JSON) c-companions
	python3 tests/test_dnd_product.py
	python3 tests/test_dnd_campaign_product.py
	python3 tests/test_dnd_scene_product.py
	python3 tests/test_dnd_dialogue_product.py
	python3 tests/test_dnd_encounter_product.py
	python3 tests/test_dnd_grounding_product.py

# Public source checks exclude rights-held research datasets and fleet endpoints.
.PHONY: test-source
test-source: $(SERVICES) vbus-broker c-runtime-test c-bus-test c-dynbuf-test c-http-test c-auth-test c-webtransport-core-test c-service-e2e check-linked-surface test-loop-context
	./c-runtime-test
	./c-bus-test
	./c-dynbuf-test
	./c-http-test
	./c-auth-test
	./c-webtransport-core-test
	PROMPT_LIBRARY_ROOT=$(ROOT)/contracts/prompt-library SERVICE_E2E_MODEL_CAPTURE=1 ./c-service-e2e
	SERVICE_E2E_TTS_WAKEUP=1 ./c-service-e2e
	SERVICE_E2E_LOOP_SCENE=1 ./c-service-e2e

test: all check-linked-surface test-dnd-tools
	./c-dnd-tools-test
	./c-runtime-test
	./c-bus-test
	./c-dynbuf-test
	./c-http-test
	./c-auth-test
	./c-webtransport-core-test
	bash tests/test_pb_interop.sh ./c-pb-interop
	bash tests/test_rag_startup.sh ./c-rag-gateway ./vbus-broker
	bash tests/test_cascade_startup.sh ./c-cascade-router ./vbus-broker
	./c-service-e2e
	SERVICE_E2E_MODEL_CAPTURE=1 ./c-service-e2e
	SERVICE_E2E_LOOP_SCENE=1 ./c-service-e2e
	SERVICE_E2E_SEGMENT_BOUND=1 ./c-service-e2e
	SERVICE_E2E_TTS_WAKEUP=1 ./c-service-e2e
	./c-vbus-tx-bench
	./c-vbus-process-latency-bench
	./c-vbus-wildcard-bench --verify
	./c-vbus-queue-bench --verify
	./c-vbus-storage-bench --verify
	./c-vbus-rx-window-bench --verify
	./c-deadline-index-bench --verify
	./c-session-index-bench --verify
	./c-worker-queue-bench --verify
	./c-nonce-cache-bench --verify
	./c-webtransport-queue-bench --verify
	./c-http-queue-bench --verify
	./c-base64-bench --verify
	./c-stage-json-bench --verify
	./c-response-json-bench --verify
	./c-turn-decode-bench --verify
	./c-openai-request-bench --verify
	./c-tts-request-bench --verify
	./c-speech-segment-bench --verify

check-linked-surface: $(SERVICES)
	@for bin in $(SERVICES); do \
	  if nm -g "$$bin" 2>/dev/null | grep -E 'pcm16_rms|pcm16_host_has_avx2|pcm16_to_f32_avx2' >/dev/null; then \
	    echo "FAIL redundant PCM scan linked into $$bin"; exit 1; \
	  fi; \
	done
	@if nm -g c-tts-module 2>/dev/null | grep -E 'audio_engine_create|route_classifier_packed_v1|reflex_session_init|telemetry_ring_new' >/dev/null; then \
	  echo "FAIL unrelated kernel linked into c-tts-module"; exit 1; \
	fi
	@if nm -g c-cascade-router 2>/dev/null | grep -E 'audio_engine_create|reflex_session_init|pcm_parse_header_v1|telemetry_ring_new' >/dev/null; then \
	  echo "FAIL unrelated kernel linked into c-cascade-router"; exit 1; \
	fi
	@echo "OK release services exclude unreachable shared kernels and redundant PCM scans"

SAN_DIR := .build/sanitize
SAN_CC ?= clang
SAN_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined
SAN_LDFLAGS := -fsanitize=address,undefined -lm
SAN_AVX2_OBJ := $(if $(AVX2_SRC),$(SAN_DIR)/pcm_condition_avx2.o,)

$(SAN_DIR):
	mkdir -p $@

$(SAN_DIR)/pcm_condition_avx2.o: $(AVX2_SRC) $(DSP)/pcm_condition.h | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) -mavx2 -c $(AVX2_SRC) -o $@

$(SAN_DIR)/c-runtime-test: tests/runtime_test.c wire/pb_min.c common/rag_select.c common/http_min.c common/openai_min.c common/pcm_frame.c common/stage_json.c common/turn_response.c common/turn_response_json.c common/utf8.c common/vbus_subject.c common/base64.c common/runtime_identity.c $(KERNEL_SRCS) $(RMS_SRC) $(SAN_AVX2_OBJ) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tests/runtime_test.c wire/pb_min.c common/rag_select.c common/http_min.c common/openai_min.c common/pcm_frame.c common/stage_json.c common/turn_response.c common/turn_response_json.c common/utf8.c common/vbus_subject.c common/base64.c common/runtime_identity.c $(KERNEL_SRCS) $(RMS_SRC) $(SAN_AVX2_OBJ) $(SAN_LDFLAGS) $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-bus-test: tests/bus_test.c tests/bus_flow_test.h bus/vbus.c wire/pb_min.c $(KERNEL_SRCS) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tests/bus_test.c bus/vbus.c wire/pb_min.c $(KERNEL_SRCS) $(SAN_LDFLAGS) -lpthread -o $@

$(SAN_DIR)/c-dynbuf-test: tests/dynbuf_test.c $(DSP)/dynbuf.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tests/dynbuf_test.c $(DSP)/dynbuf.c $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-http-test: tests/http_test.c common/http_min.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tests/http_test.c common/http_min.c $(SAN_LDFLAGS) -lpthread -o $@

$(SAN_DIR)/c-auth-test: tests/auth_test.c common/voice_auth.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tests/auth_test.c common/voice_auth.c $(SAN_LDFLAGS) $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-webtransport-core-test: tests/webtransport_core_test.c $(WT_CORE_SRCS) common/turn_response.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-vbus-tx-bench: tests/bench_vbus_tx.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-vbus-process-latency-bench: tests/bench_vbus_process_latency.c bus/vbus.c $(DSP)/dynbuf.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-vbus-wildcard-bench: tests/bench_vbus_wildcards.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-vbus-queue-bench: tests/bench_vbus_queue_groups.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-vbus-storage-bench: tests/bench_vbus_storage.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-vbus-rx-window-bench: tests/bench_vbus_rx_windows.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-deadline-index-bench: tests/bench_deadline_indexes.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-session-index-bench: tests/bench_session_indexes.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-worker-queue-bench: tests/bench_worker_queues.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -lpthread -o $@

$(SAN_DIR)/c-nonce-cache-bench: tests/bench_nonce_cache.c common/voice_auth.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-webtransport-queue-bench: tests/bench_webtransport_queues.c $(WT_CORE_SRCS) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-http-queue-bench: tests/bench_http_queues.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $< $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-base64-bench: tests/bench_base64.c common/base64.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-stage-json-bench: tests/bench_stage_json.c common/stage_json.c wire/pb_min.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-response-json-bench: tests/bench_response_json.c common/turn_response.c common/turn_response_json.c common/base64.c common/stage_json.c common/utf8.c wire/pb_min.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-turn-decode-bench: tests/bench_turn_decode.c wire/pb_min.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-openai-request-bench: tests/bench_openai_request.c common/openai_min.c common/utf8.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-tts-request-bench: tests/bench_tts_request.c common/openai_min.c common/utf8.c $(SPEECH)/speech_sanitize.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-speech-segment-bench: tests/bench_speech_segment.c $(SPEECH)/speech_segment.c $(SPEECH)/speech_seg_host.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -o $@

$(SAN_DIR)/c-service-e2e: tests/service_e2e.c bus/vbus.c wire/pb_min.c $(DSP)/dynbuf.c common/service.c common/voice_auth.c $(DND_SRCS) common/http_min.c common/utf8.c common/model_request_capture.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-audio-processor: services/audio_processor_main.c $(COMMON_SRCS) $(KERNEL_SRCS) $(STT_VOCABULARY_SRCS) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -lpthread -o $@

$(SAN_DIR)/c-cascade-router: services/cascade_router_main.c common/model_request_capture.c $(COMMON_SRCS) $(KERNEL_SRCS) $(DND_SRCS) $(DND_RAG_SRCS) $(GROUNDING_SRCS) $(LOOP_SCENE_SRCS) $(AUTH_SRC) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-dnd-tools-test: tests/dnd_tools_test.c $(DND_SRCS) $(AUTH_SRC) common/http_min.c common/utf8.c wire/pb_min.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

sanitize-dnd-tools: $(SAN_DIR)/c-dnd-tools-test $(SAN_DIR)/c-service-e2e $(SAN_DIR)/c-cascade-router $(SAN_DIR)/c-voice-session-gateway $(SAN_DIR)/c-tts-module vbus-broker tool-host-for-tests
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(SAN_DIR)/c-dnd-tools-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 DND_TOOLS_CLIENT_BIN=$(abspath $(SAN_DIR)/c-dnd-tools-test) python3 tests/test_dnd_initiative.py
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 SERVICE_E2E_DND_TOOLS=1 CASCADE_ROUTER_BIN=$(SAN_DIR)/c-cascade-router GATEWAY_BIN=$(SAN_DIR)/c-voice-session-gateway TTS_MODULE_BIN=$(SAN_DIR)/c-tts-module $(SAN_DIR)/c-service-e2e

.PHONY: sanitize-dnd-citations
sanitize-dnd-citations: $(SAN_DIR)/c-service-e2e $(SAN_DIR)/c-voice-session-gateway vbus-broker
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 SERVICE_E2E_DND_CITATIONS=1 GATEWAY_BIN=$(SAN_DIR)/c-voice-session-gateway $(SAN_DIR)/c-service-e2e

sanitize: sanitize-dnd-tools sanitize-dnd-retrieval sanitize-dnd-citations

.PHONY: sanitize-dnd-audio-metadata
sanitize-dnd-audio-metadata: $(SAN_DIR)/c-service-e2e $(SAN_DIR)/c-voice-session-gateway vbus-broker
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 SERVICE_E2E_DND_AUDIO_METADATA=1 GATEWAY_BIN=$(SAN_DIR)/c-voice-session-gateway $(SAN_DIR)/c-service-e2e

sanitize: sanitize-dnd-audio-metadata

sanitize: sanitize-speech-markup-process
sanitize: sanitize-tts-wakeup-process
.PHONY: sanitize-tts-wakeup-process
sanitize-tts-wakeup-process: $(SAN_DIR)/c-service-e2e $(SAN_DIR)/c-tts-module $(SAN_DIR)/vbus-broker
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 SERVICE_E2E_TTS_WAKEUP=1 BROKER_BIN=$(SAN_DIR)/vbus-broker TTS_MODULE_BIN=$(SAN_DIR)/c-tts-module $(SAN_DIR)/c-service-e2e

sanitize-speech-markup-process: $(SAN_DIR)/c-service-e2e $(SAN_DIR)/c-cascade-router $(SAN_DIR)/vbus-broker
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 SERVICE_E2E_SPEECH_MARKUP=1 BROKER_BIN=$(SAN_DIR)/vbus-broker CASCADE_ROUTER_BIN=$(SAN_DIR)/c-cascade-router $(SAN_DIR)/c-service-e2e
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 SERVICE_E2E_SEGMENT_BOUND=1 BROKER_BIN=$(SAN_DIR)/vbus-broker CASCADE_ROUTER_BIN=$(SAN_DIR)/c-cascade-router $(SAN_DIR)/c-service-e2e

$(SAN_DIR)/c-voice-session-gateway: services/voice_session_gateway_main.c services/gateway_http.c common/voice_auth.c common/turn_response.c common/turn_response_json.c common/base64.c common/runtime_identity.c $(COMMON_SRCS) $(KERNEL_SRCS) $(ANALYTICS_SRCS) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -lpthread $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-session-manager: services/session_manager_main.c $(COMMON_SRCS) $(KERNEL_SRCS) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) services/session_manager_main.c $(COMMON_SRCS) $(KERNEL_SRCS) $(SAN_LDFLAGS) -lpthread -o $@

$(SAN_DIR)/c-tts-module: services/tts_module_main.c bus/vbus.c wire/pb_min.c common/service.c common/http_min.c common/openai_min.c common/pcm_frame.c common/utf8.c common/vbus_subject.c $(DSP)/dynbuf.c $(PCM)/pcm_parse.c $(SPEECH)/speech_sanitize.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -lpthread -o $@

$(SAN_DIR)/c-rag-gateway: services/rag_gateway_main.c $(COMMON_SRCS) $(KERNEL_SRCS) $(DND_RAG_SRCS) | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/vbus-broker: bus/vbus_broker_main.c bus/vbus.c $(DSP)/dynbuf.c | $(SAN_DIR)
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) $^ $(SAN_LDFLAGS) -lpthread -o $@

.PHONY: sanitize-rag-source
sanitize: sanitize-rag-source
sanitize-rag-source: $(SAN_DIR)/c-rag-gateway $(SAN_DIR)/vbus-broker .build/dnd-source-artifact/emit
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		CC="$(SAN_CC)" CFLAGS="$(SAN_CFLAGS)" RAG_TEST_RUNTIME_DIR="$(abspath $(SAN_DIR))" \
		python3 tests/test_rag_source.py

sanitize: all $(SAN_DIR)/c-runtime-test $(SAN_DIR)/c-bus-test $(SAN_DIR)/c-dynbuf-test $(SAN_DIR)/c-http-test $(SAN_DIR)/c-auth-test $(SAN_DIR)/c-webtransport-core-test $(SAN_DIR)/c-vbus-tx-bench $(SAN_DIR)/c-vbus-process-latency-bench $(SAN_DIR)/c-vbus-wildcard-bench $(SAN_DIR)/c-vbus-queue-bench $(SAN_DIR)/c-vbus-storage-bench $(SAN_DIR)/c-vbus-rx-window-bench $(SAN_DIR)/c-deadline-index-bench $(SAN_DIR)/c-session-index-bench $(SAN_DIR)/c-worker-queue-bench $(SAN_DIR)/c-nonce-cache-bench $(SAN_DIR)/c-webtransport-queue-bench $(SAN_DIR)/c-http-queue-bench $(SAN_DIR)/c-base64-bench $(SAN_DIR)/c-stage-json-bench $(SAN_DIR)/c-response-json-bench $(SAN_DIR)/c-turn-decode-bench $(SAN_DIR)/c-openai-request-bench $(SAN_DIR)/c-tts-request-bench $(SAN_DIR)/c-speech-segment-bench $(SAN_DIR)/c-service-e2e $(SAN_DIR)/c-audio-processor $(SAN_DIR)/c-cascade-router $(SAN_DIR)/c-voice-session-gateway $(SAN_DIR)/c-session-manager $(SAN_DIR)/c-tts-module $(SAN_DIR)/c-rag-gateway
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 bash tests/test_cascade_startup.sh $(SAN_DIR)/c-cascade-router ./vbus-broker
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-runtime-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-bus-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-dynbuf-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-http-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-auth-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-webtransport-core-test
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-vbus-tx-bench
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-vbus-process-latency-bench
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-vbus-wildcard-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-vbus-queue-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-vbus-storage-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-vbus-rx-window-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-deadline-index-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-session-index-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-worker-queue-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-nonce-cache-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-webtransport-queue-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-http-queue-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-base64-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-stage-json-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-response-json-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-turn-decode-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-openai-request-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-tts-request-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SAN_DIR)/c-speech-segment-bench --verify
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 SERVICE_E2E_BENCH_CONCURRENCY=4 AUDIO_PROCESSOR_BIN=$(SAN_DIR)/c-audio-processor CASCADE_ROUTER_BIN=$(SAN_DIR)/c-cascade-router GATEWAY_BIN=$(SAN_DIR)/c-voice-session-gateway SESSION_MANAGER_BIN=$(SAN_DIR)/c-session-manager TTS_MODULE_BIN=$(SAN_DIR)/c-tts-module RAG_GATEWAY_BIN=$(SAN_DIR)/c-rag-gateway $(SAN_DIR)/c-service-e2e

$(SAN_DIR)/c-tts-module-tsan: services/tts_module_main.c bus/vbus.c wire/pb_min.c common/service.c common/http_min.c common/openai_min.c common/pcm_frame.c common/utf8.c common/vbus_subject.c $(DSP)/dynbuf.c $(PCM)/pcm_parse.c $(SPEECH)/speech_sanitize.c | $(SAN_DIR)
	gcc $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=thread $^ -fsanitize=thread -lm -lpthread -o $@

$(SAN_DIR)/c-audio-processor-tsan: services/audio_processor_main.c $(COMMON_SRCS) $(KERNEL_SRCS) $(STT_VOCABULARY_SRCS) | $(SAN_DIR)
	gcc $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=thread $^ -fsanitize=thread -lm -lpthread -o $@

$(SAN_DIR)/c-cascade-router-tsan: services/cascade_router_main.c $(COMMON_SRCS) $(KERNEL_SRCS) $(DND_SRCS) $(DND_RAG_SRCS) $(GROUNDING_SRCS) $(LOOP_SCENE_SRCS) $(AUTH_SRC) | $(SAN_DIR)
	gcc $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=thread $^ -fsanitize=thread -lm -lpthread $(AUTH_LDLIBS) -o $@

$(SAN_DIR)/c-worker-queue-bench-tsan: tests/bench_worker_queues.c | $(SAN_DIR)
	gcc $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=thread $< -fsanitize=thread -lm -lpthread -o $@

$(SAN_DIR)/c-service-e2e-tsan: tests/service_e2e.c bus/vbus.c wire/pb_min.c $(DSP)/dynbuf.c common/service.c common/voice_auth.c $(DND_SRCS) common/http_min.c common/utf8.c common/model_request_capture.c | $(SAN_DIR)
	gcc $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=thread $^ -fsanitize=thread -lm -lpthread $(AUTH_LDLIBS) -o $@

thread-sanitize: all $(SAN_DIR)/c-tts-module-tsan $(SAN_DIR)/c-audio-processor-tsan $(SAN_DIR)/c-cascade-router-tsan $(SAN_DIR)/c-worker-queue-bench-tsan $(SAN_DIR)/c-service-e2e-tsan tool-host-for-tests | $(SAN_DIR)
	gcc $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=thread tests/bus_test.c bus/vbus.c wire/pb_min.c $(KERNEL_SRCS) -fsanitize=thread -lm -lpthread -o $(SAN_DIR)/c-bus-test-tsan
	TSAN_OPTIONS=halt_on_error=1 $(SAN_DIR)/c-bus-test-tsan
	TSAN_OPTIONS=halt_on_error=1 $(SAN_DIR)/c-worker-queue-bench-tsan --verify
	SERVICE_E2E_BENCH_CONCURRENCY=4 TTS_MODULE_BIN=$(SAN_DIR)/c-tts-module-tsan AUDIO_PROCESSOR_BIN=$(SAN_DIR)/c-audio-processor-tsan CASCADE_ROUTER_BIN=$(SAN_DIR)/c-cascade-router-tsan TSAN_OPTIONS=halt_on_error=1:exitcode=66 $(SAN_DIR)/c-service-e2e-tsan
	SERVICE_E2E_DND_TOOLS=1 CASCADE_ROUTER_BIN=$(SAN_DIR)/c-cascade-router-tsan TTS_MODULE_BIN=$(SAN_DIR)/c-tts-module-tsan TSAN_OPTIONS=halt_on_error=1:exitcode=66 $(SAN_DIR)/c-service-e2e-tsan

valgrind: test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-runtime-test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-bus-test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-dynbuf-test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-http-test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-auth-test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-webtransport-core-test
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-vbus-tx-bench
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-vbus-process-latency-bench
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-vbus-wildcard-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-vbus-queue-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-vbus-storage-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-vbus-rx-window-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-deadline-index-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-session-index-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-worker-queue-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-nonce-cache-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-webtransport-queue-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-http-queue-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-base64-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-stage-json-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-response-json-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-turn-decode-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-openai-request-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-tts-request-bench --verify
	valgrind --quiet --error-exitcode=97 --leak-check=full --show-leak-kinds=all ./c-speech-segment-bench --verify

check-no-go: all
	@for bin in $(SERVICES) vbus-broker c-runtime-test c-bus-test c-dynbuf-test c-http-test c-auth-test c-dnd-tools-test c-webtransport-core-test c-pb-interop c-service-e2e c-vbus-tx-bench c-vbus-process-latency-bench c-vbus-wildcard-bench c-vbus-queue-bench c-vbus-storage-bench c-vbus-rx-window-bench c-deadline-index-bench c-session-index-bench c-worker-queue-bench c-nonce-cache-bench c-webtransport-queue-bench c-http-queue-bench c-base64-bench c-stage-json-bench c-response-json-bench c-turn-decode-bench c-openai-request-bench c-tts-request-bench c-speech-segment-bench; do \
	  if nm -g "$$bin" 2>/dev/null | grep -E 'runtime\.|cgo\.|go\.' >/dev/null; then \
	    echo "FAIL Go symbols in $$bin"; exit 1; \
	  fi; \
	  echo "OK pure-C $$bin"; \
	done
	@if find . -name '*.go' | grep .; then echo FAIL go sources; exit 1; fi

# Gated path must not speak NATS protocol.
check-no-nats: all
	@if test -d nats; then echo "FAIL leftover nats/ directory"; exit 1; fi
	@for bin in $(SERVICES) vbus-broker c-bus-test c-dynbuf-test c-http-test c-auth-test c-dnd-tools-test c-webtransport-core-test c-pb-interop c-service-e2e c-vbus-tx-bench c-vbus-process-latency-bench c-vbus-wildcard-bench c-vbus-queue-bench c-vbus-storage-bench c-vbus-rx-window-bench c-deadline-index-bench c-session-index-bench c-worker-queue-bench c-nonce-cache-bench c-webtransport-queue-bench c-http-queue-bench c-base64-bench c-stage-json-bench c-response-json-bench c-turn-decode-bench c-openai-request-bench c-tts-request-bench c-speech-segment-bench; do \
	  if strings "$$bin" 2>/dev/null | grep -E '^CONNECT \{|nats://' >/dev/null; then \
	    echo "FAIL NATS protocol strings in $$bin"; strings "$$bin" | grep -E 'CONNECT \{|nats://'; exit 1; \
	  fi; \
	  echo "OK no NATS wire in $$bin"; \
	done
	@if rg -n 'nats_client_connect|CONNECT \{\"' services bus common 2>/dev/null | grep -v 'no NATS' | grep .; then \
	  echo "FAIL NATS client still referenced"; exit 1; \
	fi
	@echo OK gated path has zero NATS protocol

clean:
	rm -f $(SERVICES) $(ROLLBACK_SERVICES) c-webtransport-gateway c-webtransport-gateway-zero-audit c-webtransport-e2e-peer vbus-broker c-runtime-test c-bus-test c-dynbuf-test c-http-test c-auth-test c-dnd-tools-test c-webtransport-core-test c-pb-interop c-service-e2e c-vbus-tx-bench c-vbus-process-latency-bench c-vbus-wildcard-bench c-vbus-queue-bench c-vbus-storage-bench c-vbus-rx-window-bench c-deadline-index-bench c-session-index-bench c-worker-queue-bench c-nonce-cache-bench c-webtransport-queue-bench c-http-queue-bench c-base64-bench c-stage-json-bench c-response-json-bench c-turn-decode-bench c-openai-request-bench c-tts-request-bench c-speech-segment-bench
	rm -rf .build

# Offline, bounded complete-passage compilation. Runtime admission is separate.
DND_SOURCE_PASSAGE_SRCS := common/dnd_source_passage.c common/dnd_source_page.c common/utf8.c
DND_SOURCE_PASSAGE_HEADERS := common/dnd_source_passage.h $(DND_SOURCE_PAGE_HEADERS)
.PHONY: test-dnd-source-passage
test-dnd-source-passage: .build/dnd-source-passage/test
	.build/dnd-source-passage/test
.build/dnd-source-passage/test: tests/dnd_source_passage_test.c $(DND_SOURCE_PASSAGE_SRCS) $(DND_SOURCE_PASSAGE_HEADERS)
	@mkdir -p .build/dnd-source-passage
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/dnd_source_passage_test.c $(DND_SOURCE_PASSAGE_SRCS) $(AUTH_LDLIBS) -o $@
test: test-dnd-source-passage
.PHONY: dnd-source-passage-probe
dnd-source-passage-probe: .build/dnd-source-passage/probe
.build/dnd-source-passage/probe: $(ROOT)/benchmarks/retrieval-quality/c_passage_probe.c $(DND_SOURCE_PASSAGE_SRCS) $(DND_SOURCE_PASSAGE_HEADERS) $(DND_SOURCE_ARTIFACT_SRCS) $(DND_SOURCE_ARTIFACT_HEADERS)
	@mkdir -p .build/dnd-source-passage
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(sort $(DND_SOURCE_PASSAGE_SRCS) $(DND_SOURCE_ARTIFACT_SRCS)) $(AUTH_LDLIBS) -o $@

.PHONY: test-dnd-source-passage-artifact
test: test-dnd-source-passage-artifact
test-dnd-source-passage-artifact: .build/dnd-source-passage/artifact-test
	.build/dnd-source-passage/artifact-test
	.build/dnd-source-passage/artifact-test --capacity
.build/dnd-source-passage/artifact-test: tests/dnd_source_passage_artifact_test.c tests/dnd_source_passage_fixture.h common/dnd_source_encode.h common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_HEADERS) $(DND_SOURCE_ARTIFACT_SRCS)
	@mkdir -p .build/dnd-source-passage
	$(CC) $(CPPFLAGS) $(CFLAGS) $< common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o $@

.PHONY: sanitize-dnd-source-passage-artifact
sanitize: sanitize-dnd-source-passage-artifact
sanitize-dnd-source-passage-artifact:
	@mkdir -p .build/dnd-source-passage
	$(SAN_CC) $(CPPFLAGS) $(SAN_CFLAGS) tests/dnd_source_passage_artifact_test.c common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o .build/dnd-source-passage/artifact-sanitize
	ASAN_OPTIONS="$${PASSAGE_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 .build/dnd-source-passage/artifact-sanitize
	ASAN_OPTIONS="$${PASSAGE_ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" UBSAN_OPTIONS=halt_on_error=1 .build/dnd-source-passage/artifact-sanitize --capacity

.PHONY: test-dnd-source-passage-selection
test: test-dnd-source-passage-selection
test-dnd-source-passage-selection: .build/dnd-source-passage/selection-test
	.build/dnd-source-passage/selection-test
.build/dnd-source-passage/selection-test: tests/dnd_source_passage_selection_test.c tests/dnd_source_passage_fixture.h common/dnd_source_encode.h common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_HEADERS) $(DND_SOURCE_ARTIFACT_SRCS)
	@mkdir -p .build/dnd-source-passage
	$(CC) $(CPPFLAGS) $(CFLAGS) $< common/dnd_source_encode.c $(DND_SOURCE_ARTIFACT_SRCS) $(AUTH_LDLIBS) -o $@

.PHONY: test-passage-evidence test-action-budget test-spell-budget test-budget-projection test-action-facts test-spell-source-facts test-cast-event-data test-claim-evidence
test: test-passage-evidence test-action-budget test-spell-budget test-budget-projection test-action-facts test-spell-source-facts test-cast-event-data test-claim-evidence
test-passage-evidence:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p test_passage_evidence.py
test-action-budget:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p test_action_budget_eval.py
test-spell-budget:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p test_spell_budget.py
test-budget-projection:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p test_budget_projection_eval.py
test-action-facts:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_action_fact*.py'
test-spell-source-facts:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p test_spell_source_facts.py
test-cast-event-data:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_cast_event*.py'
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_multi_cast*.py'

test-claim-evidence:
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_claim_evidence.py'
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_context_cast_eval.py'
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_context_evidence_review.py'
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_context_repair_feedback.py'
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_context_repair_eval.py'
	$(RESEARCH_PYTHON) -m unittest discover -s ../../benchmarks/response-quality/turn-repair-v1 -p 'test_context_repair_analysis.py'
