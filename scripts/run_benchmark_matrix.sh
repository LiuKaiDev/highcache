#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${HIGHCACHE_BUILD_DIR:-"${repo_root}/build-release"}
results_file=${HIGHCACHE_RESULTS_FILE:-"${repo_root}/benchmark/results/benchmark_results.csv"}
host=${HIGHCACHE_HOST:-127.0.0.1}
port=${HIGHCACHE_PORT:-11211}
server_workers=${HIGHCACHE_SERVER_WORKERS:-4}
connections=${HIGHCACHE_CONNECTIONS:-128}
requests=${HIGHCACHE_REQUESTS:-1000000}
key_space=${HIGHCACHE_KEY_SPACE:-100000}
seed=${HIGHCACHE_SEED:-12345}
warmup_requests=${HIGHCACHE_WARMUP_REQUESTS:-10000}
pipeline=${HIGHCACHE_PIPELINE:-1}
capacity_bytes=${HIGHCACHE_CAPACITY_BYTES:-1073741824}
repetitions=${HIGHCACHE_REPETITIONS:-3}
code_label=${HIGHCACHE_CODE_LABEL:-before}
mode=${1:-baseline}

server="${build_dir}/highcache_server"
client="${build_dir}/highcache_benchmark"
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

if [[ ! -x "${server}" || ! -x "${client}" ]]; then
  echo "Release server and benchmark binaries are required in ${build_dir}" >&2
  exit 1
fi

mkdir -p "$(dirname "${results_file}")"

write_header() {
  echo 'experiment,variant,repetition,code_label,allocator,shards,server_workers,requests,successful,failed,client_threads,connections,value_size,get_ratio,duration_seconds,throughput_rps,avg_us,p50_us,p95_us,p99_us,get_hits,get_misses,hit_ratio,key_space,seed,pipeline,warmup_requests,error_count,server_cpu_seconds,server_cpu_percent,server_rss_kb,logical_cache_bytes,allocator_reserved_bytes,allocator_used_bytes,allocator_fragmentation_bytes,allocator_allocations,allocator_deallocations,allocator_slabs' >"${results_file}"
}

wait_for_server() {
  for _ in $(seq 1 200); do
    if ! kill -0 "${server_pid}" 2>/dev/null; then
      return 1
    fi
    if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
      exec 3>&-
      return 0
    fi
    sleep 0.025
  done
  return 1
}

metric_from_log() {
  local name=$1
  local log_file=$2
  sed -n "s/.*${name}=\([0-9][0-9]*\).*/\1/p" "${log_file}" | tail -n 1
}

