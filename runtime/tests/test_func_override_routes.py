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
    require(pump < tail < local,
            "J/JR override entry must follow the IRQ safe point and precede local flow")
    require("!g_precise_mode && !g_ls_replay_active" in DIRTY[pump:local],
            "tail override entry must keep precise/replay plain-transfer policy")
    require("((insn >> 21) & 0x1Fu) != 31u" in DIRTY[pump:local],
            "JR $ra returns must not be treated as function tail entries")
    require("current_is_function_entry" in DIRTY[pump:tail],
            "J/JR override entry must prove the current function entry")
    require("target_is_function_entry" in DIRTY[pump:tail],
            "J/JR override entry must prove the target function entry")
    require("current_is_function_entry && target_is_function_entry" in
            DIRTY[pump:tail],
            "J/JR override entry must require both proven endpoints")
    require("overlay_loader_is_candidate(target_phys)" in DIRTY[pump:tail],
            "dynamic tail entry must require an exact overlay entry")
    require("psx_game_is_function_entry(target)" in DIRTY[pump:tail],
            "static tail entry must require a generated function entry")
    require("target_phys != phys" in DIRTY[pump:tail],
            "a jump back to the current function entry must not re-consult")
    print("function override entry routes passed")


if __name__ == "__main__":
    main()
