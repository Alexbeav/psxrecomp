#!/usr/bin/env python3
"""Pin the generated and dirty-RAM function-override entry routes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DIRTY = (ROOT / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
EMITTER = (ROOT / "recompiler/src/full_function_emitter.cpp").read_text(
    encoding="utf-8"
)


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
    require("func_override_try_dispatch(cpu, addr, cpu->gpr[31])" in EMITTER,
            "generated dispatch must use the continuation-preserving helper")
    require("g_psx_func_override_hook(cpu, addr & 0x1FFFFFFFu)) {\n"
            "            cpu->pc = cpu->gpr[31];" not in EMITTER,
            "generated dispatch must not overwrite non-local continuations")
    print("function override entry routes passed")


if __name__ == "__main__":
    main()
