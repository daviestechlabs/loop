#!/usr/bin/env bash
# Shared native build environment. Source this file; do not execute it.
native_gc_flag=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then
  native_gc_flag=-Wl,-dead_strip
  export CFLAGS="${CFLAGS:-} -D_DARWIN_C_SOURCE"
  native_openssl="${OPENSSL_PREFIX:-}"
  if [[ -z "$native_openssl" ]] && command -v brew >/dev/null 2>&1; then
    native_openssl="$(brew --prefix openssl@3)"
  fi
  if [[ -z "$native_openssl" || ! -f "$native_openssl/include/openssl/evp.h" ]]; then
    echo 'OpenSSL headers missing; set OPENSSL_PREFIX to the installed OpenSSL prefix' >&2
    return 2
  fi
  export CPATH="$native_openssl/include${CPATH:+:$CPATH}"
  export LIBRARY_PATH="$native_openssl/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
