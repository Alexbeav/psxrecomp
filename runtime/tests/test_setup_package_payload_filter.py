#!/usr/bin/env python3
"""Require the setup packager to scrub and reject private payloads."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"
PRIVATE_PATH_GATE = ROOT / "tools" / "check_private_paths.sh"
FRAMEWORK_STAGE = ROOT / "tools" / "stage_framework_tree.sh"
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
    # The psxrecomp side of the drop list lives in stage_framework_tree.sh, which
    # the packager calls and the kit-emitter-rebuild CI job calls too; only the
    # recomp-ui side is still dropped by the packager itself. Assert the
    # delegation as well, so moving the list cannot silently unguard it.
    assert "stage_framework_tree.sh" in text
    framework_text = FRAMEWORK_STAGE.read_text(encoding="utf-8")
    for private_source in (
        "CLAUDE.md",
        "docs/internal",
        "recompiler/lib/ELFIO/tests",
        "tools/aot_overlay_spike",
        "tools/tasreplays",
    ):
        assert private_source in framework_text, private_source
    for private_source in ("recomp-ui/docs/HANDOFF.md", "recomp-ui/test_data"):
        assert private_source in text, private_source
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
