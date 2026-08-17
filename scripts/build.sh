#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
usage: ./scripts/build.sh

Build HighCache with CMake. Environment overrides:
  HIGHCACHE_BUILD_DIR   build directory (default: build-release)
  HIGHCACHE_BUILD_TYPE  CMake build type (default: Release)
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
build_type=${HIGHCACHE_BUILD_TYPE:-Release}

cmake -S "${repo_root}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${build_type}"
cmake --build "${build_dir}" -j
