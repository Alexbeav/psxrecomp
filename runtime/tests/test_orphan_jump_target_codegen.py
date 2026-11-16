"""Compile and execute admitted authored indirect-jump bodies, never fallback."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from source_fixture_link import build_and_run


def run(args, out):
    here = Path(__file__).resolve().parent
    subprocess.run([str(args.bios_emitter.resolve()), str(out), 'orphan-targets'], check=True)
    for name in ('orphan-jr', 'orphan-jalr', 'complete-jr', 'complete-jalr',
                 'orphan-j', 'orphan-jalr-link'):
        lane = out / name
        admission = json.loads((lane / 'admission.json').read_text())
        assert admission == {'emitted': 1 if name.startswith('complete') else 2,
                             'skipped': 0, 'interpreted': 0}, (name, admission)
        source = lane / 'Test_full.c'
        definitions = source.read_text().count('uint32_t psx_jrt_BFC00008 = 0;')
        assert definitions == (0 if name == 'orphan-j' else 1), (name, definitions)
        link = 'jalr' in name
        read_link = name.endswith('-link')
        direct = name == 'orphan-j'
        expected_t0 = 0xbfc00040 if read_link or direct else 7
        expected_ra = 0xbfc00010 if link else 0
        expected_s0 = 0xbfc00010 if read_link else 9 if direct else 0
        driver = lane / 'driver.c'
        driver.write_text(r'''
#ifdef NDEBUG
#error Assertions must remain enabled
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "cpu_state.h"
extern void Test_func_1FC00000(CPUState *);
static unsigned native_entries, irq_checks;
void debug_server_log_call_entry(uint32_t p) {
    assert(p == 0x1fc00000u); ++native_entries;
}
void debug_server_cyc_observe(uint32_t p) { (void)p; }
void psx_check_interrupts_at(CPUState *c, uint32_t p) {
    (void)c; assert(p == 0xbfc00040u); ++irq_checks;
}
void psx_check_interrupts(CPUState *c) { (void)c; assert(0); }
void psx_pgxp_alu(CPUState *c,uint32_t i,uint32_t v,uint32_t a,uint32_t b) {
    (void)c;(void)i;(void)v;(void)a;(void)b;
}
void psx_pgxp_move(CPUState *c,uint32_t i,uint32_t v) {(void)c;(void)i;(void)v;}
int g_psx_call_bail, g_exc_escape_reason, g_rfe_escape_pending;
int main(void) {
    CPUState c = {0}; c.pc = 0xbfc00000u;
    c.read_fudge = c.ld_which_t = 32;
    Test_func_1FC00000(&c);
    assert(native_entries == 1 && irq_checks == 1);
    assert(c.pc == 0xbfc00040u && c.gpr[0] == 0 && c.load_value_rt == 0);
''' + f'    assert(c.gpr[8] == {expected_t0}u);\n'
            + f'    assert(c.gpr[31] == {expected_ra}u);\n'
            + f'    assert(c.gpr[16] == {expected_s0}u);\n'
            + '    puts("PASS generated target/slot/link; no fallback executed"); return 0;\n}\n')
        for opt in ('-O0', '-O2'):
            work = lane / opt[1:]
            work.mkdir(exist_ok=True)
            build_and_run(args.cc, lane, here.parent, opt, work,
                          [str(source)], str(driver))
            print(name, opt, 'PASS admitted generated body')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--bios-emitter', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        run(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory() as temp:
            run(args, Path(temp))
