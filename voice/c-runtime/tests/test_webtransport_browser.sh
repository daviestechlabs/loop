#!/usr/bin/env bash
set -euo pipefail

gateway=${1:?gateway binary is required}
broker=${2:?broker binary is required}
peer=${3:?oracle peer binary is required}
runtime_dir=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d /tmp/c-webtransport-browser.XXXXXXXX)
request_id=wt-oracle-request
endpoint_request_id=wt-oracle-request-endpoint
endpoint_conflict_request_id=wt-oracle-request-endpoint-conflict
audio_request_id=wt-oracle-request-audio
reuse_request_id=wt-oracle-request-reuse
reconnect_request_id=wt-oracle-request-reconnect
secret=0123456789abcdef0123456789abcdef
browser_path=${K6_BROWSER_EXECUTABLE_PATH:-}
peer_mode=${WT_ORACLE_PEER_MODE:-serve}
callgrind_turn_only=${WT_ORACLE_CALLGRIND_TURN_ONLY:-0}
transport_port=${WT_ORACLE_TRANSPORT_PORT:-$((20000 + BASHPID % 10000))}
page_port=${WT_ORACLE_PAGE_PORT:-$((40000 + BASHPID % 10000))}
broker_pid=
gateway_pid=
peer_pid=
page_pid=

run_gateway() {
  if [[ -n "${WT_ORACLE_CALLGRIND_OUT:-}" ]]; then
    if [[ "${callgrind_turn_only}" == 1 ]]; then
      exec valgrind --quiet --tool=callgrind --instr-atstart=no \
        --callgrind-out-file="${WT_ORACLE_CALLGRIND_OUT}" "${gateway}"
    fi
    exec valgrind --quiet --tool=callgrind \
      --callgrind-out-file="${WT_ORACLE_CALLGRIND_OUT}" "${gateway}"
  fi
  if [[ "${WT_ORACLE_MEMCHECK:-0}" == 1 ]]; then
    exec valgrind --quiet --error-exitcode=97 --leak-check=full \
      --errors-for-leak-kinds=definite,indirect \
      --show-leak-kinds=definite,indirect "${gateway}"
  fi
  exec "${gateway}"
}

cleanup() {
  status=$?
  if [[ ${status} -ne 0 ]]; then
    for log in gateway peer page broker; do
      if [[ -f "${test_dir}/${log}.log" ]]; then
        echo "--- ${log}.log" >&2
        tail -300 "${test_dir}/${log}.log" >&2 || true
      fi
    done
  fi
  for pid in "${page_pid}" "${peer_pid}" "${gateway_pid}" "${broker_pid}"; do
    if [[ -n "${pid}" ]]; then
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
  rm -rf "${test_dir}"
  return "${status}"
}
trap cleanup EXIT

if ! command -v k6 >/dev/null 2>&1; then
  echo "k6 is required for the browser WebTransport oracle" >&2
  exit 1
fi
if [[ -z "${browser_path}" || ! -x "${browser_path}" ]]; then
  echo "K6_BROWSER_EXECUTABLE_PATH must name a Chromium executable" >&2
  exit 1
fi
if [[ "${peer_mode}" != serve && "${peer_mode}" != serve-legacy ]]; then
  echo "WT_ORACLE_PEER_MODE must be serve or serve-legacy" >&2
  exit 1
fi
if [[ "${callgrind_turn_only}" != 0 && "${callgrind_turn_only}" != 1 ]]; then
  echo "WT_ORACLE_CALLGRIND_TURN_ONLY must be zero or one" >&2
  exit 1
fi
if [[ "${callgrind_turn_only}" == 1 && -z "${WT_ORACLE_CALLGRIND_OUT:-}" ]]; then
  echo "Turn-only profiling requires WT_ORACLE_CALLGRIND_OUT" >&2
  exit 1
fi
if [[ "${callgrind_turn_only}" == 1 ]] && ! command -v callgrind_control >/dev/null 2>&1; then
  echo "callgrind_control is required for turn-only profiling" >&2
  exit 1
fi
for port in "${transport_port}" "${page_port}"; do
  if [[ ! "${port}" =~ ^[0-9]+$ ]] || (( port < 1024 || port > 65535 )); then
    echo "WebTransport oracle ports must be integers from 1024 through 65535" >&2
    exit 1
  fi
done
if [[ "${transport_port}" == "${page_port}" ]]; then
  echo "WebTransport and page ports must differ" >&2
  exit 1
fi
if [[ -n "${WT_ORACLE_CALLGRIND_OUT:-}" && "${WT_ORACLE_MEMCHECK:-0}" == 1 ]]; then
  echo "Callgrind and Memcheck modes are mutually exclusive" >&2
  exit 1
fi

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 \
  -subj /CN=127.0.0.1 \
  -addext subjectAltName=IP:127.0.0.1,DNS:localhost \
  -keyout "${test_dir}/tls.key" \
  -out "${test_dir}/tls.crt" >/dev/null 2>&1
openssl x509 -in "${test_dir}/tls.crt" -outform DER -out "${test_dir}/tls.der"
certificate_hash=$(openssl dgst -sha256 -binary "${test_dir}/tls.der" | openssl base64 -A)
identity_token=$("${peer}" issue "${secret}" "${request_id}")
endpoint_identity_token=$("${peer}" issue "${secret}" "${endpoint_request_id}")
endpoint_conflict_identity_token=$(
  "${peer}" issue "${secret}" "${endpoint_conflict_request_id}"
)
audio_identity_token=$("${peer}" issue "${secret}" "${audio_request_id}")
reuse_identity_token=$("${peer}" issue "${secret}" "${reuse_request_id}")
reconnect_identity_token=$("${peer}" issue-free "${secret}" "${reconnect_request_id}")

