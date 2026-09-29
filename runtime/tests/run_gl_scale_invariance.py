"""Build and run test_gl_scale_invariance.c on a hidden real OpenGL context.

The fixture runs once per internal scale; the guest-visible (native) VRAM
digest must be identical at every scale, and each run checks line thickness
at internal resolution. Window runs (PSX_GL_HIRES_WINDOW=1) keep VRAM at 1x
and only the frame at S: their native digest must match too, and their frame
and native-wide (16:9) surface at internal resolution must equal the
full-VRAM run's at the same scale.
An 18x window run is 8K past a 16384 texture limit. Two clamp runs check that
an over-limit request (32x, and a tiny memory budget) stays on GL.
Side-by-side runs (mode sbs) flip two 512-wide buffers at x=0 and x=512: the
window must hold both at S and match the full-VRAM surface, as one surface
while their union fits (5x) and as two tiles when it does not (9x under a
simulated 8192 limit, PSX_GL_MAX_DIM, and 18x on a 16384 GPU); copies 1000 px
wide are staged in chunks the limit allows.
Line runs (mode lines) batch lines with triangles over native-wide: the window
runs' native frame must equal the 1x run's (the 1x authoritative surface draws
its lines as GL_LINES, in painter order), and their frame and wide surface at S
the full-VRAM run's at the same scale.
Capture runs (mode capture) check the frame-blend history and hold-last
captures: at the source scale outside the window mode, at the presented
(letterbox) size in it.
Mask runs (mode mask) change the GP0(E6) mask-check bit after a line, a flat
triangle or an opaque textured rect and before its batch is drawn: the draw
must keep the check bit it was submitted under, at 1x, above it and in the
window mode.

macOS/Linux: pass the SDL3 include directory and static library (for example
from a runtime build tree's _deps/sdl3-src/include and
_deps/sdl3-build/libSDL3.a) and a C compiler. Evidence (commands, output) is
written to receipt.json under --output.
"""
import argparse
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import tempfile

MAC_FRAMEWORKS = ["Cocoa", "OpenGL", "IOKit", "CoreVideo", "CoreAudio", "AudioToolbox",
                  "Carbon", "ForceFeedback", "GameController", "Metal", "QuartzCore",
                  "CoreMedia", "AVFoundation", "Foundation", "CoreHaptics",
                  "UniformTypeIdentifiers"]


def parse_run(stdout):
    """Return (checks, failures, digest or None) from a fixture run."""
    summary = re.search(r"^checks=(\d+) failures=(\d+)$", stdout, re.M)
    digest = re.search(r"^digest=([0-9a-f]{16})$", stdout, re.M)
    if not summary:
        return None
    return int(summary[1]), int(summary[2]), digest[1] if digest else None


def parse_hires(stdout, key="hires"):
    m = re.search(r"^" + key + r"=([0-9a-f]{16})$", stdout, re.M)
    return m[1] if m else None


