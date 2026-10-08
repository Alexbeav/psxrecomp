#!/usr/bin/env bash
# bios_emitter_fingerprint.sh — print a single content hash of every input that
# affects the BIOS generated C: the psxrecomp-bios emitter sources, the cycle
# model they bake in, and — per BIOS profile — the profile TOML itself, its
# seeds file, and the ROM image. regen_bios.sh writes this into
# generated/<stem>.emitter.sha after a regen; runtime/runtime.cmake recomputes
# it at configure time and warns if it differs — i.e. "you changed an emitter
# input but didn't regenerate the BIOS". Hashing the ROM and profile means
# swapping the BIOS image or editing its profile trips the staleness warning
# too, not just emitter-source edits.
#
# Usage: bios_emitter_fingerprint.sh [--list] [--hand-list] [profile.toml]
#   default profile: bios/SCPH1001.toml
#   --list       print the files in place of the hash, one line each, in the
#                order they are hashed: <kind> TAB <present|missing> TAB <path>
#   --hand-list  use the hand-kept file list of pin H and before. It exists to
#                judge a stamp that was written with that list, and for nothing
#                else: see runtime/bios_stale_check.cmake.
#
# WHERE THE FILE LIST COMES FROM (PS1B-131)
# The emitter sources are read from add_executable(psxrecomp-bios ...) in
# recompiler/CMakeLists.txt. The headers are the quoted #include lines of those
# sources, followed through this repository. Nothing here names an emitter
# file, so the list cannot fall behind the target.
#
# Until pin H the list was kept by hand, and it was wrong both ways. Three
# sources of the target were not hashed (pgxp_hook_emitter.cpp,
# ps1_exe_parser.cpp, recompiler_patch.cpp): a change to one of them moved no
# fingerprint. Three files the target does not compile were hashed
# (basic_block.cpp, control_flow.cpp, function_analysis.cpp): a change to the
# game emitter cried "BIOS is STALE" about a BIOS that could not have changed.
# recompiler/tests/test_bios_emitter_fingerprint.py holds this script to the
# target, and to the include lines, in both directions.
#
# The rom/seeds extraction expects simple `key = "value"` lines, which is how
# the tracked profiles are written.
#
# Written for bash 3.2 and the tools of a plain macOS as well: no associative
# arrays, no mapfile, no GNU-only sed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE=fingerprint
DEFINITION=target
while [ "$#" -gt 0 ]; do
    case "$1" in
        --list)      MODE=list; shift ;;
        --hand-list) DEFINITION=hand; shift ;;
        --)          shift; break ;;
        -*)          echo "bios_emitter_fingerprint: unknown option $1" >&2; exit 2 ;;
        *)           break ;;
    esac
done
PROFILE="${1:-bios/SCPH1001.toml}"

CMAKE_LISTS="recompiler/CMakeLists.txt"
TARGET="psxrecomp-bios"
# The folders the target puts on its include path, without the vendored
# libraries under recompiler/lib.
INCLUDE_DIRS="recompiler/include recompiler/src"
# Inputs that no #include of the emitter reaches. psx_cyc.h holds the cycle
# helpers that the generated C is written against.
EXTRA_FILES="runtime/include/psx_cyc.h"

# The list as it was kept by hand through pin H. Frozen: do not add to it.
HAND_LIST="
    recompiler/src/full_function_emitter.cpp
    recompiler/src/full_function_emitter.h
    recompiler/src/strict_translator.cpp
    recompiler/src/main_bios.cpp
    recompiler/src/function_discovery.cpp
    recompiler/src/bios_address_model.cpp
    recompiler/src/bios_address_model.h
    recompiler/src/config_loader.cpp
    recompiler/src/control_flow.cpp
    recompiler/src/function_analysis.cpp
    recompiler/src/mips_decoder.cpp
    recompiler/src/bios_slice_walker.cpp
    recompiler/src/basic_block.cpp
    runtime/include/psx_cyc.h
    runtime/include/psx_instr_cost.h
"

# awk: "a/b/../c/./d" -> "a/c/d". Shared by the two readers below.
AWK_NORM='
function norm(path,    n, part, i, top, out, stack) {
    n = split(path, part, "/")
    top = 0
    for (i = 1; i <= n; i++) {
        if (part[i] == "." || (part[i] == "" && i > 1)) continue
        if (part[i] == ".." && top > 0 && stack[top] != "..") { top--; continue }
        stack[++top] = part[i]
    }
    out = ""
    for (i = 1; i <= top; i++) out = out (i > 1 ? "/" : "") stack[i]
    return out
}'

