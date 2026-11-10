#!/usr/bin/env python3
"""Run the actual setup wrapper and report module with a controlled inner check.

PS1B-415: a setup refusal must replace the inner check's recorded verdict.
This does not run Generate, mount a disc, or draw a launcher.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument("--ui-root", type=Path)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    root = args.source_root
    candidates = [args.ui_root] if args.ui_root else (
        ([Path(os.environ["RECOMP_UI_ROOT"])] if os.environ.get("RECOMP_UI_ROOT") else [])
        + [root / "recomp-ui", root.parent / "recomp-ui"]
    )
    ui = next((p for p in candidates if (p / "src/recomp_launcher.h").is_file()), None)
    if ui is None:
        if args.ui_root:
            raise FileNotFoundError("--ui-root has no src/recomp_launcher.h")
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return
    text = (root / "host/psxrecomp_codegen_host.c").read_text(encoding="utf-8")
    start = text.index("static int host_disc_verify(")
    wrapper = text[start:text.index("\n}\n", start) + 2]
    probe = r'''#include "recomp_launcher.h"
#include "psx_setup_refusal.h"
#include "start_refusal.h"
#include <assert.h>
#include <string.h>
static char g_refused_disc[1024];
static int inner_verdict = 1;
static int inner(const char* path, RecompLauncherCDiscVerify* out) {
    if (!path || !out) return 0;
    memset(out, 0, sizeof(*out));
    strcpy(out->serial, "SLES-01156");
    strcpy(out->expected_serials, "SLES-01156");
    out->track_count = 4; out->iso_ok = 1; out->netplay_ok = 1;
    out->serial_status = RECOMP_SERIAL_LISTED;
    out->verdict = inner_verdict;
    psx_start_note_launcher("disc", "inner report");
    return 1;
}
static int (*g_inner_disc_verify)(const char*, RecompLauncherCDiscVerify*) = inner;
''' + wrapper + r'''
static void check(int refused) {
    RecompLauncherCDiscVerify out, want;
    char report[4096];
    assert(inner("selected.cue", &want));
    want.verdict = refused ? 3 : inner_verdict;
    assert(host_disc_verify("selected.cue", &out));
    assert(memcmp(&out, &want, sizeof(out)) == 0);
    assert(psx_start_launcher_status_json(report, sizeof(report)) > 0);
    if (refused) {
        assert(strstr(report, "verdict=refused serial=SLES-01156 expected=SLES-01156 tracks=4; setup refused the selected disc"));
        assert(!strstr(report, "inner report"));
    } else assert(strstr(report, "inner report"));
}
int main(void) {
    RecompLauncherCDiscVerify out;
    char before[4096], after[4096];
    check(0); /* no remembered refusal */
    strcpy(g_refused_disc, "selected.cue"); check(1); /* ok -> refused */
    inner_verdict = 2; check(1); /* warning -> refused */
    inner_verdict = 3; check(1); /* already bad; record setup's reason */
    strcpy(g_refused_disc, "another.cue"); check(0); /* another file clears it */
    assert(!g_refused_disc[0]);
    strcpy(g_refused_disc, "selected.cue");
    psx_setup_refusal_after_prepare(g_refused_disc, sizeof(g_refused_disc), "selected.cue", 0, 0);
    inner_verdict = 1; check(0); /* successful prepare clears it */
    strcpy(g_refused_disc, "selected.cue");
    psx_start_launcher_status_json(before, sizeof(before));
    g_inner_disc_verify = NULL;
    assert(!host_disc_verify("selected.cue", &out)); /* no callback */
    g_inner_disc_verify = inner;
    assert(!host_disc_verify("selected.cue", NULL)); /* no answer buffer */
    assert(!host_disc_verify(NULL, &out)); /* no path */
    psx_start_launcher_status_json(after, sizeof(after));
    assert(!strcmp(before, after));
    puts("setup refusal report: 9 controls passed");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="setup refusal report ") as tmp:
        work = Path(tmp)
        source = work / "probe.c"
        source.write_text(probe, encoding="utf-8")
        program = work / ("probe.exe" if os.name == "nt" else "probe")
        compiler = shutil.which(args.compiler) or args.compiler
        env = os.environ.copy()
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c11", "-O0", "-I" + str(ui / "src"),
                        "-I" + str(root / "host"), "-I" + str(root / "runtime/include"),
                        str(source), str(root / "runtime/src/start_refusal.c"), "-o", str(program)],
                       check=True, env=env, timeout=120, text=True,
                       encoding="utf-8", errors="replace")
        subprocess.run([str(program)], check=True, env=env, timeout=10,
                       text=True, encoding="utf-8", errors="replace")


if __name__ == "__main__":
    main()
