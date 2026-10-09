#!/usr/bin/env python3
"""Compile the runtime's buffer handoff with its real loader and ring bridge."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AudioBuffer(unittest.TestCase):
    def test_game_config_reaches_bridge(self):
        compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "a C++ compiler is required")
        main = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
        globals_ = "\n".join(re.findall(r"^static int\s+g_audio_buffer_ms\s*=.*?;", main, re.M))
        handoff = "\n".join(re.findall(r"^\s*g_audio_buffer_ms\s*=.*?;", main, re.M))
        start = main.index("rab_config cfg; rab_config_defaults(&cfg);")
        end = main.index("\n            }", start)
        startup = main[start:end]
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            src = folder / "probe.cpp"
            src.write_text('''#include <cstdio>
#include <cstdlib>
#include "config_loader.h"
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
''' + globals_ + '''
int main(int argc, char **argv) {
    auto gc = PSXRecompV4::load_game_config(argv[1]);
''' + handoff + '''
    struct { int freq; } have{48000};
    rab_bridge s_drc{};
    bool s_drc_ready = false;
''' + startup + '''
    double expected = std::strtod(argv[2], nullptr);
    if (!s_drc_ready || s_drc.cfg.target_ms != expected) {
        std::fprintf(stderr, "opened target=%.0f expected=%.0f\\n", s_drc.cfg.target_ms, expected);
        return 1;
    }
    if (s_drc.cap < (s_drc.cfg.source_rate * expected / 1000.0)) return 2;
    rab_free(&s_drc);
    return 0;
}
''', encoding="utf-8")
            binary = folder / ("probe.exe" if os.name == "nt" else "probe")
            command = [compiler, "-std=c++17", "-O0", "-DFMT_HEADER_ONLY",
                       "-I" + str(ROOT / "recompiler/src"),
                       "-I" + str(ROOT / "recompiler/include"),
                       "-I" + str(ROOT / "recompiler/lib/fmt/include"),
                       "-I" + str(ROOT / "recompiler/lib/toml11"),
                       "-I" + str(ROOT / "runtime/src"), str(src),
                       str(ROOT / "recompiler/src/config_loader.cpp"),
                       str(ROOT / "recompiler/src/ps1_exe_parser.cpp"), "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace")
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            base = ('[game]\nname="authored buffer probe"\nexe="probe.exe"\n'
                    'load_address="0x80010000"\nentry_pc="0x80010000"\ntext_size="0x1000"\n'
                    '[recompiler]\nseeds="seeds.json"\n')
            for value in (None, 30, 60, 180, 500):
                with self.subTest(buffer_ms=value):
                    config = folder / "game.toml"
                    config.write_text(base + ("" if value is None else
                                      "[audio]\nbuffer_ms=" + str(value) + "\n"), encoding="utf-8")
                    run = subprocess.run([str(binary), str(config), str(180 if value is None else value)],
                                         capture_output=True, text=True, encoding="utf-8", errors="replace")
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
