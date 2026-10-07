# Pure-C voice runtime image. It contains VBus, the services, and the QUIC edge.
# From monorepo root:
#   docker build --build-arg SOURCE_REVISION=$(git rev-parse HEAD) \
#     -f voice/c-runtime/Dockerfile -t voice-c-runtime:poc .

FROM docker.io/library/gcc:16.2.0-trixie@sha256:2e6f4ae27ab8ad6f9617b46f0468889d368fdd91f2ea29aff98e4d5d1a3637f0 AS build
ARG SOURCE_REVISION=0000000000000000000000000000000000000000
ARG AWS_LC_COMMIT=991e67ff4cf04df4dd89e407f8b920c6936cb56a
ARG LSQUIC_COMMIT=19547405c24f60c4537478d38f4214e990be1f95
ARG TEST_TARGET=test
# Let Trixie resolve current security revisions during each image rebuild.
# The base, source commits, and promoted output remain immutable by digest.
# hadolint ignore=DL3008
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates clang cmake git libclang-rt-dev libssl-dev ninja-build perl protobuf-compiler zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

# AWS-LC exposes the QUIC TLS callbacks that LSQUIC requires. DISABLE_GO keeps
# the gated image build independent of Go and uses AWS-LC's generated C files.
RUN git init /deps/aws-lc \
 && git -C /deps/aws-lc remote add origin https://github.com/aws/aws-lc.git \
 && git -C /deps/aws-lc fetch --depth 1 origin "${AWS_LC_COMMIT}" \
 && git -C /deps/aws-lc checkout --detach FETCH_HEAD \
 && test "$(git -C /deps/aws-lc rev-parse HEAD)" = "${AWS_LC_COMMIT}" \
 && cmake -S /deps/aws-lc -B /deps/aws-lc-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DBUILD_SHARED_LIBS=OFF \
      -DDISABLE_GO=ON \
      -DINSTALL_OPENSSL_SHIM=ON \
      -DCMAKE_INSTALL_PREFIX=/opt/quic \
 && cmake --build /deps/aws-lc-build --target install --parallel 2

COPY voice/c-runtime/patches/lsquic-disabled-debug-hexdump.patch /tmp/lsquic-disabled-debug-hexdump.patch
COPY voice/c-runtime/patches/lsquic-webtransport-stream-flags.patch /tmp/lsquic-webtransport-stream-flags.patch
COPY voice/c-runtime/patches/lsquic-webtransport-safari-settings.patch /tmp/lsquic-webtransport-safari-settings.patch
COPY voice/c-runtime/tests/test_webtransport_settings_wire.sh voice/c-runtime/tests/webtransport_settings_wire_test.c /tmp/wt-settings-test/
RUN git init /deps/lsquic \
 && git -C /deps/lsquic remote add origin https://github.com/litespeedtech/lsquic.git \
 && git -C /deps/lsquic fetch --depth 1 origin "${LSQUIC_COMMIT}" \
 && git -C /deps/lsquic checkout --detach FETCH_HEAD \
 && test "$(git -C /deps/lsquic rev-parse HEAD)" = "${LSQUIC_COMMIT}" \
 && git -C /deps/lsquic apply --check /tmp/lsquic-disabled-debug-hexdump.patch \
 && git -C /deps/lsquic apply /tmp/lsquic-disabled-debug-hexdump.patch \
 && git -C /deps/lsquic apply --check /tmp/lsquic-webtransport-stream-flags.patch \
 && git -C /deps/lsquic apply /tmp/lsquic-webtransport-stream-flags.patch \
 && git -C /deps/lsquic apply --check /tmp/lsquic-webtransport-safari-settings.patch \
 && git -C /deps/lsquic apply /tmp/lsquic-webtransport-safari-settings.patch \
 && bash /tmp/wt-settings-test/test_webtransport_settings_wire.sh /deps/lsquic \
 && git -C /deps/lsquic submodule update --init --depth 1 --recursive \
 && cmake -S /deps/lsquic -B /deps/lsquic-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLSQUIC_BIN=OFF \
      -DLSQUIC_TESTS=OFF \
      -DLSQUIC_WEBTRANSPORT=ON \
      -DBORINGSSL_DIR=/opt/quic \
      -DCMAKE_INSTALL_PREFIX=/opt/quic \
 && cmake --build /deps/lsquic-build --target install --parallel 2
