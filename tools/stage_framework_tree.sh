#!/usr/bin/env bash
# stage_framework_tree.sh — copy the psxrecomp framework tree into a release
# package, exactly as a shipped game kit carries it.
#
# WHY THIS IS ITS OWN SCRIPT
# --------------------------
# A kit rebuilds the emitters from this copy (`psxrecomp_cli.py
# ensure-emitters`), so every file dropped here is a file the kit cannot get
# back. That makes the filter list load-bearing build configuration, not
# packaging cosmetics — and it broke twice at once on the 2026-09-17 Parasite
# Eve macOS kit:
#
#   * `--exclude 'generated'` had no leading slash. rsync matches an unanchored
#     pattern at EVERY depth, so besides the intended top-level generated/
#     (recompiler output) it also deleted the vendored
#     recompiler/lib/rabbitizer/include/generated/ and
#     recompiler/lib/rabbitizer/cplusplus/include/generated/ — tracked headers
#     that RabbitizerInstrId.h includes. The kit could not compile rabbitizer.
#     The cp fallback below never had this bug: its removal was already
#     anchored to "${dest}/generated". Only rsync hosts shipped broken kits,
#     which is why this went unnoticed on Windows.
#   * tools/tasreplays is dropped (developer tree) while runtime/tests IS
#     shipped, so the three tests registered only there looked unregistered to
#     runtime/check_test_registration.cmake and the kit could not configure.
#     That half is fixed in the guard, which now knows those registrations live
#     in a build file source packages do not ship.
#
# Both failures are invisible in the framework repo and only appear when a kit
# is rebuilt, so .github/workflows/kit-emitter-rebuild.yml stages with THIS
# script and then builds psxrecomp-game from the result.
#
# Usage:
#   stage_framework_tree.sh --framework <psxrecomp-src> --dest <stage>/psxrecomp
set -euo pipefail

SRC=""
DEST=""

usage() {
  sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --framework) SRC="${2:?}"; shift 2 ;;
    --dest) DEST="${2:?}"; shift 2 ;;
    *) echo "error: unknown arg: $1" >&2; usage ;;
  esac
done

if [[ -z "${SRC}" || -z "${DEST}" ]]; then
  echo "error: --framework and --dest are both required" >&2
  usage
fi
if [[ ! -d "${SRC}" ]]; then
  echo "error: --framework not a directory: ${SRC}" >&2
  exit 1
fi
SRC="$(cd "${SRC}" && pwd)"

mkdir -p "${DEST}"
DEST="$(cd "${DEST}" && pwd)"

# Out-of-tree build directories and Python caches are litter at any depth, so
# those patterns stay unanchored. generated/ is NOT litter at any depth: only
# the framework root's generated/ is recompiler output. Anchor it.
if command -v rsync >/dev/null 2>&1; then
  rsync -a \
    --exclude '.git' \
    --exclude '/generated' \
    --exclude 'recompiler/build' \
    --exclude '__pycache__' \
    --exclude 'build' \
    --exclude 'build-*' \
    "${SRC}/" "${DEST}/"
else
  cp -a "${SRC}/." "${DEST}/"
  rm -rf "${DEST}/.git" "${DEST}/recompiler/build" "${DEST}/generated" 2>/dev/null || true
  find "${DEST}" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
  find "${DEST}" -type d \( -name 'build' -o -name 'build-*' \) -prune -exec rm -rf {} + 2>/dev/null || true
fi

# Developer notes, one-off capture helpers, and dependency test fixtures are
# not setup SDK inputs. Some contain paths from their authors' workstations.
# Keep those paths out of a public source package on both routes above.
rm -rf \
  "${DEST}/CLAUDE.md" \
  "${DEST}/docs/internal" \
  "${DEST}/docs/STRING_TRANSLATION.md" \
  "${DEST}/recompiler/lib/ELFIO/tests" \
  "${DEST}/tools/aot_overlay_spike" \
  "${DEST}/tools/tasreplays" \
  "${DEST}/tools/audio_capture_ab.py" \
  "${DEST}/tools/launch_tomba2_interp_perf.ps1"

# The emitters are built from this copy, so a header the build includes must
# not depend on the filter list being right. Check the vendored generated
# headers directly: they are the ones an unanchored exclude eats, and their
# absence surfaces only as a compile error deep in a kit rebuild.
missing=0
for required in \
  "recompiler/lib/rabbitizer/include/generated/InstrId_enum.h" \
  "recompiler/lib/rabbitizer/include/generated/InstrDescriptor_Descriptors_array.h" \
  "recompiler/lib/rabbitizer/cplusplus/include/generated/UniqueId_enum_class.hpp"
do
  if [[ ! -f "${DEST}/${required}" ]]; then
    echo "error: staged framework tree is missing ${required}" >&2
    missing=1
  fi
done
if [[ "${missing}" -ne 0 ]]; then
  echo "  These are tracked sources, not build output. A kit rebuilt from this" >&2
  echo "  tree cannot compile rabbitizer. Check the exclude list above." >&2
  exit 1
fi

echo "staged framework tree: ${SRC} -> ${DEST}"
