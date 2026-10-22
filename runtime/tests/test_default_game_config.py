#!/usr/bin/env python3
"""Compile the runtime's config selection with controlled lookup results.

PS1B-427: an absent baked set.toml must reach the config loader's refusal,
instead of skipping that loader and opening a launcher with no game.
This checks selection, not an end-to-end setup host or its error box.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.source_root
    text = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")
    start = text.index("    std::string default_game_config_storage;")
    end = text.index("    /* Record the resolved config path", start)
    selection = text[start:end]
    start = text.index("static std::filesystem::path anchor_on_exe_dir(")
    end = text.index("\n}\n", start) + 2
    anchor = text[start:end]
    # The selected pointer must still feed the existing load/refusal path.
    tail = text[text.index("    g_active_config_path = game_config_path;"):]
    assert "if (game_config_path) {" in tail
    assert "load_game_config(game_config_path)" in tail
    assert 'psx_start_refusal_set("config_unreadable"' in tail

    probe = r'''#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include "host_path.h"
namespace fs = std::filesystem;
static std::string baked;
static fs::path specific, existing;
#define PSX_DEFAULT_GAME_CONFIG_PATH baked.c_str()
static fs::path exe_dir_from_argv(const char* argv0) {
    return fs::path(argv0).parent_path();
}
static fs::path exe_specific_game_config(const char*) { return specific; }
static fs::path resolve_existing_runtime_path(const char*, const char*) { return existing; }
''' + anchor + r'''
static fs::path select_config(const char* explicit_path = nullptr) {
    const char* argv[] = {"kit with spaces/Resident_Evil_2.exe"};
    const char* game_config_path = explicit_path;
''' + selection + r'''
    return game_config_path ? fs::path(game_config_path) : fs::path{};
}
static void check(const fs::path& want, const char* explicit_path = nullptr) {
    const auto got = select_config(explicit_path);
    if (got != want) throw std::runtime_error("expected " + want.string() + ", got " + got.string());
}
int main() {
    const auto kit = fs::absolute("kit with spaces");
    baked = "set.toml";
    check(kit / "set.toml"); // missing set: keep the anchored absolute path
    baked = "game.toml";
    check(kit / "game.toml"); // same rule for one-title kits
    baked = fs::absolute("absent-set.toml").string();
    check(fs::path(baked)); // missing absolute default stays absolute
    baked = "set.toml";
    existing = fs::path("parent kit") / "set.toml";
    check(existing); // an existing default keeps the resolver's result
    specific = fs::path("kit with spaces") / "Leon.game.toml";
    check(specific); // executable-specific config keeps precedence
    specific.clear(); existing.clear(); baked.clear();
    check({}); // a framework build with no baked config stays config-free
    check("explicit.toml", "explicit.toml"); // explicit missing path still loads/refuses
    existing = fs::path("resolved kit") / "explicit.toml";
    check(existing, "explicit.toml"); // explicit relative path keeps resolution
    const auto absolute = fs::absolute("explicit.toml").string();
    check(fs::path(absolute), absolute.c_str()); // explicit absolute path stays unchanged
    std::cout << "default config selection: 9 cases passed\n";
}
'''
    with tempfile.TemporaryDirectory(prefix="default config ") as tmp:
        work = Path(tmp)
        source = work / "probe.cpp"
        source.write_text(probe, encoding="utf-8")
        program = work / ("probe.exe" if os.name == "nt" else "probe")
        compiler = shutil.which(args.compiler) or args.compiler
        env = os.environ.copy()
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++17", "-O0",
                        "-I" + str(root / "recompiler/include"),
                        str(source), "-o", str(program)],
                       check=True, env=env, timeout=120, text=True,
                       encoding="utf-8", errors="replace")
        subprocess.run([str(program)], check=True, env=env, timeout=10,
                       text=True, encoding="utf-8", errors="replace")


if __name__ == "__main__":
    main()