WORKDIR /src
COPY voice/c-runtime /src/voice/c-runtime
COPY voice/audio-processor/dsp /src/voice/audio-processor/dsp
COPY voice/cascade-router/internal/routeclassifier /src/voice/cascade-router/internal/routeclassifier
COPY voice/cascade-router/internal/cascade /src/voice/cascade-router/internal/cascade
COPY voice/cascade-router/internal/systemone /src/voice/cascade-router/internal/systemone
COPY voice/voice-session-gateway/reflex-c /src/voice/voice-session-gateway/reflex-c
COPY voice/tts-module/pcm /src/voice/tts-module/pcm
COPY contracts/telemetry-c /src/contracts/telemetry-c
COPY contracts/handler-base/proto /src/contracts/handler-base/proto
COPY contracts/handler-base/speechsegment /src/contracts/handler-base/speechsegment
COPY contracts/handler-base/turn_c /src/contracts/handler-base/turn_c
COPY contracts/handler-base/c-pb/pb_msg.h contracts/handler-base/c-pb/pb_wire.h contracts/handler-base/c-pb/pb_wire.c contracts/handler-base/c-pb/pb_dnd_request.h contracts/handler-base/c-pb/pb_dnd_campaign.h contracts/handler-base/c-pb/pb_dnd_action.h /src/contracts/handler-base/c-pb/
COPY contracts/interaction-analytics/c-iaevents /src/contracts/interaction-analytics/c-iaevents
COPY contracts/prompt-library/c-prompt /src/contracts/prompt-library/c-prompt
COPY contracts/prompt-library/prompts/rag /src/contracts/prompt-library/prompts/rag
COPY contracts/prompt-library/prompts/voice /src/contracts/prompt-library/prompts/voice
# The BM25 process test compiles and drives this native probe in the build stage.
COPY benchmarks/retrieval-quality/c_rag_probe.c /src/benchmarks/retrieval-quality/c_rag_probe.c
COPY benchmarks/retrieval-quality/scripts/build-c-probe.sh /src/benchmarks/retrieval-quality/scripts/build-c-probe.sh
COPY benchmarks/retrieval-quality/scripts/native-env.sh /src/benchmarks/retrieval-quality/scripts/native-env.sh
# The default test target runs the offline D&D research contracts in this stage.
COPY benchmarks/response-quality/turn-repair-v1 /src/benchmarks/response-quality/turn-repair-v1
COPY agents/platform-tools/c-dice /src/agents/platform-tools/c-dice
COPY agents/platform-tools/c-toolstore /src/agents/platform-tools/c-toolstore
COPY agents/platform-tools/c-dnd /src/agents/platform-tools/c-dnd
# These host sources support the native process test in the build stage only.
COPY agents/platform-tools/c-toolpolicy /src/agents/platform-tools/c-toolpolicy
COPY agents/platform-tools/c-campaign /src/agents/platform-tools/c-campaign
COPY agents/platform-tools/c-ptools /src/agents/platform-tools/c-ptools
COPY agents/interaction-control-plane/examples/schemas/tools /src/agents/interaction-control-plane/examples/schemas/tools
COPY product/companions-frontend/c-companions/cmp_json.c product/companions-frontend/c-companions/cmp_json.h /src/product/companions-frontend/c-companions/
COPY product/companions-frontend/c-companions/cmp_turn_metadata.c product/companions-frontend/c-companions/cmp_turn_metadata.h product/companions-frontend/c-companions/cmp_turn_metadata_policy.h /src/product/companions-frontend/c-companions/
COPY product/companions-frontend/c-companions/cmp_http_headers.c product/companions-frontend/c-companions/cmp_http_headers.h /src/product/companions-frontend/c-companions/
COPY product/companions-frontend/c-entitlements/ent_books.c product/companions-frontend/c-entitlements/ent_books.h /src/product/companions-frontend/c-entitlements/
WORKDIR /src/voice/c-runtime
RUN case "$TEST_TARGET" in test|test-source) ;; *) exit 1 ;; esac \
 && make clean "$TEST_TARGET" check-no-go check-no-nats \
      VOICE_SOURCE_REVISION="${SOURCE_REVISION}" \
      CPPFLAGS="-I/opt/quic/include" \
      AUTH_LDLIBS="-L/opt/quic/lib -Wl,-Bstatic -lcrypto -Wl,-Bdynamic -lpthread -ldl" \
 && make check-webtransport \
      VOICE_SOURCE_REVISION="${SOURCE_REVISION}" \
      LSQUIC_CPPFLAGS="-isystem /opt/quic/include/lsquic -isystem /opt/quic/include -DLSQUIC_WEBTRANSPORT_SERVER_SUPPORT=1" \
      LSQUIC_LDLIBS="-L/opt/quic/lib -Wl,-Bstatic -llsquic -lssl -lcrypto -lz -Wl,-Bdynamic -lpthread -ldl -lm" \
 && readelf -d c-voice-session-gateway > /tmp/voice-session-gateway.dynamic \
 && readelf -d c-cascade-router > /tmp/cascade-router.dynamic \
 && readelf -d c-webtransport-gateway > /tmp/webtransport-gateway.dynamic \
 && ! grep -E 'Shared library: \[(libstdc\+\+|libssl|libcrypto|libz)\.so' \
      /tmp/voice-session-gateway.dynamic /tmp/webtransport-gateway.dynamic /tmp/cascade-router.dynamic \
 && strip c-audio-processor c-cascade-router \
          c-voice-session-gateway c-webtransport-gateway c-session-manager \
          c-tts-module c-rag-gateway vbus-broker

FROM gcr.io/distroless/cc-debian13:nonroot@sha256:c31ff9abcb1910f3ab25c7957bdaf0bfe12a01eb546e8df2282f1c8f682b606c
ENV PROMPT_LIBRARY_ROOT=/opt/voice/prompt-library
COPY --from=build /src/contracts/prompt-library/prompts/rag /opt/voice/prompt-library/prompts/rag
COPY --from=build /src/contracts/prompt-library/prompts/voice /opt/voice/prompt-library/prompts/voice
COPY --from=build /src/voice/c-runtime/c-audio-processor /usr/local/bin/c-audio-processor
COPY --from=build /src/voice/c-runtime/c-cascade-router /usr/local/bin/c-cascade-router
COPY --from=build /src/voice/c-runtime/c-voice-session-gateway /usr/local/bin/c-voice-session-gateway
COPY --from=build /src/voice/c-runtime/c-webtransport-gateway /usr/local/bin/c-webtransport-gateway
COPY --from=build /src/voice/c-runtime/c-session-manager /usr/local/bin/c-session-manager
COPY --from=build /src/voice/c-runtime/c-tts-module /usr/local/bin/c-tts-module
COPY --from=build /src/voice/c-runtime/c-rag-gateway /usr/local/bin/c-rag-gateway
COPY --from=build /src/voice/c-runtime/vbus-broker /usr/local/bin/vbus-broker
ENV VBUS_PATH=/run/voice-vbus/bus.sock
USER nonroot:nonroot
# The Deployment overrides this command for each process.
ENTRYPOINT ["/usr/local/bin/c-audio-processor"]
