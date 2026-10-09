#!/usr/bin/env python3
"""Regression guards for the self-contained CLI project pipeline."""

from pathlib import Path
import importlib.util
import os
import sys
import tempfile
from unittest.mock import patch
import zipfile


ROOT = Path(__file__).resolve().parents[2]


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def check_packaged_profiles() -> None:
    """Run the real packager with authored marker files, never BIOS images."""
    retired_seed = "phase2_ghidra_seeds_SCPH101.json"
    if (ROOT / "bios/SCPH101.toml").exists() or (
            ROOT / "recompiler/seeds" / retired_seed).exists():
        raise AssertionError("retired profile and seed must both be removed")
    profiles = {path.name for path in (ROOT / "bios").glob("*.toml")}
    spec = importlib.util.spec_from_file_location("build_cli", ROOT / "tools/build_cli.py")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)

    with tempfile.TemporaryDirectory(prefix="cli-package-test-") as directory:
        fixture = Path(directory)
        files = [
            "runtime/runtime.cmake", "recompiler/source-marker.txt",
            "recompiler/seeds/phase2_ghidra_seeds.json", "tools/embed_spirv.py",
            "bios/openbios.bin", "bios/OpenBIOS.LICENSE", ".gitignore",
            "LICENSE", "THIRD_PARTY_ATTRIBUTION.md", "README.md",
        ]
        files += [f"bios/{name}" for name in profiles]
        suffix = ".exe" if os.name == "nt" else ""
        files += [f"build/{name}{suffix}" for name in package.TOOLS]
        for name in files:
            destination = fixture / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(f"authored packaging fixture: {name}\n", encoding="utf-8")
        with patch.object(package, "ROOT", fixture), patch.object(
                sys, "argv", ["build_cli.py", "--skip-build", "--build-dir", "build"]):
            if package.main() != 0:
                raise AssertionError("CLI packager failed")

        archive, = (fixture / "dist").glob("*.zip")
        with zipfile.ZipFile(archive) as packaged:
            names = packaged.namelist()
            actual = {
                Path(name).name for name in names
                if "/framework/bios/" in name and name.endswith(".toml")
            }
            if actual != profiles:
                raise AssertionError(f"packaged profiles {actual} differ from source {profiles}")
            if any(Path(name).name in {"SCPH101.toml", retired_seed} for name in names):
                raise AssertionError("CLI package contains retired BIOS assets")
            for name in profiles:
                member, = (item for item in names if item.endswith(f"/framework/bios/{name}"))
                if packaged.read(member) != (fixture / "bios" / name).read_bytes():
                    raise AssertionError(f"CLI package changed profile {name}")


def main() -> int:
    cli = (ROOT / "recompiler/src/main_cli.cpp").read_text(encoding="utf-8")
    bios = (ROOT / "recompiler/src/main_bios.cpp").read_text(encoding="utf-8")
    package = (ROOT / "tools/build_cli.py").read_text(encoding="utf-8")
    new_project_cmake = (
        ROOT / "tools/new_project_layout/templates/CMakeLists.txt.in"
    ).read_text(encoding="utf-8")

    require(cli, 'bios_config = \\"psxrecomp/bios/{}\\"',
            "generated game.toml does not name its BIOS profile")
    require(cli, '"--config", fs::absolute(profile_destination).string()',
            "CLI BIOS recompilation does not use the selected profile")
    require(cli, '"--rom", options.bios.string()',
            "CLI BIOS recompilation does not pass the user-selected ROM")
    require(cli, '"--out-dir",',
            "CLI BIOS recompilation does not override the generated output")
    require(cli, 'framework_destination / "generated"',
            "BIOS sources are not emitted where runtime.cmake discovers them")
    require(cli, 'set(PSXRECOMP_BIOS_STEMS \\"{}\\"',
            "generated CMake does not select the emitted BIOS backend")
    require(cli, 'set(PSX_RECOMP_UI OFF CACHE BOOL \\"\\" FORCE)',
            "self-contained CLI projects must not require a game-owned "
            "recomp-ui checkout")
    require(cli, 'PSXRecomp runtime is missing:',
            "generated CLI project does not fail clearly when psxrecomp/ "
            "is missing")
    require(cli, 'git submodule update --init --recursive',
            "generated CLI project missing-runtime error lacks the git "
            "submodule recovery command")
    require(cli, '$RuntimeCMake = Join-Path $Root',
            "generated PowerShell build script lacks a runtime.cmake "
            "preflight")
    require(cli, '[ ! -f \\"$ROOT/psxrecomp/runtime/runtime.cmake\\" ]',
            "generated shell build script lacks a runtime.cmake preflight")
    require(new_project_cmake, 'PSXRecomp runtime is missing:',
            "new-project-layout CMake template does not fail clearly when "
            "psxrecomp/ is missing")
    require(new_project_cmake, 'git submodule update --init --recursive',
            "new-project-layout missing-runtime error lacks the git "
            "submodule recovery command")

    copy_pos = cli.find('fmt::print("[1/4] Copying build framework')
    game_pos = cli.find('fmt::print("[2/4] Recompiling game executable')
    if copy_pos < 0 or game_pos < 0 or copy_pos >= game_pos:
        raise AssertionError(
            "framework/profile must be copied before game recompilation")

    for profile in sorted(path.name for path in (ROOT / "bios").glob("*.toml")):
        require(package, f'"{profile}"',
                f"CLI package omits BIOS profile {profile}")
    require(package, 'shutil.copy2(ROOT / ".gitignore", framework)',
            "packaged framework lacks the project-root marker used by profiles")
    require(package, 'ROOT / "bios" / "openbios.bin"',
            "packaged framework omits the redistributable OpenBIOS image")
    require(package, 'ROOT / "bios" / "OpenBIOS.LICENSE"',
            "packaged framework omits the OpenBIOS license")
    require(package, 'ROOT / "tools" / "embed_spirv.py"',
            "packaged framework omits the Vulkan shader embedding tool")

    require(bios, 'a == "--rom"',
            "psxrecomp-bios config mode lacks the ROM override")
    require(bios, 'a == "--out-dir"',
            "psxrecomp-bios config mode lacks the output override")

    check_packaged_profiles()

    print("PASS: CLI profiles, stage order, BIOS overrides, and UI isolation agree")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
