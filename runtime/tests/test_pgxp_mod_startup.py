#!/usr/bin/env python3
"""Test the PGXP activation/startup seam using current source fragments.

The C/C++ fixture uses the real plugin and extracted main.cpp request, reset and
configuration code. GPU/GTE setters are stubs; this does not test a running game,
the renderer, preprocessing of all main.cpp, or native product behavior.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import os

ROOT = Path(__file__).resolve().parents[2]


def pgxp_startup_order(source, begin):
    """Ignore inactive text and require calls in the configuration's block."""
    if type(begin) is not int or not 0 <= begin < len(source):
        return False
    inactive = (r'//[^\n]*|/\*.*?\*/|'
                r'R"(?P<raw>[^\s()\\]{0,16})\(.*?\)(?P=raw)"|'
                r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    code = re.sub(inactive, lambda m: re.sub(r"[^\n]", " ", m[0]),
                  source, flags=re.S)
    if not code.startswith("    if (const char* e = std::getenv(", begin):
        return False
    blocks, activated, initialized = [], set(), set()
    tokens = r"[{}]|(?m:^    (?:mod_runtime_activate_plugins|gpu_init)\(\);[ \t]*$)"
    for match in re.finditer(tokens, code[:begin]):
        token = match[0].strip()
        if token == "{":
            blocks.append(match.start())
        elif token == "}":
            if not blocks:
                return False
            blocks.pop()
        elif token == "mod_runtime_activate_plugins();":
            activated.add(tuple(blocks))
            initialized.discard(tuple(blocks))
        elif tuple(blocks) in activated:
            initialized.add(tuple(blocks))
    return bool(blocks) and tuple(blocks) in initialized


def one(pattern, source, label):
    matches = list(re.finditer(pattern, source, re.M))
    assert len(matches) == 1, f"expected one {label}, got {len(matches)}"
    return matches[0]


def startup_fragments(source, header, plugin):
    """Fail closed on the selected startup seam before compiling its fixture."""
    main = one(r"^int main\(int argc, char\*\* argv\) \{$", source, "main")
    state = one(r"^static int\s+g_mod_pgxp_cpu_mode = -1;$", source,
                "PGXP request state")
    request = one(
        r'^extern "C" void psx_mod_request_pgxp\(int cpu_mode\) \{\n'
        r'    g_mod_pgxp_cpu_mode = cpu_mode \? 1 : 0;\n\}',
        source, "PGXP request definition")
    reset = one(r"^    g_mod_pgxp_cpu_mode = -1;$", source, "session reset")
    activate = one(r"^    mod_runtime_activate_plugins\(\);$", source, "activation")
    init = one(r"^    gpu_init\(\);$", source, "GPU initialization")
    marker = '    if (const char* e = std::getenv("PSX_GEOMETRY_CORRECTION"))'
    begin = one(re.escape(marker), source, "geometry configuration").start()
    end = source.index("    /* Scanlines:", begin)
    settings = source[begin:end]
    assert main.start() < reset.start() < activate.start() < init.start() < begin
    # A lobby rematch jumps to session_reboot; it must clear and re-run activation.
    label = one(r"^session_reboot:$", source, "session_reboot label")
    assert label.start() < reset.start(), \
        "the mod reset and activation must follow session_reboot so a rematch reruns them"
    # Some mods select OpenGL during activation; --renderer must still win.
    renderer = r"^    if \(cli_renderer\s+>= 0\) g_video_renderer = cli_renderer;$"
    assert any(activate.start() < match.start() < init.start()
               for match in re.finditer(renderer, source, re.M)), \
        "the --renderer override must follow mod activation"
    assert pgxp_startup_order(source, begin), \
        "PGXP setup must follow activation and GPU initialization in its block"
    assert "void psx_mod_request_pgxp(int cpu_mode);" in header
    assert "    psx_mod_request_pgxp(pgxp_option_flag(\"pgxp\", \"cpu_mode\"));" in plugin
    for direct_setter in ("gte_geometry_correction_set", "gpu_texture_correction_set",
                          "pgxp_set_cpu_mode"):
        assert direct_setter not in plugin, \
            f"activation must defer {direct_setter} until GPU startup"
    for expression in (
        "const bool pgxp_mod_enabled = g_mod_pgxp_cpu_mode >= 0 && !net_cfg.enabled;",
        "const int pgxp_session_geometry = g_video_geometry_correction || pgxp_mod_enabled;",
        "const int pgxp_session_texture = g_video_perspective_texturing || pgxp_mod_enabled;",
        "gte_geometry_correction_set(pgxp_session_geometry);",
        "gpu_texture_correction_set(pgxp_session_texture);",
        "pgxp_set_cpu_mode(pgxp_mod_enabled ? g_mod_pgxp_cpu_mode : g_video_pgxp_cpu_mode);",
        "pgxp_set_tolerance(g_video_pgxp_tolerance);",
    ):
        assert expression in settings, f"missing startup expression: {expression}"
    for saved_value in ("geometry_correction", "perspective_texturing", "pgxp_cpu_mode"):
        assert f"g_video_{saved_value} = pgxp_" not in settings, \
            "a session request must not overwrite saved settings"
    return dict(state=state[0], request=request[0], reset=reset[0],
                activate=activate[0], init=init[0], settings=settings)


HARNESS = r'''
#include "mod_plugins.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
@@state@@
static int g_video_geometry_correction, g_video_perspective_texturing;
static int g_video_pgxp_cpu_mode;
static float g_video_pgxp_tolerance = 0.5f;
static struct { bool enabled; } net_cfg;
static PSXModActivationCallback activation;
static bool package_enabled, gpu_initialized;
static const char* cpu_option;
static int geometry, texture, cpu;
extern "C" int psx_mod_register_activation_plugin(
    const char* id, PSXModActivationCallback callback) {
    assert(std::strcmp(id, "psx.pgxp") == 0);
    activation = callback;
    return 1;
}
extern "C" int psx_mod_option_value(
    const char* package, const char* feature, const char* option,
    char* out, uint32_t size) {
    assert(std::strcmp(package, "psx.enhancement.pgxp") == 0);
    assert(std::strcmp(feature, "pgxp") == 0);
    assert(std::strcmp(option, "cpu_mode") == 0);
    if (std::strcmp(cpu_option, "unset") == 0) return 0;
    std::snprintf(out, size, "%s", cpu_option);
    return 1;
}
@@request@@
static void mod_runtime_activate_plugins() {
    if (package_enabled) activation();
}
static void gpu_init() {
    gpu_initialized = true;
    geometry = texture = cpu = 0;
}
extern "C" void gte_geometry_correction_set(int value) {
    assert(gpu_initialized); geometry = value;
}
extern "C" void gpu_texture_correction_set(int value) {
    assert(gpu_initialized); texture = value;
}
extern "C" void pgxp_set_cpu_mode(int value) {
    assert(gpu_initialized); cpu = value;
}
extern "C" void pgxp_set_tolerance(float value) {
    assert(gpu_initialized && value == 0.5f);
}
static void start_session() {
    gpu_initialized = false;
@@reset@@
@@activate@@
@@init@@
@@settings@@
}
int main(int argc, char** argv) {
    assert(argc == 9 && activation);
    g_video_geometry_correction = std::atoi(argv[1]);
    g_video_perspective_texturing = std::atoi(argv[2]);
    g_video_pgxp_cpu_mode = std::atoi(argv[3]);
    net_cfg.enabled = std::atoi(argv[4]) != 0;
    cpu_option = argv[5];
    package_enabled = true;
    start_session();
    assert(geometry == std::atoi(argv[6]));
    assert(texture == std::atoi(argv[7]));
    assert(cpu == std::atoi(argv[8]));
    assert(g_video_geometry_correction == std::atoi(argv[1]));
    assert(g_video_perspective_texturing == std::atoi(argv[2]));
    assert(g_video_pgxp_cpu_mode == std::atoi(argv[3]));
    package_enabled = false;
    start_session();
    assert(g_mod_pgxp_cpu_mode == -1);
    assert(geometry == std::atoi(argv[1]));
    assert(texture == std::atoi(argv[2]));
    assert(cpu == std::atoi(argv[3]));
}
'''


def render_harness(fragments):
    harness = HARNESS
    for name, text in fragments.items():
        harness = harness.replace(f"@@{name}@@", text)
    assert "@@" not in harness
    return harness


def run(command, **kwargs):
    subprocess.run(command, check=True, text=True, encoding="utf-8",
                   errors="replace", **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.source_root.resolve()
    source = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")
    header = (root / "runtime/include/mod_plugins.h").read_text(encoding="utf-8")
    plugin = (root / "runtime/src/mod_builtin_pgxp.c").read_text(encoding="utf-8")
    fragments = startup_fragments(source, header, plugin)
    env = os.environ.copy()
    for name in ("PSX_GEOMETRY_CORRECTION", "PSX_PERSPECTIVE_TEXTURING",
                 "PSX_PGXP_CPU_MODE"):
        env.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="pgxp-startup-") as scratch:
        scratch = Path(scratch)
        fixture = scratch / "startup.cpp"
        fixture.write_text(render_harness(fragments), encoding="utf-8")
        obj = scratch / "plugin.o"
        exe = scratch / ("startup.exe" if os.name == "nt" else "startup")
        include = str(root / "runtime/include")
        run([args.cc, "-std=c11", "-I", include, "-c",
             str(root / "runtime/src/mod_builtin_pgxp.c"), "-o", str(obj)])
        run([args.cxx, "-std=c++17", "-I", include, str(fixture),
             str(obj), "-o", str(exe)])
        # Each run also disables the package in a second session and checks
        # restoration of its independent baseline and a cleared request.
        cases = (
            ("cpu-on then disabled", 0, 0, 0, 0, "true", 1, 1, 1),
            ("missing option with texture baseline", 0, 1, 1, 0, "unset", 1, 1, 0),
            ("cpu-off with geometry baseline", 1, 0, 1, 0, "false", 1, 1, 0),
            ("netplay ignores activation request", 0, 1, 1, 1, "true", 0, 1, 1),
        )
        for label, *values in cases:
            run([str(exe), *(str(value) for value in values)], env=env)
            print(f"PGXP startup seam: {label}")


if __name__ == "__main__":
    main()
