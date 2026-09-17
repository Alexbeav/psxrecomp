"""Exercise production CPU load paths at CD event boundaries without retail assets."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def check(output, cc):
    runtime = Path(__file__).resolve().parents[1]
    memory = (runtime / 'src/memory.c').read_text(encoding="utf-8")
    def section(start, end):
        return memory[memory.index(start):memory.index(end)]
    production = section('static inline uint32_t psx_mmio_read_wait(', '/* Resolve PSX_LOAD_DELAY')
    production += section('static inline uint32_t psx_cyc_load_timing(', '/* Production value-read fast path')
    production += section('static int source_hblank_counter_sample(', '/* Deprecated uncharged passthroughs')
    harness = r'''
#define PSX_CYC_H /* This fixture starts at ReadMemory, after instruction setup. */
#define PSXRECOMP_PSX_ICACHE_H
#include "cpu_state.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define PSX_ENABLE_BLOCK_CYCLES
uint64_t psx_cycle_count;
static uint64_t deadline, sampled;
static unsigned flag, source_active=1;
static void psx_advance_cycles(uint32_t n) {
 psx_cycle_count+=n;if(psx_cycle_count>=deadline)flag=1;
}
static void psx_devices_service_to_now(void){psx_advance_cycles(0);}
static void psx_load_charge_cycles(uint32_t n){psx_advance_cycles(n);}
static uint32_t dma_cpu_read_penalty(void){return 0;}
static int source_gpu_runtime_active(void){return source_active;}
static int timers_source_hblank_counter_read(uint32_t a){(void)a;return 0;}
static int psx_load_delay_enabled(void){return 1;}
static void psx_cyc_base(CPUState *c){(void)c;}
static void psx_cyc_deps(CPUState *c,uint32_t m){(void)c;(void)m;}
static void psx_cyc_lds(CPUState *c){(void)c;}
static uint32_t psx_read_word(uint32_t a){(void)a;sampled=psx_cycle_count;return flag;}
static uint16_t psx_read_half(uint32_t a){return psx_read_word(a);}
static uint8_t psx_read_byte(uint32_t a){return psx_read_word(a);}
'''
    harness += production + r'''
int main(void) {
 unsigned count=0;
 for(unsigned active=0;active<2;active++)
 for(unsigned alias=0;alias<3;alias++)
 for(unsigned kind=0;kind<4;kind++)
 for(unsigned fudge=0;fudge<=2;fudge+=2)
 for(unsigned offset=0;offset<=28;offset++) {
  CPUState c={0};c.read_fudge=fudge?0x20:0;c.ld_which_t=32;
  unsigned width=kind==0?1:kind==1?2:4, completion=kind==3?1:2;
  uint32_t addr=0x1f801803u|(alias==1?0x80000000u:alias==2?0xa0000000u:0);
  source_active=active;psx_cycle_count=1000;deadline=1000+offset;flag=0;
  uint32_t value=kind==0?psx_cyc_load_byte(&c,addr,3,0):
   kind==1?psx_cyc_load_half_slow(&c,addr,3,0):
   kind==2?psx_cyc_load_word_slow(&c,addr,3,0):psx_cyc_lwc2_read(&c,addr);
  unsigned expected_sample=1000+fudge+(active?0:6*width);
  assert(sampled==expected_sample && value==(deadline<=expected_sample));
  assert(psx_cycle_count==1000+fudge+6*width+completion);
  assert(c.ld_absorb==6*width+completion);count++;
 }
 printf("PASS %u CD read boundary cases: widths, aliases, LWC2, source/default and event order\n",count);
}
'''
    output.mkdir(parents=True, exist_ok=True)
    source = output / 'cd-read-order.c'
    source.write_text(harness)
    for mode in ('O0', 'O2'):
        exe = output / ('cd-read-order-' + mode + '.exe')
        subprocess.run([cc, '-' + mode, '-I' + str(runtime / 'include'), str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        check(args.output, args.cc)
    else:
        with tempfile.TemporaryDirectory() as directory:
            check(Path(directory), args.cc)
