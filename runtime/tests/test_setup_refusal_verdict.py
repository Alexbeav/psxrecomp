#!/usr/bin/env python3
"""Guard that the disc panel follows a file that setup refused (PS1B-415).

In a kit's setup window two checks judge the selected disc. The disc panel
asks the game program (ISO header and serial). "Generate & rebuild" asks the
CLI, which compares the data track with the kit and can refuse (exit code 3).
Nothing connected them: a first pressing with the kit's serial showed "Disc
verified" in the panel, and "Disc verification failed: ..." under it.

The setup host remembers the one file the CLI refused, and its wrapper around
the game program's disc check answers "bad" for that file. This test:

  1. compiles the rules (host/psx_setup_refusal.h) and runs them: only exit
     code 3 remembers a file; a success forgets it; another file forgets it;
  2. reads host/psxrecomp_codegen_host.c for the wiring: both CLI runners
     record the exit code, the generate callback applies the rules with it,
     the wrapper is installed over the game program's check, and what is
     remembered is memory of the process only.

The launcher's half (it asks again after a failed prepare) is tested in
recomp-ui, tests/launcher_setup_refusal_verdict_test.c.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = (ROOT / "host" / "psxrecomp_codegen_host.c").read_text(encoding="utf-8")

HARNESS = r"""
#include "psx_setup_refusal.h"
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main(void) {
    char held[64] = "";
    const char* disc = "D:/Games/Suikoden (USA).cue";

    /* Nothing remembered: every file is the game program's to judge. */
    CHECK(!psx_setup_refusal_at_check(held, disc));

    /* Only exit code 3 remembers a file. A build failure (1), a usage error
     * (2), a tool that could not be started (-1), a signal (139): no. */
    {
        static const long other[] = { 1, 2, 4, -1, 127, 139 };
        size_t i;
        for (i = 0; i < sizeof(other) / sizeof(other[0]); ++i) {
            psx_setup_refusal_after_prepare(held, sizeof(held), disc, other[i], 1);
            CHECK(held[0] == '\0');
            CHECK(!psx_setup_refusal_at_check(held, disc));
        }
    }
    /* Exit code 3 for ANOTHER disc: a game whose discs are separate programs
     * checks every disc of the set. The selected file is not marked. */
    psx_setup_refusal_after_prepare(held, sizeof(held), disc, PSX_SETUP_EXIT_VERIFY, 0);
    CHECK(held[0] == '\0' && !psx_setup_refusal_at_check(held, disc));

    /* What the CLI writes when it refuses the file given as --disc, and what
     * it writes for another disc of a set. Compact JSON, sorted keys. */
    CHECK(psx_setup_refusal_line_names_given(
        "{\"code\":3,\"event\":\"error\",\"message\":\"This is not the disc image this kit was made from.\","
        "\"refused_given_disc\":true,\"t\":1.204,\"verify_failed\":true}"));
    CHECK(!psx_setup_refusal_line_names_given(
        "{\"code\":3,\"event\":\"error\",\"message\":\"Disc 2 of Resident Evil 2 must be SLUS-00756\","
        "\"refused_given_disc\":false,\"t\":0.3,\"verify_failed\":true}"));
    CHECK(!psx_setup_refusal_line_names_given(      /* a CLI that does not say */
        "{\"code\":3,\"event\":\"error\",\"message\":\"wrong dump\",\"t\":0.3,\"verify_failed\":true}"));
    CHECK(!psx_setup_refusal_line_names_given(      /* not an error event */
        "{\"event\":\"log\",\"message\":\"x\",\"refused_given_disc\":true,\"t\":0.3}"));
    CHECK(!psx_setup_refusal_line_names_given(      /* the words inside a message are text */
        "{\"code\":1,\"event\":\"error\",\"message\":\"saw \\\"refused_given_disc\\\":true in a log\",\"t\":0.3}"));
    CHECK(!psx_setup_refusal_line_names_given(
        "{\"code\":1,\"event\":\"log\",\"message\":\"\\\"event\\\":\\\"error\\\"\",\"refused_given_disc\":true}"));
    CHECK(!psx_setup_refusal_line_names_given("psxrecomp: \"event\":\"error\" \"refused_given_disc\":true"));
    CHECK(!psx_setup_refusal_line_names_given(""));
    CHECK(!psx_setup_refusal_line_names_given(NULL));

    psx_setup_refusal_after_prepare(held, sizeof(held), disc, PSX_SETUP_EXIT_VERIFY, 1);
    CHECK(strcmp(held, disc) == 0);
    CHECK(psx_setup_refusal_at_check(held, disc));
    CHECK(psx_setup_refusal_at_check(held, disc));          /* asking does not forget */
    CHECK(psx_setup_refusal_at_check(held, "D:\\Games\\Suikoden (USA).cue"));   /* either slash */
#if defined(_WIN32)
    CHECK(psx_setup_refusal_at_check(held, "d:/games/SUIKODEN (usa).CUE"));
#else
    {
        char copy[64];
        snprintf(copy, sizeof(copy), "%s", held);
        CHECK(!psx_setup_refusal_at_check(copy, "d:/games/SUIKODEN (usa).CUE"));
    }
#endif

    /* A later failure that is not a refusal leaves it: the file is still the
     * one setup refused. */
    psx_setup_refusal_after_prepare(held, sizeof(held), disc, 1, 0);
    psx_setup_refusal_after_prepare(held, sizeof(held), disc, -1, 0);
    CHECK(psx_setup_refusal_at_check(held, disc));

    /* A prepare that succeeds forgets it: the player replaced the file in
     * place and pressed Generate again. */
    psx_setup_refusal_after_prepare(held, sizeof(held), disc, 0, 0);
    CHECK(held[0] == '\0' && !psx_setup_refusal_at_check(held, disc));

    /* Another file forgets it, and going back to the first one does not
     * bring it back. */
    psx_setup_refusal_after_prepare(held, sizeof(held), disc, PSX_SETUP_EXIT_VERIFY, 1);
    CHECK(!psx_setup_refusal_at_check(held, "D:/Games/Suikoden (USA) (Rev 1).cue"));
    CHECK(held[0] == '\0');
    CHECK(!psx_setup_refusal_at_check(held, disc));

    /* A second refusal replaces the first. */
    psx_setup_refusal_after_prepare(held, sizeof(held), disc, PSX_SETUP_EXIT_VERIFY, 1);
    psx_setup_refusal_after_prepare(held, sizeof(held), "E:/other.cue", PSX_SETUP_EXIT_VERIFY, 1);
    CHECK(psx_setup_refusal_at_check(held, "E:/other.cue"));

    /* No path, no buffer: nothing happens. */
    held[0] = '\0';
    psx_setup_refusal_after_prepare(held, sizeof(held), "", PSX_SETUP_EXIT_VERIFY, 1);
    psx_setup_refusal_after_prepare(held, sizeof(held), NULL, PSX_SETUP_EXIT_VERIFY, 1);
    CHECK(held[0] == '\0');
    psx_setup_refusal_after_prepare(NULL, 0, disc, PSX_SETUP_EXIT_VERIFY, 1);
    CHECK(!psx_setup_refusal_at_check(NULL, disc));
    psx_setup_refusal_after_prepare(held, sizeof(held), disc, PSX_SETUP_EXIT_VERIFY, 1);
    CHECK(!psx_setup_refusal_at_check(held, ""));            /* an empty selection is another file */
    CHECK(held[0] == '\0');

    /* A path longer than the room is cut and so never matches: the file is
     * then judged by the game program alone, never refused by mistake. */
    {
        char small[8] = "";
        psx_setup_refusal_after_prepare(small, sizeof(small), disc, PSX_SETUP_EXIT_VERIFY, 1);
        CHECK(!psx_setup_refusal_at_check(small, disc));
    }
    if (failures) return 1;
    puts("rules: ok");
    return 0;
}
"""


def run_rules() -> None:
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        print("SKIP the rules: no C compiler on PATH")
        return
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        src = tmp / "rules.c"
        src.write_text(HARNESS, encoding="utf-8")
        exe = tmp / ("rules.exe" if os.name == "nt" else "rules")
        build = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-o", str(exe), str(src),
                                "-I", str(ROOT / "host")], capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
        assert build.returncode == 0, "the rules do not compile:\n" + build.stderr[-2000:]
        assert "psx_setup_refusal.h" not in build.stderr, "a warning in the rules:\n" + build.stderr[-1500:]
        run = subprocess.run([str(exe)], capture_output=True, text=True, encoding="utf-8", errors="replace")
        assert run.returncode == 0, "the rules failed:\n" + run.stderr[-2000:]


run_rules()

# The exit code of the CLI is recorded by both runners, where it is known, and
# reads -1 until then.
for runner in ("static int run_cli_win(", "static int run_cli_posix("):
    body = HOST[HOST.index(runner):]
    body = body[: body.index("\n}\n")]
    assert "g_cli_last_exit_code = -1;" in body, f"{runner} must not leave an old exit code standing"
    assert re.search(r"g_cli_last_exit_code = \(long\)code;\s*if \(code == 0\) return 1;", body), (
        f"{runner} must record the exit code before it returns"
    )
    assert re.search(r"g_cli_last_exit_code = -1;\s*g_cli_refused_given_disc = 0;", body), (
        f"{runner} must not leave an old 'the given file was refused' standing"
    )
    assert "cli_tail_note(&tail, line);" in body, f"{runner} must hand every line of the CLI to cli_tail_note"

# Whether the refused file is the selected one is read from the CLI's own
# words, for every line it writes.
note = HOST[HOST.index("static void cli_tail_note("):]
note = note[: note.index("\n}\n")]
assert re.search(
    r"if \(psx_setup_refusal_line_names_given\(line\)\)\s*g_cli_refused_given_disc = 1;\s*"
    r"if \(!json_get_string\(line, \"message\"",
    note,
), "the flag must be read before a line without a message is dropped"
assert HOST.count("g_cli_refused_given_disc = 1;") == 1

# The CLI says it: the single program for its --disc, the set only for disc 1
# when that disc is the --disc file, a program of a set only for that disc.
CLI = (ROOT / "psxrecomp_cli.py").read_text(encoding="utf-8")
cli_generate = CLI[CLI.index("def cmd_generate("):]
cli_generate = cli_generate[: cli_generate.index("\ndef ")]
assert re.search(
    r"progress\.error\(str\(exc\), code=EXIT_VERIFY, verify_failed=True,\s*"
    r"refused_given_disc=bool\(args\.disc\)\)",
    cli_generate,
)
SET = (ROOT / "tools" / "program_set.py").read_text(encoding="utf-8")
assert re.search(
    r"progress\.error\(str\(error\), code=cli\.EXIT_VERIFY, verify_failed=True,\s*"
    r"refused_given_disc=disc_1_is_given and error\.number == 1\)",
    SET,
)
assert 'given_disc=disc_1_is_given and program["positions"][0] == 1)' in SET

# The generate callback applies the rules: with the CLI's exit code and its
# word on the file when the run failed, with 0 when the disc was accepted.
# Nothing else writes the remembered file: a rebuild, a toolchain install and
# a PGO run do not.
generate = HOST[HOST.index("static int host_prepare_generate("):]
generate = generate[: generate.index("\n}\n")]
assert len(re.findall(
    r"psx_setup_refusal_after_prepare\(g_refused_disc, sizeof\(g_refused_disc\),\s*"
    r"source_path, g_cli_last_exit_code,\s*g_cli_refused_given_disc\);\s*return 0;", generate)) == 2, (
    "a failed generate must apply the rules with the CLI's exit code on both platforms"
)
assert re.search(
    r"psx_setup_refusal_after_prepare\(g_refused_disc, sizeof\(g_refused_disc\),\s*source_path, 0, 0\);",
    generate,
), "an accepted disc must forget a refusal"
assert HOST.count("psx_setup_refusal_after_prepare(") == 3, "only the generate callback may set or forget the refusal"
# The early ends of the callback (no tools, no disc, a project that fails its
# preflight) come before any CLI run and do not touch what is remembered.
first_rule = generate.index("psx_setup_refusal_after_prepare(")
for early in ('"Local codegen tools are not available."', '"No disc selected."', "host_preflight_project_root("):
    assert generate.index(early) < first_rule

# The wrapper: the game program answers first, and only its verdict is
# replaced, only for the remembered file.
wrapper = HOST[HOST.index("static int host_disc_verify("):]
wrapper = wrapper[: wrapper.index("\n}\n")]
assert "g_inner_disc_verify ? g_inner_disc_verify(disc_path, out) : 0;" in wrapper
assert re.search(
    r"if \(answered && out && disc_path && disc_path\[0\] &&\s*"
    r"psx_setup_refusal_at_check\(g_refused_disc, disc_path\)\)\s*out->verdict = 3;",
    wrapper,
)
assert wrapper.count("out->") == 1, "the wrapper may change the verdict and nothing else"
assert HOST.count("psx_setup_refusal_at_check(") == 1

# Installed over the game program's own check when the host is applied, once.
apply = HOST[HOST.index("void psxrecomp_codegen_host_apply("):]
assert re.search(
    r"if \(gi->disc_verify && gi->disc_verify != host_disc_verify\) \{\s*"
    r"g_inner_disc_verify = gi->disc_verify;\s*gi->disc_verify = host_disc_verify;\s*\}",
    apply,
)

# Memory of the process only. The remembered file is one static buffer, it is
# given to no function that writes a file or a setting, and the hand-over to
# the built game starts that program anew (a new process on Windows, a new
# program image elsewhere), which begins with none.
assert "static char g_refused_disc[1024];" in HOST
uses = re.findall(r"[^\n]*g_refused_disc[^\n]*", HOST)
assert len(uses) == 5, uses   # the definition, three rule calls, the check
for line in uses:
    assert not re.search(r"fopen|fprintf|fputs|write_line_file|persist|setenv|_putenv", line), line
forward = HOST[HOST.index("void psxrecomp_codegen_host_forward_if_built("):]
forward = forward[: forward.index("\n}\n")]
assert "CreateProcess" in forward and "execv(" in forward, "the hand-over must start the built game anew"

print("setup refusal verdict: ok")
