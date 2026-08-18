#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
usage: ./scripts/run_benchmark.sh

Build must already exist. This starts a temporary local server, runs a
representative mixed TCP workload, prints the result, and stops the server.

Environment overrides:
  HIGHCACHE_BUILD_DIR       build directory (default: build-release)
  HIGHCACHE_REQUESTS        measured requests (default: 1000000)
  HIGHCACHE_CLIENT_THREADS  client threads (default: 4)
  HIGHCACHE_CONNECTIONS     TCP connections (default: 128)
  HIGHCACHE_KEY_SPACE       preloaded keys (default: 100000)
  HIGHCACHE_WARMUP_REQUESTS warmup requests (default: 10000)
EOF
}

if [[ ${1:-} == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${HIGHCACHE_BUILD_DIR:-"${repo_root}/build-release"}
host=127.0.0.1
port=11211
requests=${HIGHCACHE_REQUESTS:-1000000}
client_threads=${HIGHCACHE_CLIENT_THREADS:-4}
connections=${HIGHCACHE_CONNECTIONS:-128}
key_space=${HIGHCACHE_KEY_SPACE:-100000}
warmup_requests=${HIGHCACHE_WARMUP_REQUESTS:-10000}
server="${build_dir}/highcache_server"
benchmark="${build_dir}/highcache_benchmark"
configuration="${repo_root}/benchmark/benchmark_server.conf"
temporary_dir=$(mktemp -d)
server_pid=

cleanup() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill -TERM "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
  rm -rf "${temporary_dir}"
}
trap cleanup EXIT

if [[ ! -x "${server}" || ! -x "${benchmark}" ]]; then
  echo "Release binaries are missing; run ./scripts/build.sh first" >&2
  exit 1
fi

"${server}" "${configuration}" >"${temporary_dir}/server.log" 2>&1 &
server_pid=$!

server_ready=false
for _ in $(seq 1 200); do
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    break
  fi
  if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
    exec 3>&-
    sleep 0.05
    if kill -0 "${server_pid}" 2>/dev/null; then
      server_ready=true
      break
    fi
  fi
  sleep 0.025
done

if [[ ${server_ready} != true ]]; then
  echo "temporary benchmark server failed to start" >&2
  sed -n '1,120p' "${temporary_dir}/server.log" >&2
  exit 1
fi

echo "Running 80% GET / 20% SET: ${requests} requests, ${connections} connections, ${client_threads} client threads"
"${benchmark}" \
  --host "${host}" \
  --port "${port}" \
  --threads "${client_threads}" \
  --connections "${connections}" \
  --requests "${requests}" \
  --get-ratio 0.8 \
  --value-size 256 \
  --key-space "${key_space}" \
  --seed 12345 \
  --warmup-requests "${warmup_requests}" \
  --pipeline 1
