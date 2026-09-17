#!/usr/bin/env python3
"""Resident control-flow patches never run stale native code.

Fixture: the 5-word resident text from 17e9bbaa0's
test_overlay_resident_patch_promotion.py, at the real boot-text base
0x80010000, with the JALR at 0x80010008 patched to NOP. That commit added a
compile-time classifier that withheld such shards as ".unpromoted". This test
shows why the framework does not need it, end to end:

1. tools/compile_overlays.py publishes an exact-range shard for the patched
   bytes (no .unpromoted sidecar).
2. The real overlay loader and the real memory.c store path / exact-range text
   guard (overlay_resident_patch_harness.c) then decide ownership:
   - pristine text: the static resident function owns it; the shard cannot run;
   - guest CPU store JALR->NOP: the exact-range guard blocks the static entry
     and its post-call continuation; the page is outside the overlay window,
     so the shard does not load and the interpreter owns the live bytes;
   - the same bytes marked executable by a load path: the shard runs natively
     only on exact bytes, and the old call-return PC is a foreign interior
     entry that fails closed;
   - JALR restored: static owns again and the shard does not run.
3. Negative control: with native overlay execution forced off the harness must
   report a mismatch, so a harness that cannot see native runs cannot pass.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import platform
import shutil
import struct
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"
TESTS = RUNTIME / "tests"
GAME_ID = "T106-CFG"

sys.path.insert(0, str(ROOT / "tools"))
import compile_overlays  # noqa: E402


def words(*values: int) -> bytes:
    return b"".join(struct.pack("<I", value) for value in values)


RESIDENT = words(0x27BDFFF0, 0x11111111, 0x01204009, 0x24020001, 0x03E00008)
PATCHED = words(0x27BDFFF0, 0x11111111, 0x00000000, 0x24020001, 0x03E00008)
GUARD = words(0x00000000)  # writer-appended delay slot for the final jr ra


def run(command: list[str], *, cwd: pathlib.Path | None = None,
        env: dict | None = None, expect_ok: bool = True) -> str:
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if expect_ok and result.returncode:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}")
    if not expect_ok and result.returncode == 0:
        raise AssertionError(
            f"command unexpectedly passed: {' '.join(command)}\n{result.stdout}")
    return result.stdout


def write_project(work: pathlib.Path) -> None:
    text = RESIDENT + GUARD + bytes(0x800 - len(RESIDENT) - len(GUARD))
    header = bytearray(0x800)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<IIII", header, 0x10, 0x80010008, 0, 0x80010000, len(text))
    struct.pack_into("<I", header, 0x30, 0x801FFF00)
    (work / "GAME.EXE").write_bytes(bytes(header) + text)
    (work / "seeds.txt").write_text("0x80010008\n", encoding="utf-8")
    (work / "game.toml").write_text(
        '[game]\nname = "Resident CFG patch"\n'
        f'id = "{GAME_ID}"\nexe = "GAME.EXE"\n\n'
        '[recompiler]\nseeds = "seeds.txt"\nout_dir = "generated"\n\n'
        '[runtime]\noverlay_cache = true\n',
        encoding="utf-8")
    (work / "overlay_captures.json").write_text(json.dumps([{
        "load_addr": "0x80010000",
        "size": len(PATCHED) + len(GUARD),
        "guard_bytes": len(GUARD),
        "bytes_b64": base64.b64encode(PATCHED + GUARD).decode("ascii"),
        "function_entry_pcs": ["0x80010008"],
        "dispatch_entry_pcs": ["0x80010008"],
        "executed_pcs": ["0x80010008", "0x8001000C",
                         "0x80010010", "0x80010014"],
    }]), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True)
    parser.add_argument("--compiler",
                        default=shutil.which("gcc") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.compiler:
        raise SystemExit("gcc/cc is required")
    recompiler = os.path.abspath(args.recompiler)
    include = str(RUNTIME / "include")

    with tempfile.TemporaryDirectory(prefix="psx-resident-cfg-",
                                     ignore_cleanup_errors=True) as raw:
        work = pathlib.Path(raw)
        write_project(work)
        cache = work / "cache"
        env = dict(os.environ)
        env.pop("PSX_OVERLAY_CACHE_DIR", None)
        env.pop("PSX_OVERLAY_CAPTURES", None)
        env.pop("PSX_OVERLAY_NATIVE_OFF", None)
        compiler_dir = os.path.dirname(os.path.abspath(args.compiler))
        env["PATH"] = compiler_dir + os.pathsep + env.get("PATH", "")
        output = run([
            sys.executable, str(ROOT / "tools" / "compile_overlays.py"),
            "--captures", str(work / "overlay_captures.json"),
            "--game-toml", str(work / "game.toml"),
            "--recompiler", recompiler,
            "--runtime-include", include,
            "--out-dir", str(cache),
            "--gcc", args.compiler, "--jobs", "1",
        ], cwd=work, env=env)
        print(output.strip().splitlines()[-2])

        toml = str(work / "game.toml")
        leaf = (cache / GAME_ID / "gcc" / compile_overlays.cache_arch_abi() /
                compile_overlays.cache_tag(include, recompiler, toml, 0))
        ext = ".dll" if platform.system() == "Windows" else ".so"
        shards = sorted(leaf.glob(f"00010000_*{ext}"))
        if len(shards) != 1:
            raise AssertionError(f"expected one published shard in {leaf}: "
                                 f"{sorted(p.name for p in leaf.iterdir())}")
        manifest = shards[0].with_suffix(".ranges").read_text(encoding="ascii")
        if "F 80010008 " not in manifest:
            raise AssertionError(f"shard does not own 0x80010008:\n{manifest}")
        if list(leaf.glob("*.unpromoted")):
            raise AssertionError("compile_overlays wrote an .unpromoted sidecar")
        print(f"published {shards[0].name} without an .unpromoted sidecar")

        exe = ".exe" if platform.system() == "Windows" else ""
        harness = work / f"resident-patch-harness{exe}"
        codegen = compile_overlays.codegen_hash(include, recompiler)
        config = compile_overlays.overlay_config_hash(recompiler, toml)
        command = [
            args.compiler, "-std=c11", "-O0", "-Wall", "-Wextra",
            "-DPSX_NO_DEBUG_TOOLS", "-DPSX_OVERLAY_DLL_BUILD",
            f"-DPSX_OVERLAY_CODEGEN_HASH=0x{codegen:08x}u", f"-I{include}",
        ]
        if platform.system() != "Windows":
            command.append("-D_GNU_SOURCE")
        command += [
            str(RUNTIME / "src" / "overlay_loader.c"),
            str(RUNTIME / "src" / "overlay_path_canon.c"),
            str(RUNTIME / "src" / "overlay_posix.c"),
            str(RUNTIME / "src" / "crc32.c"),
            str(RUNTIME / "src" / "memory.c"),
            str(TESTS / "overlay_resident_patch_harness.c"),
            "-o", str(harness),
        ]
        if platform.system() != "Windows":
            command += ["-ldl", "-pthread"]
        run(command)

        harness_args = [str(harness), str(cache), GAME_ID, f"{config:08x}"]
        print(run(harness_args, env=env).strip())

        control = dict(env)
        control["PSX_OVERLAY_NATIVE_OFF"] = "1"
        control_output = run(harness_args, env=control, expect_ok=False)
        if "loaded patch: entry" not in control_output or \
                "UNEXPECTED" not in control_output:
            raise AssertionError(
                f"negative control did not detect the missing native run:\n"
                f"{control_output}")
        print("negative control: native-off run reports the mismatch")
    print("PASS: resident control-flow patches never run stale native code")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
