#!/usr/bin/env bash
# Gate a bundled release on the committed game C being present and clean.
#
# Releases ship a compiled game: CI builds from the generated/ tree the title
# COMMITS, so a checkout missing it (or carrying the wrong marker name) must
# fail here, at the top of the job, not twenty minutes later at link -- and
# never on a player's machine, which is where the old setup-host model put
# every failure of this kind.
#
# Asserts, from the game repo root:
#   1. generated/<boot>_dispatch.c exists, where <boot> is the boot-EXE name
#      game.toml declares (the same name check_boot_exe.sh cross-checks), and
#      at least one generated/<boot>_full*.c shard sits beside it.
#   2. Those files are tracked by git. An untracked generated/ builds fine on
#      the developer's machine and is absent from every CI checkout, which is
#      the exact gap between "works for me" and a red release.
#   3. No retail-BIOS-derived C is present anywhere it could be linked:
#      generated/SCPH*_{dispatch,full}.c and psxrecomp/generated/SCPH*. Those
#      are derivatives of a copyrighted Sony image and must not reach a public
#      artifact. OpenBIOS is the only backend a release links, and CI emits it
#      from the bundled MIT image (tools/ci/generate_openbios.sh).
#   4. No BIOS dump is tracked (SCPH*.BIN / *.bin under bios/).
#
# Usage: check_generated.sh [--root DIR]    (default: cwd)
# Runs on bash 3.2+ (macOS runners).
set -euo pipefail

ROOT="${PWD}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done
ROOT="$(cd "${ROOT}" && pwd)"

toml_get() { # <section> <key> -- first quoted value of key inside [section]
  awk -v sec="$1" -v key="$2" '
    /^[[:space:]]*\[/ { in_sec = ($0 ~ ("^[[:space:]]*\\[" sec "\\]")) }
    in_sec && $0 ~ ("^[[:space:]]*" key "[[:space:]]*=") {
      if (match($0, /"[^"]*"/)) { print substr($0, RSTART + 1, RLENGTH - 2) }
      exit
    }' "${ROOT}/game.toml"
}

fail() { echo "error: $*" >&2; exit 1; }

[[ -f "${ROOT}/game.toml" ]] || fail "${ROOT}/game.toml missing -- not a game repo root"

boot="$(toml_get prepare_disc boot_exe || true)"
if [[ -z "${boot}" ]]; then
  exe="$(toml_get game exe || true)"
  boot="${exe##*/}"
fi
if [[ -z "${boot}" && -f "${ROOT}/CMakeLists.txt" ]]; then
  boot="$(sed -n 's/.*GEN_MARKER[[:space:]]*"generated\/\(.*\)_dispatch\.c".*/\1/p' \
    "${ROOT}/CMakeLists.txt" | head -n1)"
fi
[[ -n "${boot}" ]] || fail "cannot determine the boot-EXE name (game.toml [prepare_disc] boot_exe / [game] exe, or CMakeLists GEN_MARKER)"

gen="${ROOT}/generated"
dispatch="${gen}/${boot}_dispatch.c"

if [[ ! -f "${dispatch}" ]]; then
  echo "error: ${dispatch#"${ROOT}"/} is missing." >&2
  echo "  Bundled releases build from COMMITTED game C. Generate it once from a" >&2
  echo "  legal disc and commit generated/:" >&2
  echo "    python3 psxrecomp/psxrecomp_cli.py generate --config game.toml \\" >&2
  echo "        --project-root . --disc disc/<game>.cue" >&2
  echo "    git add generated && git commit -m 'Add generated game C'" >&2
  if grep -qE '^/?generated/?[[:space:]]*$' "${ROOT}/.gitignore" 2>/dev/null; then
    echo "  .gitignore still ignores generated/ (the old setup-host rule) -- remove" >&2
    echo "  that line; see psxrecomp/docs/ci/templates/game.gitignore." >&2
  fi
  exit 1
fi

shards=0
for f in "${gen}/${boot}_full"*.c; do
  [[ -f "${f}" ]] && shards=$((shards + 1))
done
[[ "${shards}" -gt 0 ]] || fail "no generated/${boot}_full*.c beside ${boot}_dispatch.c -- regenerate"

# 2. Tracked, not merely present.
if git -C "${ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  untracked="$(git -C "${ROOT}" ls-files --others --exclude-standard -- "generated/${boot}_dispatch.c" "generated/${boot}_full"*.c 2>/dev/null || true)"
  ignored="$(git -C "${ROOT}" ls-files --others --ignored --exclude-standard -- "generated/${boot}_dispatch.c" "generated/${boot}_full"*.c 2>/dev/null || true)"
  if [[ -n "${untracked}${ignored}" ]]; then
    echo "error: generated game C is present but NOT tracked by git:" >&2
    printf '%s\n' ${untracked} ${ignored} | sed 's/^/  /' >&2
    echo "  A release builds from the checkout, so these files never reach CI." >&2
    echo "  Remove any /generated/ rule from .gitignore, then: git add generated" >&2
    exit 1
  fi
fi

# 3. No retail-BIOS-derived C anywhere it could link.
retail=()
for d in "${gen}" "${ROOT}/psxrecomp/generated"; do
  [[ -d "${d}" ]] || continue
  for f in "${d}"/SCPH*_dispatch.c "${d}"/SCPH*_full.c "${d}"/SCPH*_full_*.c; do
    [[ -f "${f}" ]] && retail+=("${f#"${ROOT}"/}")
  done
done
if [[ "${#retail[@]}" -gt 0 ]]; then
  echo "error: retail-BIOS-derived C is present and would be linked into the release:" >&2
  printf '  %s\n' "${retail[@]}" >&2
  echo "  Only the OpenBIOS backend ships (CI emits it from the bundled MIT image)." >&2
  echo "  Delete these from the checkout; they must never be committed." >&2
  exit 1
fi

# 4. No BIOS dump tracked.
if git -C "${ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  dumps="$(git -C "${ROOT}" ls-files -- 'bios/*' '*.BIN' 'SCPH*' 2>/dev/null \
    | grep -Ei '\.(bin|rom)$' | grep -v -i 'openbios' || true)"
  if [[ -n "${dumps}" ]]; then
    echo "error: BIOS image(s) tracked in the repo:" >&2
    printf '%s\n' "${dumps}" | sed 's/^/  /' >&2
    exit 1
  fi
fi

echo "generated game C ok: generated/${boot}_dispatch.c + ${shards} full shard(s), tracked, no retail BIOS C"
