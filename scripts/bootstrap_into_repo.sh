#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="${1:-/home/chaos/projects/highcache}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
STARTER_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

if [[ ! -d "${REPO_DIR}/.git" ]]; then
    echo "error: ${REPO_DIR} is not a Git repository" >&2
    echo "clone first: git clone git@github.com:LiuKaiDev/highcache.git ${REPO_DIR}" >&2
    exit 1
fi

if [[ -n "$(git -C "${REPO_DIR}" status --porcelain)" ]]; then
    echo "error: target repository has uncommitted changes; refusing to overwrite" >&2
    exit 1
fi

cp -a "${STARTER_DIR}/." "${REPO_DIR}/"
rm -f "${REPO_DIR}/scripts/bootstrap_into_repo.sh"

echo "Starter copied to ${REPO_DIR}"
echo "Next: cd ${REPO_DIR} && git status"