run_point() {
  local experiment=$1
  local variant=$2
  local repetition=$3
  local allocator=$4
  local shards=$5
  local client_threads=$6
  local get_ratio=$7
  local value_size=$8
  local configuration="${temporary_dir}/server.conf"
  local server_log="${temporary_dir}/server.log"
  local client_csv="${temporary_dir}/client.csv"

  {
    echo 'log_level=info'
    echo "host=${host}"
    echo "port=${port}"
    echo "worker_threads=${server_workers}"
    echo "cache_capacity_bytes=${capacity_bytes}"
    echo "shard_count=${shards}"
    echo "allocator=${allocator}"
  } >"${configuration}"

  "${server}" "${configuration}" >"${server_log}" 2>&1 &
  server_pid=$!
  if ! wait_for_server; then
    echo "server failed to become ready for ${experiment}/${variant}" >&2
    sed -n '1,120p' "${server_log}" >&2
    return 1
  fi

  local clock_ticks
  local cpu_start
  local wall_start
  clock_ticks=$(getconf CLK_TCK)
  cpu_start=$(awk '{print $14 + $15}' "/proc/${server_pid}/stat")
  wall_start=$(date +%s%N)

  "${client}" \
    --host "${host}" \
    --port "${port}" \
    --threads "${client_threads}" \
    --connections "${connections}" \
    --requests "${requests}" \
    --get-ratio "${get_ratio}" \
    --value-size "${value_size}" \
    --key-space "${key_space}" \
    --seed "${seed}" \
    --warmup-requests "${warmup_requests}" \
    --pipeline "${pipeline}" \
    --csv >"${client_csv}"

  local wall_end
  local cpu_end
  local rss_kb
  wall_end=$(date +%s%N)
  cpu_end=$(awk '{print $14 + $15}' "/proc/${server_pid}/stat")
  rss_kb=$(awk '/^VmRSS:/ {print $2}' "/proc/${server_pid}/status")

  kill -TERM "${server_pid}"
  wait "${server_pid}"
  server_pid=

  local cpu_seconds
  local cpu_percent
  cpu_seconds=$(awk -v start="${cpu_start}" -v end="${cpu_end}" \
    -v hz="${clock_ticks}" 'BEGIN {printf "%.6f", (end-start)/hz}')
  cpu_percent=$(awk -v start="${cpu_start}" -v end="${cpu_end}" \
    -v hz="${clock_ticks}" -v begin="${wall_start}" -v finish="${wall_end}" \
    'BEGIN {elapsed=(finish-begin)/1000000000; if (elapsed > 0) printf "%.3f", ((end-start)/hz)/elapsed*100; else print "0.000"}')

  local logical_bytes
  local reserved_bytes
  local used_bytes
  local fragmentation_bytes
  local allocations
  local deallocations
  local slabs
  logical_bytes=$(metric_from_log logical_bytes "${server_log}")
  reserved_bytes=$(metric_from_log allocator_reserved_bytes "${server_log}")
  used_bytes=$(metric_from_log allocator_used_bytes "${server_log}")
  fragmentation_bytes=$(metric_from_log allocator_fragmentation_bytes "${server_log}")
  allocations=$(metric_from_log allocator_allocations "${server_log}")
  deallocations=$(metric_from_log allocator_deallocations "${server_log}")
  slabs=$(metric_from_log allocator_slabs "${server_log}")

  local client_row
  client_row=$(tail -n 1 "${client_csv}")
  echo "${experiment},${variant},${repetition},${code_label},${allocator},${shards},${server_workers},${client_row},${cpu_seconds},${cpu_percent},${rss_kb},${logical_bytes},${reserved_bytes},${used_bytes},${fragmentation_bytes},${allocations},${deallocations},${slabs}" >>"${results_file}"
  echo "completed ${experiment}/${variant} repetition ${repetition}"
}

repeat_point() {
  local experiment=$1
  local variant=$2
  local allocator=$3
  local shards=$4
  local client_threads=$5
  local get_ratio=$6
  local value_size=$7
  local count=${8:-${repetitions}}
  for repetition in $(seq 1 "${count}"); do
    run_point "${experiment}" "${variant}" "${repetition}" "${allocator}" \
      "${shards}" "${client_threads}" "${get_ratio}" "${value_size}"
  done
}

case "${mode}" in
baseline)
  write_header
  repeat_point workload A slab 64 8 1.0 256
  repeat_point workload B slab 64 8 0.8 256
  repeat_point workload C slab 64 8 0.5 256

  for shards in 1 4 16 32 64 128; do
    repeat_point shard_count "shards-${shards}" slab "${shards}" 8 0.8 256
  done

  for allocator in system slab; do
    repeat_point allocator "${allocator}-64B" "${allocator}" 64 8 0.5 64 1
    repeat_point allocator "${allocator}-256B" "${allocator}" 64 8 0.5 256
    repeat_point allocator "${allocator}-1024B" "${allocator}" 64 8 0.5 1024 1
    repeat_point allocator "${allocator}-4096B" "${allocator}" 64 8 0.5 4096 1
  done

  for client_threads in 1 2 4 8; do
    repeat_point thread_scaling "threads-${client_threads}" slab 64 \
      "${client_threads}" 0.8 256
  done
  ;;
optimization)
  if [[ ! -f "${results_file}" ]]; then
    write_header
  fi
  repeat_point optimization after slab 64 8 0.8 256
  ;;
smoke)
  write_header
  requests=${HIGHCACHE_REQUESTS:-10000}
  key_space=${HIGHCACHE_KEY_SPACE:-1000}
  warmup_requests=${HIGHCACHE_WARMUP_REQUESTS:-1000}
  repeat_point smoke smoke slab 64 2 0.8 256 1
  ;;
*)
  echo "usage: $0 [baseline|optimization|smoke]" >&2
  exit 2
  ;;
esac