VBUS_PATH="${test_dir}/vbus.sock" "${broker}" >"${test_dir}/broker.log" 2>&1 &
broker_pid=$!
for _ in $(seq 1 100); do
  [[ -S "${test_dir}/vbus.sock" ]] && break
  kill -0 "${broker_pid}" 2>/dev/null
  sleep 0.02
done
[[ -S "${test_dir}/vbus.sock" ]]

(
  cd "${runtime_dir}/tests"
  exec openssl s_server -quiet -WWW -accept "${page_port}" \
    -cert "${test_dir}/tls.crt" -key "${test_dir}/tls.key"
) >"${test_dir}/page.log" 2>&1 &
page_pid=$!

VBUS_PATH="${test_dir}/vbus.sock" \
WT_ORACLE_RESPONSE_DELAY_MS="${WT_ORACLE_RESPONSE_DELAY_MS:-250}" \
  "${peer}" "${peer_mode}" "${request_id}" \
  "${endpoint_request_id}" \
  "${audio_request_id}" \
  "${reuse_request_id}" \
  "${reconnect_request_id}" \
  >"${test_dir}/peer.log" 2>&1 &
peer_pid=$!

VBUS_PATH="${test_dir}/vbus.sock" \
GATEWAY_WEBTRANSPORT_CERT_FILE="${test_dir}/tls.crt" \
GATEWAY_WEBTRANSPORT_KEY_FILE="${test_dir}/tls.key" \
GATEWAY_WEBTRANSPORT_AUTHORITY="localhost:${transport_port}" \
GATEWAY_WEBTRANSPORT_ALLOWED_ORIGIN="https://localhost:${page_port}" \
GATEWAY_WEBTRANSPORT_PORT="${transport_port}" \
GATEWAY_WEBTRANSPORT_KEEPALIVE_MS="${WT_ORACLE_KEEPALIVE_MS:-100}" \
VOICE_GATEWAY_TOKEN="${secret}" \
  run_gateway >"${test_dir}/gateway.log" 2>&1 &
gateway_pid=$!

for _ in $(seq 1 500); do
  if grep -F "pure-C WebTransport + datagrams" "${test_dir}/gateway.log" >/dev/null; then
    break
  fi
  kill -0 "${gateway_pid}" 2>/dev/null
  sleep 0.02
done
grep -F "pure-C WebTransport + datagrams" "${test_dir}/gateway.log" >/dev/null

for _ in $(seq 1 100); do
  if curl --silent --insecure --fail \
      "https://localhost:${page_port}/webtransport_oracle_index.html" >/dev/null; then
    break
  fi
  kill -0 "${page_pid}" 2>/dev/null
  sleep 0.02
done

if [[ "${callgrind_turn_only}" == 1 ]]; then
  callgrind_control --instr=on --zero "${gateway_pid}" >/dev/null
fi

K6_BROWSER_EXECUTABLE_PATH="${browser_path}" \
WT_ORACLE_BASE_URL="https://localhost:${page_port}/webtransport_oracle_index.html" \
WT_ORACLE_WEBTRANSPORT_URL="https://localhost:${transport_port}/v1/voice/turns" \
WT_ORACLE_CERT_SHA256="${certificate_hash}" \
WT_ORACLE_IDENTITY_TOKEN="${identity_token}" \
WT_ORACLE_REQUEST_ID="${request_id}" \
WT_ORACLE_ENDPOINT_IDENTITY_TOKEN="${endpoint_identity_token}" \
WT_ORACLE_ENDPOINT_REQUEST_ID="${endpoint_request_id}" \
WT_ORACLE_ENDPOINT_CONFLICT_IDENTITY_TOKEN="${endpoint_conflict_identity_token}" \
WT_ORACLE_ENDPOINT_CONFLICT_REQUEST_ID="${endpoint_conflict_request_id}" \
WT_ORACLE_AUDIO_IDENTITY_TOKEN="${audio_identity_token}" \
WT_ORACLE_AUDIO_REQUEST_ID="${audio_request_id}" \
WT_ORACLE_REUSE_IDENTITY_TOKEN="${reuse_identity_token}" \
WT_ORACLE_REUSE_REQUEST_ID="${reuse_request_id}" \
WT_ORACLE_RECONNECT_IDENTITY_TOKEN="${reconnect_identity_token}" \
WT_ORACLE_RECONNECT_REQUEST_ID="${reconnect_request_id}" \
  k6 run "${runtime_dir}/tests/webtransport_browser_oracle.js"

if [[ "${callgrind_turn_only}" == 1 ]]; then
  callgrind_control --instr=off "${gateway_pid}" >/dev/null
fi

wait "${peer_pid}"
peer_pid=
kill "${gateway_pid}"
set +e
wait "${gateway_pid}"
gateway_status=$?
set -e
gateway_pid=
if [[ ${gateway_status} -ne 0 ]]; then
  echo "gateway exited with status ${gateway_status}" >&2
  exit "${gateway_status}"
fi
grep -F "PASS WebTransport VBus oracle published canonical lifecycle, stopped PCM at a request-bound endpoint, drained its tail, reused one transport, exercised datagram conflict, and reallocated its routing slot" \
  "${test_dir}/peer.log" >/dev/null
grep -F "PASS native WebTransport D&D metadata, signed owner and premium, free RAG denial, and scope reset" \
  "${test_dir}/peer.log" >/dev/null
echo "PASS authenticated browser WebTransport endpoint feedback, tail drain, stream reuse, reconnect, datagram conflict, and fail-closed capabilities"
