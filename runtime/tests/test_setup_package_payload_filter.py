#!/usr/bin/env python3
"""Require the setup packager to scrub and reject private payloads."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"


def main() -> None:
    text = PACKAGER.read_text(encoding="utf-8")
    assert "The source tree can contain these files even when Git reports it clean." in text
    for suffix in ("*.cue*", "*.iso*", "*.chd*", "*.mcd*", "*.mcr*"):
        assert suffix in text
    assert "-iname '*.bin*'" in text
    assert "! -iname 'openbios.bin' -delete" in text
    assert "forbidden owned-input or player-state payload" in text
    assert "forbidden retail BIOS payload" in text
    assert "--omit-openbios) OMIT_OPENBIOS=1" in text
    assert '"${STAGE}/psxrecomp/bios/openbios.bin"' in text
    assert '"${STAGE}/psxrecomp/bios/OpenBIOS.toml"' in text
    assert '"${STAGE}/psxrecomp/bios/OpenBIOS.LICENSE"' in text
    assert "forbidden final BIOS payload" in text
    assert "*.mcd'" not in text
    assert "*.mcr'" not in text
    assert "elif command -v cmake" in text
    assert 'cmake -E tar cf "${DIST}/${ZIP_NAME}" --format=zip .' in text
    print("setup package payload filter test: PASS")


if __name__ == "__main__":
    main()
