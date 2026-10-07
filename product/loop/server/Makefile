CC ?= cc
COMMON = ../../../voice/c-runtime/common
CMP = ../../companions-frontend/c-companions
PRODUCT = ../../companions-frontend/c-product
OPENSSL_PREFIX ?= $(shell test ! -d /opt/homebrew/opt/openssl@3 || echo /opt/homebrew/opt/openssl@3)
CPPFLAGS += -I. -I$(CMP) -I$(PRODUCT) -I$(COMMON) $(if $(OPENSSL_PREFIX),-I$(OPENSSL_PREFIX)/include)
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS += $(if $(OPENSSL_PREFIX),-L$(OPENSSL_PREFIX)/lib) -lssl -lcrypto -lsqlite3 -lpthread -lm
CORE = configuration.c anvil_remote.c retention.c capacity.c loop.c storage.c backup.c evidence.c report.c replay.c anvil_links.c anvil_export.c anvil_bundle.c rerun.c model_request.c $(CMP)/cmp_json.c $(CMP)/cmp_gateway_identity.c $(CMP)/cmp_oauth.c $(CMP)/cmp_index.c $(PRODUCT)/product.c $(COMMON)/utf8.c $(COMMON)/voice_auth.c $(COMMON)/model_request_capture.c
.PHONY: all test sanitize clean
all: loop-server
loop-server: main.c http.c $(CORE) loop.h capacity.h retention.h anvil_remote.h
	$(CC) $(CPPFLAGS) $(CFLAGS) main.c http.c $(CORE) $(LDLIBS) -o $@
loop-test: test_loop.c $(CORE) loop.h capacity.h retention.h anvil_remote.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLOOP_TESTING test_loop.c $(CORE) $(LDLIBS) -o $@
loop-test-oauth: test_oauth.c $(CORE) loop.h capacity.h retention.h anvil_remote.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_oauth.c $(CORE) $(LDLIBS) -o $@
loop-test-sso: test_sso.c $(CORE) loop.h capacity.h retention.h anvil_remote.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_sso.c $(CORE) $(LDLIBS) -o $@
loop-test-capacity: test_capacity.c capacity.c capacity.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_capacity.c capacity.c -o $@
loop-test-retention: test_retention.c $(CORE) loop.h retention.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_retention.c $(CORE) $(LDLIBS) -o $@
test: loop-test-anvil-remote loop-test loop-test-oauth loop-test-sso loop-test-capacity loop-test-retention
	./loop-test-anvil-remote
	./loop-test-retention
	./loop-test-capacity
	./loop-test
	./loop-test-oauth
	./loop-test-sso
sanitize:
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_anvil_remote.c $(CORE) $(LDLIBS) -o loop-test-anvil-remote-asan
	./loop-test-anvil-remote-asan
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_retention.c $(CORE) $(LDLIBS) -o loop-test-retention-asan
	./loop-test-retention-asan
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_capacity.c capacity.c -o loop-test-capacity-asan
	./loop-test-capacity-asan
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLOOP_TESTING -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_loop.c $(CORE) $(LDLIBS) -o loop-test-asan
	./loop-test-asan
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_oauth.c $(CORE) $(LDLIBS) -o loop-test-oauth-asan
	./loop-test-oauth-asan
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_sso.c $(CORE) $(LDLIBS) -o loop-test-sso-asan
	./loop-test-sso-asan
clean:
	rm -f loop-test-session-https
	rm -f loop-test-anvil-remote loop-test-anvil-remote-asan loop-test-retention loop-test-retention-asan loop-test-capacity loop-test-capacity-asan loop-test-sso loop-test-sso-asan loop-server loop-test-oauth loop-test-oauth-asan loop-test loop-test-asan loop-test-http loop-test-http-asan
loop-test-http: fixture_main.c http.c $(CORE) loop.h capacity.h retention.h anvil_remote.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLOOP_TESTING fixture_main.c http.c $(CORE) $(LDLIBS) -o $@
loop-test-http-asan: fixture_main.c http.c $(CORE) loop.h capacity.h retention.h anvil_remote.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLOOP_TESTING -O1 -fsanitize=address,undefined -fno-omit-frame-pointer fixture_main.c http.c $(CORE) $(LDLIBS) -o $@

loop-test-anvil-remote: test_anvil_remote.c $(CORE) loop.h anvil_remote.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_anvil_remote.c $(CORE) $(LDLIBS) -o $@

loop-test-session-https: test_session_https.c $(CORE) loop.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_session_https.c $(CORE) $(LDLIBS) -o $@
