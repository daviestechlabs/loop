# Public source build for Linux amd64. The lab's offline recipe stays separate.
FROM docker.io/library/debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential ca-certificates libssl-dev libsqlite3-dev unzip \
 && rm -rf /var/lib/apt/lists/*
COPY product/loop/vendor-artifacts/bun-linux-x64-1.3.14.zip /tmp/bun.zip
RUN unzip -q /tmp/bun.zip -d /tmp \
 && mv /tmp/bun-linux-x64/bun /usr/local/bin/bun \
 && rm -rf /tmp/bun.zip /tmp/bun-linux-x64
WORKDIR /repo/product/loop
COPY product/loop/package.json product/loop/bun.lock product/loop/vendor.lock.json ./
COPY product/loop/vendor-artifacts/npm-*.tgz ./vendor-artifacts/
COPY product/loop/scripts/install-vendor.ts ./scripts/install-vendor.ts
RUN --network=none bun scripts/install-vendor.ts
COPY contracts/ai-sdk /repo/contracts/ai-sdk
COPY product/loop/src ./src
COPY product/loop/scripts/build.ts ./scripts/build.ts
COPY product/loop/tsconfig.json ./
COPY product/loop/static ./static
RUN --network=none bun node_modules/typescript/bin/tsc --noEmit -p tsconfig.json && bun scripts/build.ts
WORKDIR /repo
COPY voice/c-runtime/common voice/c-runtime/common
COPY product/companions-frontend/c-companions product/companions-frontend/c-companions
COPY product/companions-frontend/c-product product/companions-frontend/c-product
COPY product/loop/server product/loop/server
RUN --network=none make -C product/loop/server clean all test

FROM docker.io/library/debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libssl3t64 libsqlite3-0 \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir /data && chown 65532:65532 /data && chmod 700 /data
COPY --from=build /repo/product/loop/server/loop-server /usr/local/bin/loop-server
COPY --from=build /repo/product/loop/static /srv
COPY LICENSE /usr/share/licenses/loop/LICENSE
COPY agents/platform-tools/LICENSE /usr/share/licenses/loop/platform-tools/LICENSE
COPY contracts/ai-sdk/LICENSE /usr/share/licenses/loop/ai-sdk/LICENSE
COPY contracts/handler-base/LICENSE /usr/share/licenses/loop/handler-base/LICENSE
COPY contracts/interaction-analytics/LICENSE /usr/share/licenses/loop/interaction-analytics/LICENSE
COPY contracts/prompt-library/LICENSE /usr/share/licenses/loop/prompt-library/LICENSE
COPY voice/audio-processor/LICENSE /usr/share/licenses/loop/audio-processor/LICENSE
COPY voice/voice-session-gateway/LICENSE /usr/share/licenses/loop/voice-session-gateway/LICENSE
COPY voice/tts-module/LICENSE /usr/share/licenses/loop/tts-module/LICENSE
COPY product/loop/open-source/THIRD-PARTY-NOTICES.md /usr/share/licenses/loop/THIRD-PARTY-NOTICES.md
ENV LOOP_BIND=0.0.0.0 LOOP_PORT=8080 LOOP_STATIC=/srv LOOP_DB=/data/loop.sqlite
USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/loop-server"]
