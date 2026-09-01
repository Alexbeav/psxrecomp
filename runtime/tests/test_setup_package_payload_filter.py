#!/usr/bin/env python3
"""Require the setup packager to scrub and reject private payloads."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"
ATTRIBUTES = ROOT / ".gitattributes"


def main() -> None:
    text = PACKAGER.read_text(encoding="utf-8")
    for suffix in ("*.cue*", "*.iso*", "*.chd*", "*.mcd*", "*.mcr*"):
        assert suffix in text
    assert "-iname '*.bin*'" in text
    assert "! -iname 'openbios.bin' -delete" in text
    assert "forbidden owned-input or player-state payload" in text
    assert "forbidden retail BIOS payload" in text
    assert text.count("assert_no_private_payload") >= 3
    assert "assert_no_private_build_paths" in text
    assert text.count("assert_no_private_build_paths") >= 3
    for private_source in (
        "CLAUDE.md",
        "docs/internal",
        "recompiler/lib/ELFIO/tests",
        "tools/aot_overlay_spike",
        "recomp-ui/docs/HANDOFF.md",
    ):
        assert private_source in text
    assert "developer-machine path" in text
    assert "Users[\\\\/]\\.\\.\\." in text
    assert "*.sh text eol=lf" in ATTRIBUTES.read_text(encoding="utf-8")
    print("setup package payload filter test: PASS")


if __name__ == "__main__":
    main()
