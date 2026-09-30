#!/usr/bin/env python3
"""Guard bundled-only BIOS selection against stale hidden player paths."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

# A bundled-only product must bypass both explicit and cached player paths
# before validate_bios_for_launch can produce an impossible mismatch dialog.
assert "const bool bundled_only =" in MAIN
assert "if (!bundled_only) {" in MAIN
assert MAIN.index("if (!bundled_only) {") < MAIN.index(
    "if (!chosen.empty() && std::filesystem::exists(chosen))"
)

# The launcher must derive persistence from linked capabilities, not from the
# active image: psx_bios_image is deliberately zeroed until selection occurs.
assert "const bool bios_choice_supported =" in MAIN
assert "if (bios_choice_supported && ls.bios_path[0])" in MAIN
assert "ls.bios_path[0] && !psx_bios_image.image_bundled" not in MAIN

# A shipped bundled build links OpenBIOS only and carries no CLI, sources or
# toolchain. Its BIOS row must not be forced on, and a retail pick must not be
# answered with "Generate & rebuild": that offer is only real when the codegen
# host wired Generate (prepare_with_progress) or nothing is linked yet.
HOST = (ROOT / "host" / "psxrecomp_codegen_host.c").read_text(encoding="utf-8")
assert "gi->has_bios = 1;" not in HOST
assert MAIN.count("gi.has_bios = 1;") == 0
assert MAIN.count("g_lnch_can_regen = gi.prepare_with_progress != nullptr") == 2
assert "gi.has_bios = (psx_bios_has_selectable() || g_lnch_can_regen) ? 1 : 0;" in MAIN
assert "if (g_lnch_can_regen) {" in MAIN
assert MAIN.index("if (g_lnch_can_regen) {") < MAIN.index(
    '"Generate & rebuild to switch (or use OpenBIOS)."')
assert "cannot be selected. Clear the BIOS field to play." in MAIN

print("BIOS selection capability guards passed")