# Sources of the target, in the order CMake lists them: the words between
# "add_executable(psxrecomp-bios" and its closing parenthesis. A word that is
# not a plain file name (a variable, a generator expression) cannot be read
# here, and that is an error, not an empty list.
target_sources() {
    awk -v target="$TARGET" "$AWK_NORM"'
    {
        line = $0
        sub(/\r$/, "", line)
        sub(/#.*$/, "", line)
        if (!inside) {
            if (line !~ ("add_executable[ \t]*[(][ \t]*" target "([ \t)]|$)")) next
            sub(("^.*add_executable[ \t]*[(][ \t]*" target), "", line)
            inside = 1
        }
        closing = index(line, ")")
        if (closing) line = substr(line, 1, closing - 1)
        n = split(line, word, /[ \t]+/)
        for (i = 1; i <= n; i++) {
            if (word[i] == "") continue
            if (word[i] ~ /[$<>"]/) {
                print "bios_emitter_fingerprint: " target " lists \"" word[i] "\", which is not a plain file name" > "/dev/stderr"
                failed = 1
                exit
            }
            print norm("recompiler/" word[i])
            found++
        }
        if (closing) exit
    }
    END {
        if (failed) exit 3
        if (!found) {
            print "bios_emitter_fingerprint: no sources of " target " found in '"$CMAKE_LISTS"'" > "/dev/stderr"
            exit 3
        }
    }' "$CMAKE_LISTS"
}

# Headers of the target: each file a source names in a quoted #include, and
# each file those name, as far as it is in this repository. An include is
# looked for beside the file that names it, then in INCLUDE_DIRS. Reads the
# sources on stdin and prints the headers, sorted so that the order does not
# depend on the order of the include lines. The sort is done here, in the C
# locale: on Windows a plain `sort` can be the one of System32.
target_headers() {
    LC_ALL=C awk -v dirs="$INCLUDE_DIRS" "$AWK_NORM"'
    function readable(path,    line, status) {
        status = (getline line < path)
        close(path)
        return status >= 0
    }
    BEGIN {
        ndirs = split(dirs, dir, " ")
        head = 0
        tail = 0
        while ((getline source) > 0) {
            if (source == "") continue
            queue[++tail] = source
            seen[source] = 1
        }
        while (head < tail) {
            file = queue[++head]
            base = file
            sub(/[^\/]*$/, "", base)
            while ((getline line < file) > 0) {
                if (line !~ /^[ \t]*#[ \t]*include[ \t]*"/) continue
                name = line
                sub(/^[ \t]*#[ \t]*include[ \t]*"/, "", name)
                sub(/".*$/, "", name)
                found = ""
                for (i = 0; i <= ndirs && found == ""; i++) {
                    candidate = norm((i == 0 ? base : dir[i] "/") name)
                    if (candidate ~ /^\.\.(\/|$)/) continue
                    if ((candidate in seen) || readable(candidate)) found = candidate
                }
                if (found == "" || (found in seen)) continue
                seen[found] = 1
                queue[++tail] = found
                header[++headers] = found
            }
            close(file)
        }
        for (i = 2; i <= headers; i++) {
            moved = header[i]
            for (j = i - 1; j >= 1 && header[j] > moved; j--) header[j + 1] = header[j]
            header[j + 1] = moved
        }
        for (i = 1; i <= headers; i++) print header[i]
    }'
}

toml_value() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$PROFILE" | head -1
}

TAB="$(printf '\t')"
NL='
'
# FILES holds one "<kind> TAB <path>" line for each input, in hashing order.
FILES=""
add() {
    kind="$1"
    shift
    for path in "$@"; do
        if [ -n "$path" ]; then FILES="${FILES}${kind}${TAB}${path}${NL}"; fi
    done
}

if [ "$DEFINITION" = hand ]; then
    add hand $HAND_LIST
elif [ -f "$CMAKE_LISTS" ]; then
    SOURCES="$(target_sources)"
    HEADERS="$(printf '%s\n' "$SOURCES" | target_headers)"
    add source $SOURCES
    add header $HEADERS
    add extra $EXTRA_FILES
fi
# Without recompiler/CMakeLists.txt (an install that carries built emitters
# only) there is no emitter source to hash: the profile inputs are the list.

# Per-profile inputs: the profile itself, its seeds file, its ROM.
if [ -f "$PROFILE" ]; then
    add profile "$PROFILE" "$(toml_value seeds)" "$(toml_value rom)"
else
    # Legacy fallback (pre-profile invocations): the historical seeds path.
    add profile recompiler/seeds/phase2_ghidra_seeds.json
fi

# Hash only files that exist: a tree can lack the ROM.
present=()
while IFS="$TAB" read -r kind path; do
    [ -n "$kind" ] || continue
    if [ -f "$path" ]; then
        present+=("$path")
        state=present
    else
        state=missing
    fi
    if [ "$MODE" = list ]; then printf '%s\t%s\t%s\n' "$kind" "$state" "$path"; fi
done <<EOF
$FILES
EOF
if [ "$MODE" = list ]; then exit 0; fi

# One digest over the digests of the file CONTENTS, in the order of FILES. No
# path goes into it: regen_bios.sh records with a relative profile path while
# runtime.cmake recomputes with an absolute one, and a digest that held the
# spelling read as STALE at every configure of a byte-identical tree.
if [ "${#present[@]}" -eq 0 ]; then
    printf '' | sha256sum | awk '{print $1}'
elif [ "$DEFINITION" = hand ]; then
    # The form of pin H, kept exactly: one sha256sum of stdin for each file.
    for f in "${present[@]}"; do
        sha256sum < "$f"
    done | sha256sum | awk '{print $1}'
else
    # One sha256sum for all files. It prints "<digest>  <path>", and puts a
    # backslash before the digest when the path holds one; only the digest is
    # kept.
    sha256sum -- "${present[@]}" | awk '{ digest = $1; sub(/^\\/, "", digest); print digest }' \
        | sha256sum | awk '{print $1}'
fi
