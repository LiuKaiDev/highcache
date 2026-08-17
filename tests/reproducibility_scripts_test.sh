#!/usr/bin/env bash

set -euo pipefail

repo_root=${1:?repository root is required}

for script in build.sh run_server.sh run_benchmark.sh; do
  script_path="${repo_root}/scripts/${script}"
  test -x "${script_path}"
  bash -n "${script_path}"
  "${script_path}" --help >/dev/null
done
