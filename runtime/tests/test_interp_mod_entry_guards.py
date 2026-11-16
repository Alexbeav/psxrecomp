"""Keep local interpreted control flow from bypassing mod entry callbacks."""

from pathlib import Path
import re


def main():
    source = (Path(__file__).resolve().parents[1] /
              'src/dirty_ram_interp.c').read_text(encoding='utf-8')
    context_start = re.search(r'static int exec_one_fetched_context\([^;{]*\)\s*\{', source)
    assert context_start, 'shared execution owner missing'
    context = source[context_start.start():]
    context = context[:context.index('static int exec_one_fetched(')]
    hook = 'if (g_psx_mod_instruction_hooks) psx_mod_instruction(cpu, pc, insn);'
    assert context.count('psx_mod_instruction(cpu, pc, insn)') == 1
    assert hook in context
    assert context.index('psx_cpu_step_boundary(cpu,pc)') < context.index('psx_load_value_begin(cpu)') < context.index(hook)
    # Ordinary execution delegates to the same hook and delayed-value owner.
    wrapper = re.search(r'static int exec_one_fetched\([^;{]*\)\s*\{([^}]+)\}', source)
    assert wrapper and 'return exec_one_fetched_context(cpu,pc,insn,next_pc_out,0,0,0);' in wrapper.group(1)
    start = source.index('static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {')
    source = source[start:]
    static_dispatch = source[source.index('/* B-2: statically-compiled'):]
    static_dispatch = static_dispatch[:static_dispatch.index('/* A-1:')]
    assert static_dispatch.index('g_exec_phase = 3;') < static_dispatch.index(
        'psx_overlay_dispatch(cpu, addr);') < static_dispatch.index(
        'g_exec_phase = previous_phase;') < static_dispatch.index('if (handled) return 1;')
    assert source.count('psx_mod_function_entry(cpu, addr)') == 1
    assert source.count('psx_mod_try_function_replacement(cpu, addr)') == 1
    # Every interpreted entry reaches these calls: without an active hook they
    # must cost one load, never a call into the plugin table.
    # A forced span replay (psx_mod_run_guest_span) runs inside a hook
    # already: it must not re-enter the entry hooks, so the check leads.
    assert ('if (!forced && g_psx_mod_function_entry_hooks && '
            'psx_mod_function_entry(cpu, addr))\n            return 1;') in source
    assert 'if ((g_psx_mod_function_entry_hooks &&\n' in source
    local_start = source.index('if (allow_local_dirty_flow && target != 0 &&')
    local_end = source.index('current_page = target_phys >> 12;', local_start)
    local = source[local_start:local_end]
    probe_start = local.index('if (psx_overlay_static_can_dispatch(target))')
    probe_end = local.index('#endif', probe_start)
    probe = local[probe_start:probe_end]
    assert 'g_dirty_interp_chain_target = target;' in probe
    assert 'OV_FPLOG_RET1();' in probe
    assert 'psx_overlay_dispatch(cpu' not in probe
    assert local.count('psx_mod_function_entry(cpu, target)') == 1
    assert local.count('psx_mod_try_function_replacement(cpu, target)') == 1
    assert local.index('overlay_loader_dispatch(cpu, target)') < local.index(
        'psx_mod_function_entry(cpu, target)') < local.index('pc = target;')
    completion = local[local.index('if ((g_psx_mod_function_entry_hooks &&'):]
    completion = completion[:completion.index('pc = target;')]
    assert 'g_dirty_interp_chain_target = cpu->pc;' in completion
    assert 'OV_FPLOG_RET1();' in completion
    print('interpreter mod entry guards: PASS')


if __name__ == '__main__':
    main()
