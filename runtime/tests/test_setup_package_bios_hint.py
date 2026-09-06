#!/usr/bin/env python3
"""Keep setup-package BIOS instructions title-specific."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"


def main() -> None:
    text = PACKAGER.read_text(encoding="utf-8")
    assert '--bios-hint) BIOS_HINT="${2:?}"; shift 2 ;;' in text
    assert "Provide ${DISC_HINT} and ${BIOS_HINT}." in text
    assert "Provide ${DISC_HINT} (and optional retail SCPH-1001 BIOS" not in text
    assert '${EXE_DIR}/bios/openbios.bin' in text
    assert '${STAGE}/bios/openbios.bin' in text
    assert "runtime and SDK OpenBIOS images differ" in text
    assert "runtime OpenBIOS license is missing" in text
    print("setup package BIOS hint test: PASS")


if __name__ == "__main__":
    main()
