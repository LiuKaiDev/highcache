#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
usage: ./scripts/run_server.sh [config-file]

Run HighCache in the foreground. Environment overrides:
  HIGHCACHE_BUILD_DIR  build directory (default: build-release)
  HIGHCACHE_CONFIG     config file when no argument is given
EOF
}

if [[ ${1:-} == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -gt 1 ]]; then
  usage >&2
  exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${HIGHCACHE_BUILD_DIR:-"${repo_root}/build-release"}
config=${1:-${HIGHCACHE_CONFIG:-"${repo_root}/config/highcache.conf.example"}}
server="${build_dir}/highcache_server"

if [[ ! -x "${server}" ]]; then
  echo "highcache_server is missing; run ./scripts/build.sh first" >&2
  exit 1
fi
if [[ ! -f "${config}" ]]; then
  echo "configuration file not found: ${config}" >&2
  exit 1
fi

exec "${server}" "${config}"
