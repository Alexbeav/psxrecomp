#!/usr/bin/env bash
# Reject developer-machine paths from a staged setup package.
set -euo pipefail

if [[ "$#" -ne 1 || ! -d "$1" ]]; then
  echo "usage: check_private_paths.sh STAGED_PACKAGE_ROOT" >&2
  exit 2
fi

stage="$1"
hits="$(grep -rIEn \
  -e '[A-Za-z]:[\\/](Users|Projects|AgentData|OneDrive|Share)[\\/]' \
  -e '\\\\([0-9]{1,3}\.){3}[0-9]{1,3}[\\/]' \
  -e '/mnt/[A-Za-z]/' \
  "${stage}" 2>/dev/null \
  | grep -Ev 'C:[\\/]Users[\\/](You|username|\.\.\.)[\\/]' \
  | grep -Ev 'C:[\\/]Users[\\/]\.\.\.([^A-Za-z0-9_]|$)' \
  || true)"

if [[ -n "${hits}" ]]; then
  echo "error: staged source contains a developer-machine path:" >&2
  printf '%s\n' "${hits}" >&2
  exit 1
fi
