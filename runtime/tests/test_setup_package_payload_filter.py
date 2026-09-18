#!/usr/bin/env python3
"""Require the setup packager to scrub and reject private payloads."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"
FRAMEWORK_STAGER = ROOT / "tools" / "stage_framework_tree.sh"
PRIVATE_PATH_GATE = ROOT / "tools" / "check_private_paths.sh"
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
    assert 'check_private_paths.sh"' in text
    assert 'bash "${gate}" "${STAGE}"' in text
    # The psxrecomp half of the drop list lives in stage_framework_tree.sh, so
    # the CI kit-emitter-rebuild job and the shipped tree filter identically.
    # What must hold is that nothing private ships, not which file says so, so
    # check both -- but only after confirming the packager still calls the
    # stager, since the list is worthless if it never runs.
    assert 'bash "${SCRIPT_DIR}/stage_framework_tree.sh"' in text
    stager_text = FRAMEWORK_STAGER.read_text(encoding="utf-8")
    dropped = text + "\n" + stager_text
    for private_source in (
        "CLAUDE.md",
        "docs/internal",
        "recompiler/lib/ELFIO/tests",
        "tools/aot_overlay_spike",
        "tools/tasreplays",
        "recomp-ui/docs/HANDOFF.md",
        "recomp-ui/test_data",
    ):
        assert private_source in dropped, private_source
    gate_text = PRIVATE_PATH_GATE.read_text(encoding="utf-8")
    assert "developer-machine path" in gate_text
    for token in ("Users", "Projects", "AgentData", "OneDrive", "Share", "/mnt/"):
        assert token in gate_text
    assert "*.sh text eol=lf" in ATTRIBUTES.read_text(encoding="utf-8")
    assert "--omit-openbios) OMIT_OPENBIOS=1" in text
    assert '"${STAGE}/psxrecomp/bios/openbios.bin"' in text
    assert '"${STAGE}/psxrecomp/bios/OpenBIOS.toml"' in text
    assert '"${STAGE}/psxrecomp/bios/OpenBIOS.LICENSE"' in text
    assert "forbidden final BIOS payload" in text
    print("setup package payload filter test: PASS")


if __name__ == "__main__":
    main()
