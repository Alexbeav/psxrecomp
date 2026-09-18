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

Three tests are `DISABLED` deliberately (`recompiler/CMakeLists.txt`) and report as
`Not Run (Disabled)`. Those are expected and are not breakage.

## Known-issue notes decay into permanent excuses

`aot_overlay_discovery` carried a note attributing its failure to a missed `cwd` in
`compile_fragment_batch`. That call had passed `cwd` for a month before the note was written. The
note was wrong on the day it was added, and for five weeks it gave every reader a confident
explanation for a failure that was really the two bugs above — so nobody looked further.

**Rule:** a known-issue note needs the date it was verified and the evidence it rests on. Without
those it becomes a standing excuse for whatever fails next under that test's name. The response
to a red test is never to remove its registration; use ctest's `DISABLED` property, which keeps
it counted and visible.