def digests_agree(results):
    """results: {scale: digest}. All present and equal."""
    values = list(results.values())
    return bool(values) and all(v is not None for v in values) and len(set(values)) == 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--sdl-include", required=True)
    ap.add_argument("--sdl-library", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--fixture", type=pathlib.Path)
    ap.add_argument("--scales", default="1,2,3,5,9")
    ap.add_argument("--window-scales", default="2,3,5,9,18")
    args = ap.parse_args()
    # ';'-separated when CMake hands over a target's include list.
    sdl_includes = [str(pathlib.Path(d).resolve()) for d in args.sdl_include.split(";") if d]
    args.sdl_library = str(pathlib.Path(args.sdl_library).resolve())
    framework = pathlib.Path(__file__).resolve().parents[2]
    fixture = args.fixture or framework / "runtime/tests/test_gl_scale_invariance.c"
    out_root = pathlib.Path(args.output).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    dest = pathlib.Path(tempfile.mkdtemp(prefix="scale-", dir=out_root))
    print("Evidence directory:", dest)
    receipt = []

    def run(command, env=None):
        command = [str(c) for c in command]
        r = subprocess.run(command, cwd=dest, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env)
        receipt.append({"cmd": command, "exit": r.returncode,
                        "stdout": r.stdout, "stderr": r.stderr[-4000:]})
        (dest / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        return r

    includes = ["-I", framework / "runtime/include", "-I", framework / "runtime/src"]
    for d in sdl_includes:
        includes += ["-I", d]
    # The fixture #includes gpu_gl_renderer.c; these are the other runtime
    # sources the renderer calls into. render_pass_plan.c exists once the
    # frame-rate render passes have landed, and the renderer calls it from then
    # on, so it is linked whenever it is there.
    sources = [("probe", fixture), ("sw", framework / "runtime/src/gpu_sw_renderer.c"),
               ("fi", framework / "runtime/src/frame_interpolation.c")]
    if (framework / "runtime/src/render_pass_plan.c").exists():
        sources.append(("rp", framework / "runtime/src/render_pass_plan.c"))
    # Unused renderer functions reference the rest of the runtime; the linker
    # drops them (-dead_strip, or per-function sections with --gc-sections).
    # Anything still unresolved is a link error, not a NULL call at run time.
    sections = [] if platform.system() == "Darwin" else ["-ffunction-sections", "-fdata-sections"]
    objs = []
    for name, src in sources:
        o = dest / (name + ".o")
        r = run([args.cc, "-std=gnu11", "-O1", "-DPSX_SDL3=1", "-DPSX_NO_DEBUG_TOOLS=1",
                 "-DGL_SILENCE_DEPRECATION=1", "-w", *sections, *includes, "-c", src, "-o", o])
        if r.returncode:
            print(r.stderr[-3000:])
            return 2
        objs.append(o)
    link = [args.cc, *objs, args.sdl_library, "-o", dest / "probe"]
    if platform.system() == "Darwin":
        for f in MAC_FRAMEWORKS:
            link += ["-framework", f]
        link += ["-liconv", "-lm", "-Wl,-dead_strip"]
    else:
        link += ["-lGL", "-lm", "-ldl", "-lpthread", "-Wl,--gc-sections"]
    r = run(link)
    if r.returncode:
        print(r.stderr[-3000:])
        return 2

    ok = True
    digests = {}
    hires_full = {}
    for s in [int(v) for v in args.scales.split(",") if v]:
        r = run([dest / "probe", s])
        parsed = parse_run(r.stdout)
        print(f"scale {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-3:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        digests[("full", s)] = parsed[2] if parsed else None
        hires_full[s] = (parse_hires(r.stdout), parse_hires(r.stdout, "wide"))
    env = os.environ.copy()
    wenv = dict(env)
    wenv["PSX_GL_HIRES_WINDOW"] = "1"
    for s in [int(v) for v in args.window_scales.split(",") if v]:
        r = run([dest / "probe", s, "window"], env=wenv)
        parsed = parse_run(r.stdout)
        print(f"window {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-3:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        digests[("window", s)] = parsed[2] if parsed else None
        h = (parse_hires(r.stdout), parse_hires(r.stdout, "wide"))
        if s in hires_full and hires_full[s] != h:
            print(f"FAIL window {s}x frame/wide surface differ from the full-VRAM "
                  f"surface at {s}x:", h, hires_full[s])
            ok = False
    if not digests_agree(digests):
        print("FAIL native VRAM digest differs across scales/modes:", digests)
        ok = False
    # Side-by-side flips: (label, scale, env, reference run or None, tiles).
    sbs = {}
    sbs_runs = (
        ("full", 1, {}, None, None),
        ("full", 5, {}, None, None),
        ("full", 9, {}, None, None),
        ("window", 5, {"PSX_GL_HIRES_WINDOW": "1"}, ("full", 5), 1),
        ("window-8192", 9, {"PSX_GL_MAX_DIM": "8192"}, ("full", 9), 2),
        ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"}, None, None),
    )
    for label, s, extra, ref, tiles in sbs_runs:
        e = dict(env)
        e.update(extra)
        r = run([dest / "probe", s, "sbs"], env=e)
        parsed = parse_run(r.stdout)
        got_tiles = re.search(r"^tiles=(\d+)$", r.stdout, re.M)
        print(f"sbs {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-5:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        sbs[(label, s)] = (parsed[2] if parsed else None,
                           parse_hires(r.stdout), parse_hires(r.stdout, "wide"))
        if tiles is not None and (not got_tiles or int(got_tiles[1]) != tiles):
            print(f"FAIL sbs {label} {s}x: tiles", got_tiles and got_tiles[1], "want", tiles)
            ok = False
        if ref is not None and sbs.get(ref, (None,))[1:] != sbs[(label, s)][1:]:
            print(f"FAIL sbs {label} {s}x buffers differ from {ref}:",
                  sbs[(label, s)][1:], sbs.get(ref))
            ok = False
    if not digests_agree({k: v[0] for k, v in sbs.items()}):
        print("FAIL sbs native VRAM digest differs across scales/modes:", sbs)
        ok = False
    # Lines batched with triangles: (label, scale, env).
    lines = {}
    for label, s, extra in (("full", 1, {}), ("full", 9, {}),
                            ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}),
                            ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"})):
        e = dict(env)
        e.update(extra)
        r = run([dest / "probe", s, "lines"], env=e)
        parsed = parse_run(r.stdout)
        print(f"lines {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-4:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        lines[(label, s)] = (parse_hires(r.stdout, "lband"), parse_hires(r.stdout),
                             parse_hires(r.stdout, "wide"))
    for key in (("window", 9), ("window", 18)):
        if lines[key][0] is None or lines[key][0] != lines[("full", 1)][0]:
            print(f"FAIL lines {key}: native frame differs from the 1x run:",
                  lines[key][0], lines[("full", 1)][0])
            ok = False
    if None in lines[("window", 9)][1:] or lines[("window", 9)][1:] != lines[("full", 9)][1:]:
        print("FAIL lines window 9x frame/wide surface differ from the full-VRAM run:",
              lines[("window", 9)][1:], lines[("full", 9)][1:])
        ok = False
    # Frame-blend and hold-last captures: (label, scale, env, windowed).
    for label, s, extra, windowed in (("full", 9, {}, False),
                                      ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}, True),
                                      ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"}, True)):
        e = dict(env)
        e.update(extra)
        r = run([dest / "probe", s, "capture"], env=e)
        parsed = parse_run(r.stdout)
        print(f"capture {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-2:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        m = re.search(r"^capture=(\d+)x(\d+) source=(\d+)x(\d+)$", r.stdout, re.M)
        if not m or ((m[1], m[2]) == (m[3], m[4])) == windowed:
            print(f"FAIL capture {label} {s}x: size", m and m.group(0))
            ok = False
    # Mask-check changes between a draw and its batch: (label, scale, env).
    for label, s, extra in (("full", 1, {}), ("full", 4, {}), ("full", 9, {}),
                            ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}),
                            ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"})):
        e = dict(env)
        e.update(extra)
        r = run([dest / "probe", s, "mask"], env=e)
        parsed = parse_run(r.stdout)
        print(f"mask {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-1:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
    for label, s, budget in (("over-limit", 32, None), ("budget", 12, "40")):
        e = dict(env)
        if budget is not None:
            e["PSX_GL_VRAM_BUDGET_MB"] = budget
        r = run([dest / "probe", s, "clamp"], env=e)
        parsed = parse_run(r.stdout)
        print(f"clamp {label}: exit={r.returncode}", r.stdout.strip().splitlines()[-2:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
