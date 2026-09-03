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
    assert text.count("remove_non_sdk_helpers") >= 3
    assert "tomba1_extract.py" in text
    assert "tomba2_extract.py" in text
    assert "--exclude 'test_data'" in text
    assert 'rm -rf "${STAGE}/recomp-ui/test_data"' in text
    assert "*.sh text eol=lf" in ATTRIBUTES.read_text(encoding="utf-8")
    print("setup package payload filter test: PASS")


if __name__ == "__main__":
    main()
