#!/usr/bin/env bash
set -euo pipefail

gateway=${1:?gateway binary is required}
broker=${2:?broker binary is required}
test_dir=$(mktemp -d /tmp/c-webtransport-startup.XXXXXXXX)
broker_pid=
gateway_pid=

cleanup() {
  if [[ -n "${gateway_pid}" ]]; then
    kill "${gateway_pid}" 2>/dev/null || true
    wait "${gateway_pid}" 2>/dev/null || true
  fi
  if [[ -n "${broker_pid}" ]]; then
    kill "${broker_pid}" 2>/dev/null || true
    wait "${broker_pid}" 2>/dev/null || true
  fi
  rm -rf "${test_dir}"
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj /CN=voice.example.test \
  -keyout "${test_dir}/tls.key" \
  -out "${test_dir}/tls.crt" >/dev/null 2>&1

VBUS_PATH="${test_dir}/vbus.sock" "${broker}" >"${test_dir}/broker.log" 2>&1 &
broker_pid=$!
for _ in $(seq 1 100); do
  [[ -S "${test_dir}/vbus.sock" ]] && break
  kill -0 "${broker_pid}" 2>/dev/null
  sleep 0.02
done
[[ -S "${test_dir}/vbus.sock" ]]

if VBUS_PATH="${test_dir}/vbus.sock" \
  GATEWAY_WEBTRANSPORT_CERT_FILE="${test_dir}/tls.crt" \
  GATEWAY_WEBTRANSPORT_KEY_FILE="${test_dir}/tls.key" \
  VOICE_GATEWAY_TOKEN=0123456789abcdef0123456789abcdef \
  "${gateway}" >"${test_dir}/missing-origin.log" 2>&1; then
  echo "gateway accepted a missing origin policy" >&2
  exit 1
fi

if VBUS_PATH="${test_dir}/vbus.sock" \
  GATEWAY_WEBTRANSPORT_CERT_FILE="${test_dir}/tls.crt" \
  GATEWAY_WEBTRANSPORT_KEY_FILE="${test_dir}/tls.key" \
  GATEWAY_WEBTRANSPORT_AUTHORITY=voice-session-gateway.lab.daviestechlabs.io:18443 \
  GATEWAY_WEBTRANSPORT_ALLOWED_ORIGIN=https://companions.example.test \
  GATEWAY_WEBTRANSPORT_BIND_ADDRESS=invalid-address \
  VOICE_GATEWAY_TOKEN=0123456789abcdef0123456789abcdef \
  "${gateway}" >"${test_dir}/invalid-bind.log" 2>&1; then
  echo "gateway accepted an invalid bind address" >&2
  exit 1
fi

VBUS_PATH="${test_dir}/vbus.sock" \
GATEWAY_WEBTRANSPORT_CERT_FILE="${test_dir}/tls.crt" \
GATEWAY_WEBTRANSPORT_KEY_FILE="${test_dir}/tls.key" \
GATEWAY_WEBTRANSPORT_AUTHORITY=voice-session-gateway.lab.daviestechlabs.io:18443 \
GATEWAY_WEBTRANSPORT_ALLOWED_ORIGIN=https://companions.example.test \
GATEWAY_WEBTRANSPORT_PORT=18443 \
VOICE_GATEWAY_TOKEN=0123456789abcdef0123456789abcdef \
  "${gateway}" >"${test_dir}/gateway.log" 2>&1 &
gateway_pid=$!

for _ in $(seq 1 100); do
  if grep -F "pure-C WebTransport + datagrams" "${test_dir}/gateway.log" >/dev/null; then
    break
  fi
  kill -0 "${gateway_pid}" 2>/dev/null
  sleep 0.02
done

grep -F "pure-C WebTransport + datagrams" "${test_dir}/gateway.log" >/dev/null
kill -0 "${gateway_pid}"
echo "PASS WebTransport TLS/engine/VBus startup and fail-closed origin policy"
