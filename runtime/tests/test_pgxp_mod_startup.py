"""Execute the real PGXP activation and video-configuration statements."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.source_root
    source = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")
    begin = source.index('    if (const char* e = std::getenv("PSX_GEOMETRY_CORRECTION"))')
    end = source.index("    /* Scanlines:", begin)
    settings = source[begin:end]
    request = re.search(
        r'extern "C" void psx_mod_request_pgxp\(int cpu_mode\) \{.*?^\}',
        source, re.M | re.S,
    )
    reset = re.search(r"^    g_mod_pgxp_cpu_mode = -1;$", source, re.M)
    request_code = request.group() if request else ""
    reset_code = reset.group().strip() if reset else ""
    if request:
        assert reset, "PGXP request must reset before each activation"
        assert reset.start() < source.index("    mod_runtime_activate_plugins();") < begin
        assert "g_mod_pgxp_cpu_mode >= 0 && !net_cfg.enabled" in settings
    harness = r'''
#include "mod_plugins.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
static PSXModActivationCallback activate;
static int geometry, texture, cpu_mode, option_cpu;
static int g_video_geometry_correction, g_video_perspective_texturing;
static int g_video_pgxp_cpu_mode, g_mod_pgxp_cpu_mode = -1;
static float g_video_pgxp_tolerance;
static struct { bool enabled; } net_cfg;
extern "C" int psx_mod_register_activation_plugin(const char* id, PSXModActivationCallback fn) {
    assert(std::strcmp(id, "psx.pgxp") == 0); activate = fn; return 1;
}
extern "C" int psx_mod_option_value(const char*, const char*, const char*, char* out, uint32_t size) {
    std::snprintf(out, size, "%s", option_cpu ? "true" : "false"); return 1;
}
extern "C" void gte_geometry_correction_set(int enabled) { geometry = enabled; }
extern "C" void gpu_texture_correction_set(int enabled) { texture = enabled; }
extern "C" void pgxp_set_cpu_mode(int enabled) { cpu_mode = enabled; }
extern "C" void pgxp_set_tolerance(float) {}
REQUEST
static void configure_video() { SETTINGS }
static void launch(bool enabled, bool netplay = false) {
    RESET
    net_cfg.enabled = netplay;
    if (enabled) activate();
    configure_video();
}
int main() {
    assert(activate);
    launch(true);
    assert(geometry == 1 && texture == 1 && cpu_mode == 0);
    option_cpu = 1;
    launch(true);
    assert(geometry == 1 && texture == 1 && cpu_mode == 1);
    launch(false); // Disabling after an enabled session must remove its request.
    assert(geometry == 0 && texture == 0 && cpu_mode == 0);
    g_video_perspective_texturing = 1;
    launch(false);
    assert(geometry == 0 && texture == 1 && cpu_mode == 0);
    g_video_perspective_texturing = 0;
    launch(true, true); // A rematch cannot reactivate an offline mod.
    assert(geometry == 0 && texture == 0 && cpu_mode == 0);
    return 0;
}
'''.replace("REQUEST", request_code).replace("SETTINGS", settings).replace("RESET", reset_code)
    harness = "#include <cstdio>\n" + harness
    env = os.environ.copy()
    for name in ("PSX_GEOMETRY_CORRECTION", "PSX_PERSPECTIVE_TEXTURING", "PSX_PGXP_CPU_MODE"):
        env.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="pgxp-order-") as directory:
        work = Path(directory)
        (work / "main.cpp").write_text(harness, encoding="utf-8")
        include = "-I" + str(root / "runtime/include")
        subprocess.run([args.cc, "-std=c11", include, "-c",
                        str(root / "runtime/src/mod_builtin_pgxp.c"),
                        "-o", str(work / "plugin.o")], check=True)
        exe = work / ("test.exe" if os.name == "nt" else "test")
        subprocess.run([args.cxx, "-std=c++17", include, str(work / "main.cpp"),
                        str(work / "plugin.o"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, env=env)
    print("PGXP activation survives video setup; disable, CPU option and netplay controls pass")


if __name__ == "__main__":
    main()
