#!/usr/bin/env python3
"""Pin the dirty-RAM function-override entry route."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DIRTY = (ROOT / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    helper = "func_override_try_dispatch(cpu, target, cpu->gpr[31])"
    pump = DIRTY.index("dirty_ram_pump_boundary(cpu, target, 1)")
    tail = DIRTY.index(helper, pump)
    local = DIRTY.index("if (allow_local_dirty_flow", tail)
    update = DIRTY.index("current_function_entry_phys = target_phys", local)
    require(pump < tail < local < update,
            "J/JR override entry must follow the IRQ safe point and precede local flow")
    require("!g_precise_mode && !g_ls_replay_active" in DIRTY[pump:local],
            "tail override entry must keep precise/replay plain-transfer policy")
    require("((insn >> 21) & 0x1Fu) != 31u" in DIRTY[pump:local],
            "JR $ra returns must not be treated as function tail entries")
    require("current_function_entry_phys" in DIRTY[pump:tail],
            "J/JR override entry must track current function provenance")
    require("target_is_function_entry" in DIRTY[pump:tail],
            "J/JR override entry must prove the target function entry")
    require("source_phys = pc & 0x1FFFFFFFu" in DIRTY[pump:tail],
            "J/JR override entry must use the current transfer source")
    require("target_phys != current_function_entry_phys" in DIRTY[pump:tail],
            "a back-edge to the current function entry must not re-consult")
    require("overlay_loader_is_candidate(target_phys)" in DIRTY[pump:tail],
            "dynamic tail entry must require an exact overlay entry")
    require("psx_game_is_function_entry(target)" in DIRTY[pump:tail],
            "static tail entry must require a generated function entry")
    print("function override entry routes passed")


if __name__ == "__main__":
    main()
