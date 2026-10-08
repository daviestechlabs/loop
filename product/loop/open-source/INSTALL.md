# Install Loop from source

Run all commands from the source tree root.
Use a separate working copy for builds.
Keep model files, recordings, and training archives outside the source tree.

## Native source checks

Linux needs a C compiler, Make, Python 3.9 or later, OpenSSL development headers, SQLite development headers, tar, and Bun 1.3.14.
The workflow installs these dependencies on Ubuntu 24.04.
Its host dependency lock selects the glibc Bun build for Ubuntu.
The image dependency lock retains the musl Bun build for Alpine.

The dependency fetcher downloads seven exact npm archives from their public sources.
It checks locked size, SHA-256, and npm SHA-512 integrity before installation.
The installer runs no package lifecycle scripts.

```sh
bash product/loop/open-source/check.sh
```

On an Apple Silicon Mac, install OpenSSL and SQLite development libraries first.
Use these additional compiler settings for the voice runtime.

```sh
export CPPFLAGS=-I/opt/homebrew/opt/openssl@3/include
export AUTH_LDLIBS='-L/opt/homebrew/opt/openssl@3/lib -lcrypto'
bash product/loop/open-source/check.sh
```

The checks build the frontend and Loop C server.
They run authentication, ownership, capture, recovery, sanitizer, and bounded C voice tests.
The public voice test target excludes separately held research datasets.
The full private test target remains available in the original monorepo.

## Local images

The Loop image recipe targets Linux amd64.
The public Loop recipe uses a pinned Debian base and public system packages.
The fetcher verifies Bun and npm archives before the build.
Frontend and C build steps use no network.
The voice image uses its existing pinned native QUIC sources and public base images.

The original lab recipe retains its offline Alpine archive lock.
Some pinned Alpine archives are no longer available from their original public URLs.
That recipe requires an existing verified archive cache.

```sh
python3 product/loop/open-source/fetch-dependencies.py
python3 product/loop/open-source/fetch-dependencies.py --group bun-host
docker build --platform linux/amd64 -f product/loop/open-source/Dockerfile -t loop-source:reviewed .
docker build --build-arg TEST_TARGET=test-source -f voice/c-runtime/Dockerfile -t loop-voice-source:reviewed .
```

Local image creation does not authorize image redistribution.
Preserve third-party notices and meet applicable source obligations before distributing binaries.
The CI workflow builds images but never publishes them.

## Configure authenticated access

Loop uses an existing OpenID Connect gateway in front of its HTTP service.
OpenID Connect is the identity protocol; the short form is OIDC.
Configure the Authentik provider for this demo.
Configure its application for OIDC login.
Use an explicit demo group, such as `loop-demo-users`.
Bind the application to that group and require authentication.

Use the provider's actual issuer, audience, and HTTPS signing-key URL in Loop.
Use authorization-code login and the gateway's configured callback URL.
No example secret or issuer is a working credential.

The gateway must remove incoming `x-envoy-oidc-id-token` headers before setting its verified ID token.
Loop checks the token signature, issuer, audience, expiry, and required group again.
Keep the Loop HTTP listener reachable only by that gateway.
Protect the browser origin with a trusted HTTPS certificate.
Configure the gateway's shared logout path as `/oauth2/logout`.
This example preserves the existing gateway contract; it does not install another identity proxy.

Copy the example configuration, then set its private values.

```sh
cp product/loop/open-source/examples/loop.env.example product/loop/open-source/examples/loop.env
chmod 600 product/loop/open-source/examples/loop.env
docker compose -f product/loop/open-source/examples/compose.yaml up --build -d
```

The Compose example binds HTTP to host loopback port 8080.
Place the authenticated HTTPS gateway in front of that listener.
The container stores private evidence in its persistent volume.
Optional recording discloses audio, evidence, and model context before consent.
Set storage limits and operator backups before admitting users.
Recovery snapshots retain 15 days; training archives retain their separate indefinite-retention intent.

## Connect voice and Anvil

Run the existing C voice services and VBus with your configured native engines.
Use the included voice runtime source and its existing service configuration.
The gateway example lists the QUIC edge's public authority, route, allowed origin, and TLS paths.
Set the same private purpose-separated `VOICE_GATEWAY_TOKEN` in Loop and the C gateway.
Keep TLS private keys and signing keys outside this source tree.

Expose UDP port 8443 for QUIC and use a browser-trusted certificate for the voice hostname.
The WebTransport route is exactly `/v1/voice/turns`.
Loop's connection policy permits only its own origin and the configured voice origin.

Set `LOOP_ANVIL_ORIGIN` to your existing Anvil HTTPS origin.
Configure Anvil's delegated Loop evidence routes for the same verified issuer, audience, and demo group.
Anvil retains its owner checks, evidence admission, execution, reviews, and promotion.
Model placement and engines remain operator configuration.
This guide supplies no model weights, datasets, voice recordings, or anonymous execution access.

## Acceptance

Check a fresh login and logout with an admitted user.
Check that an unadmitted user cannot obtain a voice identity or another user's recordings.
Use approved saved input for initial connected tests.
The user performs live phone microphone, interruption, and latency tests.
Synthetic checks do not close those acceptance gates.
