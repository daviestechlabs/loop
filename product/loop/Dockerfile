FROM scratch AS toolchain
ADD product/loop/vendor-artifacts/alpine-minirootfs-3.24.1-x86_64.tar.gz /
COPY product/loop/vendor-artifacts/*.apk /tmp/vendor-apks/
RUN --network=none apk add --no-network --no-cache --repositories-file /dev/null --upgrade /tmp/vendor-apks/*.apk \
    && rm -rf /tmp/vendor-apks /var/cache/apk/*

FROM toolchain AS frontend
COPY product/loop/vendor-artifacts/bun-linux-x64-musl-1.3.14.zip /tmp/bun.zip
RUN --network=none unzip -q /tmp/bun.zip -d /tmp \
    && mv /tmp/bun-linux-x64-musl/bun /usr/local/bin/bun \
    && rm -rf /tmp/bun.zip /tmp/bun-linux-x64-musl
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

FROM toolchain AS build
WORKDIR /repo
COPY voice/c-runtime/common voice/c-runtime/common
COPY product/companions-frontend/c-companions product/companions-frontend/c-companions
COPY product/companions-frontend/c-product product/companions-frontend/c-product
COPY product/loop/server product/loop/server
RUN --network=none make -C product/loop/server clean all test

FROM scratch
ADD product/loop/vendor-artifacts/alpine-minirootfs-3.24.1-x86_64.tar.gz /
COPY product/loop/vendor-artifacts/musl-1.2.6-r2.apk \
     product/loop/vendor-artifacts/libcrypto3-*.apk \
     product/loop/vendor-artifacts/libssl3-*.apk \
     product/loop/vendor-artifacts/sqlite-libs-*.apk \
     product/loop/vendor-artifacts/ca-certificates-bundle-*.apk /tmp/runtime-apks/
RUN --network=none apk add --no-network --no-cache --repositories-file /dev/null --upgrade /tmp/runtime-apks/*.apk \
    && rm -rf /tmp/runtime-apks /var/cache/apk/* \
    && mkdir /data && chown 65532:65532 /data
COPY --from=build /repo/product/loop/server/loop-server /usr/local/bin/loop-server
COPY --from=frontend /repo/product/loop/static /srv
ENV LOOP_BIND=0.0.0.0 LOOP_PORT=8080 LOOP_STATIC=/srv LOOP_DB=/data/loop.sqlite
USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/loop-server"]
