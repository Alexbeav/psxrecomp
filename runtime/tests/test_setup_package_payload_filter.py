#!/usr/bin/env python3
"""Require the setup packager to scrub and reject private payloads."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "tools" / "package_setup_host.sh"
PRIVATE_PATH_GATE = ROOT / "tools" / "check_private_paths.sh"
ATTRIBUTES = ROOT / ".gitattributes"
WRAPPER_TEMPLATE = ROOT / "tools" / "new_project_layout" / "templates" / "package_setup_release.sh.in"


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
    for private_source in (
        "CLAUDE.md",
        "docs/internal",
        "recompiler/lib/ELFIO/tests",
        "tools/aot_overlay_spike",
        "recomp-ui/docs/HANDOFF.md",
    ):
        assert private_source in text
    gate_text = PRIVATE_PATH_GATE.read_text(encoding="utf-8")
    assert "developer-machine path" in gate_text
    for token in ("Users", "Projects", "AgentData", "OneDrive", "Share", "/mnt/"):
        assert token in gate_text
    assert "*.sh text eol=lf" in ATTRIBUTES.read_text(encoding="utf-8")
    assert "--project-file project-manifest.toml" in WRAPPER_TEMPLATE.read_text(encoding="utf-8")
    wrapper = WRAPPER_TEMPLATE.read_text(encoding="utf-8")
    assert "--runtime-dir mods" in wrapper
    assert 'find "${ROOT}/launcher_assets" -type f -print -quit' in wrapper
    assert "--exclude 'test_data'" in text
    assert text.count('rm -rf "${STAGE}/recomp-ui/test_data"') == 2
    print("setup package payload filter test: PASS")


if __name__ == "__main__":
    main()
