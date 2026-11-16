"""Pin the dirty-RAM interpreter's [[mod_immediate.sites]] call site: the word
substitution must follow the guarded instruction hook (which compares the
original word) and precede every use of `insn` for decode and execution."""
import pathlib
import re

SRC = pathlib.Path(__file__).resolve().parents[1] / "src" / "dirty_ram_interp.c"


def test_interp_substitutes_after_instruction_hook():
    text = SRC.read_text(encoding="utf-8")
    patch = "if (g_psx_mod_immediate_live) insn = psx_mod_immediate_patch(pc, insn);"
    hook = "if (g_psx_mod_instruction_hooks) psx_mod_instruction(cpu, pc, insn);"
    for name in ("exec_one_fetched_context",):
        definition = re.search(r'static int ' + name + r'\([^;{]*\)\s*\{', text)
        assert definition, name + ': execution owner missing'
        start = definition.start()
        end = text.index("\nstatic int ", start + 10)
        body = text[start:end]
        assert body.count(patch) == 1, name
        assert body.index(hook) < body.index("const uint32_t original_instruction = insn;") < body.index(patch), name + ": hook guards the original word"
        # The shared execution owner decodes only after substituting the immediate.
        assert body.index(patch) < body.index("const uint32_t ld_op = op_field(insn)"), name

    wrapper = re.search(r'static int exec_one_fetched\([^;{]*\)\s*\{([^}]+)\}', text)
    assert wrapper and 'return exec_one_fetched_context(cpu,pc,insn,next_pc_out,0,0,0);' in wrapper.group(1)
    assert text.count(patch) == 1, 'every execution path uses the shared patch owner'


def test_interp_includes_header():
    text = SRC.read_text(encoding="utf-8")
    assert re.search(r'#include "mod_immediate\.h"', text)

def test_session_seals_before_scope_and_guest():
    text = (SRC.parent / "main.cpp").read_text(encoding="utf-8")
    body = text[text.index("auto start_mod_session ="):]
    assert body.index("psx_mod_immediate_begin_session(") < body.index("mod_runtime_activate_plugins();")
    assert body.index("mod_runtime_activate_plugins();") < body.index("mod_runtime_savestate_scope(")
    text = (SRC.parent / "mod_runtime.cpp").read_text(encoding="utf-8")
    body = text[text.index('extern "C" void mod_runtime_activate_plugins(void)'):]
    assert body.index("mod_reset_plugin_sessions();") < body.index("psx_mod_immediate_begin_activation();")
    assert body.index("mod_invoke_activation_plugin(plugin.id);") < body.index("psx_mod_immediate_end_activation();")



if __name__ == "__main__":
    test_interp_substitutes_after_instruction_hook()
    test_interp_includes_header()
    test_session_seals_before_scope_and_guest()
