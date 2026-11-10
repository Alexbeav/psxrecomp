"""Guard the deterministic savestate completion receipt used by load gates."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/savestate.h").read_text(encoding="utf-8")
STATE = (ROOT / "src/savestate.c").read_text(encoding="utf-8")
SERVER = (ROOT / "src/debug_server.c").read_text(encoding="utf-8")
INTERRUPTS_H = (ROOT / "include/interrupts.h").read_text(encoding="utf-8")
INTERRUPTS = (ROOT / "src/interrupts.c").read_text(encoding="utf-8")
REWIND = (ROOT / "src/psx_rewind.c").read_text(encoding="utf-8")
DIRTY = (ROOT / "src/dirty_ram_interp.c").read_text(encoding="utf-8")
BOOT_STATE_H = (ROOT / "include/boot_state.h").read_text(encoding="utf-8")
DMA = (ROOT / "src/dma.c").read_text(encoding="utf-8")
BOOT_STATE = (ROOT / "src/boot_state.c").read_text(encoding="utf-8")

# The per-word DMA2 cursor and XA DATA_END pending bit grow the snapshot wire.
# Composed v17 carries replay and modern device state; v18 additionally
# requires the trusted host provider. Reject legacy layouts before mutation.
assert "#define DMA_GPU_LL_WIRE (4u + (10u * 4u))" in DMA
assert "#define BOOT_STATE_VERSION 18u" in BOOT_STATE_H
assert "#define BOOT_STATE_VERSION_WITHOUT_HOST 17u" in BOOT_STATE_H
assert "h.version       = BOOT_STATE_VERSION_WITHOUT_HOST;" in BOOT_STATE
assert "h.version = BOOT_STATE_VERSION;" in BOOT_STATE
assert "if (h.version == BOOT_STATE_VERSION && !(seen & (1u << BS_SEC_MOD_HOST))) goto cleanup;" in BOOT_STATE
assert "if (h.version == BOOT_STATE_VERSION_WITHOUT_HOST && (seen & (1u << BS_SEC_MOD_HOST))) goto cleanup;" in BOOT_STATE
assert "(1u<<BS_SEC_GAME_START)|(1u<<BS_SEC_MEMCTRL)|(1u<<BS_SEC_SPUCD)|" in BOOT_STATE
assert "#define BOOT_STATE_VERSION_MIN_READ 17u" in BOOT_STATE_H
assert "Neither legacy\n * replay v16 nor modern v9/v10 is wire compatible with this layout." in BOOT_STATE_H

assert "void savestate_status_json(char* buf, size_t cap);" in HEADER
assert '\\"generation\\"' in STATE and '\\"pending\\"' in STATE
assert "s_status_generation++" in STATE
assert '"savestate_status"' in SERVER
assert "savestate_status_json(status, sizeof status)" in SERVER
assert "int psx_irq_resume_context_snapshot_safe(void);" in INTERRUPTS_H
assert "int psx_irq_resume_context_snapshot_safe_at(uint32_t resume_pc);" in INTERRUPTS_H
assert "uint32_t psx_irq_resume_context_snapshot_pc(void);" in INTERRUPTS_H
assert "psx_irq_resume_context_snapshot_safe(void)" in INTERRUPTS
assert "psx_irq_resume_context_snapshot_safe_at(uint32_t resume_pc)" in INTERRUPTS
assert "psx_irq_resume_context_snapshot_pc(void)" in INTERRUPTS
assert "g_cosim_dirty_pump_site == 0" in INTERRUPTS
assert INTERRUPTS.index('if (psx_mod_function_entry_active()) return 0;') < INTERRUPTS.index('if (g_cosim_dirty_pump_site == 0)')
assert 'if (psx_mod_function_entry_active()) { s_status_wait = "mod_callback"; return; }' in STATE
assert 'if (!psx_mod_snapshot_ready()) { s_status_wait = "mod_snapshot"; return; }' in STATE
assert "case 1: /* transfer surface: target PC is materialized in CPUState */" in INTERRUPTS
assert "cands[n++] = psx_irq_resume_context_snapshot_pc();" in STATE
assert "cands[n++] = psx_irq_resume_context_snapshot_pc();" in REWIND
assert "int pc_ok = savestate_resume_pc_ok(pc);" in STATE
assert "(resume_pc == 0u && !pc_matches_cpu) || !snapshot_safe || !pc_ok" in STATE
assert "!psx_irq_resume_context_snapshot_safe_at(pc) || !resume_pc_ok(pc)" in REWIND
assert "psx_check_interrupts_at(cpu, pc)" in DIRTY
print("savestate status protocol guard passed")

# The debug server names an out-of-range slot instead of the generic refusal.
_handler = SERVER[SERVER.index("static void handle_savestate(int id"):]
_handler = _handler[:_handler.index("\n}\n")]
assert "slot >= SAVESTATE_SLOTS" in _handler
assert _handler.index("slot >= SAVESTATE_SLOTS") < _handler.index("savestate_request_save(slot)")
assert "out of range (0..%d)" in _handler
