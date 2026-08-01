#!/usr/bin/env bash
# Configure and build psxrecomp-game + psxrecomp-bios.
#
# Usage (from game repo root):
#   psxrecomp/tools/ci/build_emitters.sh \
#     [--framework psxrecomp] [--build-dir build-recompiler] [--jobs N]
set -euo pipefail

FRAMEWORK="psxrecomp"
BUILD_DIR="build-recompiler"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --framework) FRAMEWORK="${2:?}"; shift 2 ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --jobs) JOBS="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -d /mingw64/bin ]]; then
  export PATH="/mingw64/bin:${PATH}"
fi

if [[ ! -d "${FRAMEWORK}/recompiler" ]]; then
  echo "error: ${FRAMEWORK}/recompiler missing" >&2
  exit 1
fi

cmake -S "${FRAMEWORK}/recompiler" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target psxrecomp-game psxrecomp-bios -j"${JOBS}"

for bin in psxrecomp-bios psxrecomp-game; do
  if [[ -f "${BUILD_DIR}/${bin}.exe" ]]; then
    chmod +x "${BUILD_DIR}/${bin}.exe"
  elif [[ -f "${BUILD_DIR}/${bin}" ]]; then
    chmod +x "${BUILD_DIR}/${bin}"
  else
    echo "error: ${bin} missing after recompiler build" >&2
    exit 1
  fi
done

echo "emitters ready under ${BUILD_DIR}"
