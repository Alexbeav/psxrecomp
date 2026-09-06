#!/usr/bin/env python3
"""Regression for game-selected retail BIOS profiles in psxrecomp_cli.py."""

from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

import psxrecomp_cli  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        project = Path(temporary) / "game"
        framework = project / "psxrecomp"
        profile = framework / "bios" / "SCPH5552.toml"
        profile.parent.mkdir(parents=True)
        profile.write_text(
            "[program]\n"
            'id = "SCPH-5552"\n'
            'rom = "bios/SCPH5552.BIN"\n'
            "[recompiler]\n"
            'out_stem = "SCPH5552"\n',
            encoding="utf-8",
        )

        resolved = psxrecomp_cli.resolve_retail_bios_profile(
            project,
            framework,
            {"bios_config": "psxrecomp/bios/SCPH5552.toml"},
        )
        assert resolved == (
            "bios/SCPH5552.toml",
            "SCPH5552",
            (framework / "bios" / "SCPH5552.BIN").resolve(),
            "SCPH-5552",
        )

        outside = Path(temporary) / "outside.toml"
        outside.write_text("[program]\n", encoding="utf-8")
        try:
            psxrecomp_cli.resolve_retail_bios_profile(
                project, framework, {"bios_config": str(outside)}
            )
        except ValueError:
            pass
        else:
            raise AssertionError("an external BIOS profile was accepted")

    print("PASS: game-selected retail BIOS profile resolves inside the framework")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
