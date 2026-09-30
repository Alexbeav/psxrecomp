#!/usr/bin/env bash
# Emit the OpenBIOS backend C in CI so a bundled release links a BIOS.
#
# A bundled release compiles the title's COMMITTED game C, but BIOS C lives in
# the framework submodule's generated/ (psxrecomp/generated/), which a title
# cannot commit. OpenBIOS is MIT and its image ships in psxrecomp/bios/, so CI
# regenerates that one backend from the emitter it just built. Retail BIOS
# backends are never produced here: their C derives from a Sony image and must
# not reach a public artifact (tools/ci/check_generated.sh refuses them).
#
# This is the same emission tools/regen_bios.sh performs locally, minus that
# script's "rebuild the emitter first" step (CI has just built or restored it)
# and its search for a recompiler build dir under the framework root (CI
# builds emitters out of tree, at <game root>/build-recompiler).
#
# Usage (from game repo root):
#   psxrecomp/tools/ci/generate_openbios.sh [--framework psxrecomp] \
#       [--recompiler-build build-recompiler]
set -euo pipefail

FRAMEWORK="psxrecomp"
RECOMPILER_BUILD="build-recompiler"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --framework) FRAMEWORK="${2:?}"; shift 2 ;;
    --recompiler-build) RECOMPILER_BUILD="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

[[ -d "${FRAMEWORK}" ]] || { echo "error: framework dir missing: ${FRAMEWORK}" >&2; exit 1; }
FRAMEWORK="$(cd "${FRAMEWORK}" && pwd)"
[[ -d "${RECOMPILER_BUILD}" ]] || { echo "error: recompiler build dir missing: ${RECOMPILER_BUILD}" >&2; exit 1; }
RECOMPILER_BUILD="$(cd "${RECOMPILER_BUILD}" && pwd)"

EMITTER=""
for cand in "${RECOMPILER_BUILD}/psxrecomp-bios" "${RECOMPILER_BUILD}/psxrecomp-bios.exe" \
            "${RECOMPILER_BUILD}/Release/psxrecomp-bios.exe"; do
  if [[ -f "${cand}" ]]; then EMITTER="${cand}"; break; fi
done
[[ -n "${EMITTER}" ]] || { echo "error: psxrecomp-bios not found under ${RECOMPILER_BUILD} (run build_emitters.sh)" >&2; exit 1; }

PROFILE="bios/OpenBIOS.toml"
for req in "${PROFILE}" bios/openbios.bin bios/OpenBIOS.LICENSE tools/bios_emitter_fingerprint.sh; do
  [[ -f "${FRAMEWORK}/${req}" ]] || { echo "error: ${FRAMEWORK}/${req} missing" >&2; exit 1; }
done

toml_value() {
  sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\([^\"]*\)\".*/\1/p" "${FRAMEWORK}/${PROFILE}" | head -1
}
OUT="$(toml_value out_dir)"; OUT="${OUT:-generated}"
STEM="$(toml_value out_stem)"
[[ -n "${STEM}" ]] || { echo "error: no out_stem in ${PROFILE}" >&2; exit 1; }

# The profile's rom / seeds paths are framework-relative, so run from there.
mkdir -p "${FRAMEWORK}/${OUT}"
echo "generate_openbios: ${EMITTER} --config ${PROFILE} (stem ${STEM}) -> ${FRAMEWORK}/${OUT}"
( cd "${FRAMEWORK}" && "${EMITTER}" --config "${PROFILE}" )

# Same fingerprint regen_bios.sh records, so runtime.cmake's staleness check
# sees a matching stamp instead of warning on every CI configure.
( cd "${FRAMEWORK}" && bash tools/bios_emitter_fingerprint.sh "${PROFILE}" > "${OUT}/${STEM}.emitter.sha" )

dispatch="${FRAMEWORK}/${OUT}/${STEM}_dispatch.c"
full="${FRAMEWORK}/${OUT}/${STEM}_full.c"
for f in "${dispatch}" "${full}"; do
  [[ -f "${f}" ]] || { echo "error: emitter produced no ${f}" >&2; exit 1; }
done
# runtime.cmake links a stem only when its descriptor is present; probe the
# same thing so a stale emitter fails here rather than as a silent skip.
if ! grep -q "${STEM}_psx_bios_backend" "${dispatch}"; then
  echo "error: ${dispatch} defines no ${STEM}_psx_bios_backend descriptor -- emitter predates the backend mechanism" >&2
  exit 1
fi
echo "generate_openbios: ok ($(grep -m1 -o 'Dispatch entries: [0-9]*' "${dispatch}" || echo 'descriptor present'))"
