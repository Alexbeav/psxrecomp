# Windows toolchain traps

Failure modes specific to building and testing this project on a Windows host. They share one
shape: **a wrapper reports a verdict that disagrees with the thing it wrapped.** A process that
could not start, a decoder that could not decode, or a matcher that found nothing all produce
output that reads like a genuine finding about the code. Every one of these cost real debugging
time before it was recognised.

If a failure here looks like a product defect, check this list before believing it.

## A binary that cannot load reads as a stale binary

`psxrecomp-game` links `libstdc++-6.dll` and `libgcc_s_seh-1.dll`. If a different toolchain's
copy appears earlier on `PATH` than the one it was built against, the image never loads: Windows
exits with an NTSTATUS and the process writes nothing to either stream.

    0xC0000135  STATUS_DLL_NOT_FOUND
    0xC0000139  STATUS_ENTRYPOINT_NOT_FOUND   (DLL found, wrong vintage)
    0xC0000142  STATUS_DLL_INIT_FAILED

**The exit code has more than one spelling, and searching for the wrong one hides half the
cases.** The same binary, the same build, only `PATH` differing:

    correct toolchain first      exit 0, prints usage
    foreign libstdc++-6.dll first, launched from a native shell
                                 exit -1073741511  (0xC0000139 as a signed int)
    the same, launched from Git Bash
                                 exit 127

Git Bash reports it as 127, which reads as "command not found" and sends the reader looking for a
missing file rather than a shadowed DLL. Both spellings print nothing on either stream. Grep the
symptom, not the number: **a non-zero exit with both streams empty.**

Note also what this failure mode *cannot* be: it makes tests fail, never pass. A green suite is
never a symptom of it, so it can invalidate a red run and never a clean one.

A caller checking only "non-zero exit, no output" cannot tell that from a binary too old to
understand the flag it was asked about. `tools/compile_overlays.py` used to report it as
staleness and advise a rebuild, which cannot help — the binary was current, the launcher was
wrong. It now calls `loader_failure_detail()`, which names the status and lists each non-system
DLL the image imports beside the file `PATH` resolves it to, so shadowing is visible.

Observed with MSYS2's `libstdc++-6.dll` (GCC 13-era) ahead of a WinLibs GCC 16 build.

**Rule:** a test harness must never put a compiler's `bin` directory *ahead* of the runtime the
recompiler was built against. Pass the compiler by absolute path and **append** its directory:
Windows resolves a spawned image's DLLs from its own directory first, so appending costs the
compile nothing.

## Subprocess output decoded with the host codepage

`subprocess.run(..., text=True)` with no `encoding=` decodes the child's output using the host
ANSI codepage. On any non-Latin-1 Windows host, a byte the codepage cannot map raises
`UnicodeDecodeError` — inside `subprocess`'s reader thread, so the traceback appears without an
obvious owner and the capture is lost mid-build.

Observed on a cp1253 (Greek) host: byte `0x9c` in ordinary GCC diagnostics.

**Rule:** always pass `encoding='utf-8', errors='replace'` when capturing subprocess output.
A bare `text=True` is a portability defect, not a style preference. It is invisible on the Linux
CI runners and on English-locale Windows, so it reaches only contributors outside those locales
and survives review indefinitely.

## `MSYS_NO_PATHCONV` is scoped to ref arguments

Under Git Bash, `git show <ref>:<path>` can return empty because the shell rewrites the `<path>`
half of the pair. `MSYS_NO_PATHCONV=1` fixes that, and breaks anything that takes a real path:

    MSYS_NO_PATHCONV=1 git worktree add /i/Projects/x   # creates I:\i\Projects\x
    MSYS_NO_PATHCONV=1 cmake -S /c/...                  # "directory does not exist"

**Rule:** scope it to the single `git show` invocation. Never export it across a block that also
passes paths to `cd`, `cmake -S/-B`, or `git worktree add`. Split a command that needs both kinds
of argument.

## ctest counts unbuilt targets as failures

Building a single target and then running the whole suite reports every test whose executable was
never built as `***Not Run`, and counts it toward the failure total — a partial build can read as
a catastrophically broken tree:

    Could not find executable .../control_flow_metadata_test.exe
    1/192 Test #1: control_flow_metadata_test ...***Not Run

**Rule:** before quoting a suite number, build every target and confirm `Not Run: 0`. A run with
non-zero `Not Run` is not a measurement of the tree.

**Second half of the same rule, from a run that was quoted before it was earned:** also confirm
the recompiler actually loads, because a suite whose spawned binary cannot start reports a
plausible pass rate rather than an error. The cheap gate, before ctest:

    psxrecomp-game --help   # must exit 0 and print usage

Without it, 2026-09-18 produced `84% tests passed, 30 tests failed out of 189` — a number with a
percentage, a denominator and no meaning at all, because every test that spawns the recompiler
died at image load. With the gate, the same tree measured 0 of 189.

Three tests are `DISABLED` deliberately (`recompiler/CMakeLists.txt`) and report as
`Not Run (Disabled)`. Those are expected and are not breakage.

## A hand-rolled hash of CMake-read files is wrong on a CRLF checkout

`PSX_OVERLAY_CODEGEN_HASH` is SHA-256 over the files in
`runtime/codegen_hash_sources.cmake`, concatenated in order, first 8 hex characters
(`runtime/hash_codegen.cmake`). CMake's `file(READ)` **normalises CRLF to LF**. A script that
reads the same files as raw bytes on a CRLF checkout therefore hashes something else — and still
returns eight plausible hex characters that compare cleanly against nothing.

Reproducing it by hand produced `7af52ab7`, then `91f42d0d`, before the correct `80dd9a27`. Both
wrong values looked exactly as much like a hash as the right one.

**Rule:** use `codegen_hash.py`, which normalises the way `file(READ)` does, or the built
binary's `--codegen-hash`, which is authoritative. Never hand-roll it. And when checking a tree
you have not built, **run the script first against a tree whose hash is already known** — a
control run is the only thing separating a correct implementation from a confidently wrong one.

Worked example, 2026-09-18: control `cdc88fb44` → `ef02e740`, already known, so the same script
at `65730fc70` → `80dd9a27` could be believed. Without the control, `80dd9a27` would have been
a number that merely looked right.

## Known-issue notes decay into permanent excuses

`aot_overlay_discovery` carried a note attributing its failure to a missed `cwd` in
`compile_fragment_batch`. That call had passed `cwd` for a month before the note was written. The
note was wrong on the day it was added, and for five weeks it gave every reader a confident
explanation for a failure that was really the two bugs above — so nobody looked further.

**Rule:** a known-issue note needs the date it was verified and the evidence it rests on. Without
those it becomes a standing excuse for whatever fails next under that test's name. The response
to a red test is never to remove its registration; use ctest's `DISABLED` property, which keeps
it counted and visible.
