#include "cpu_state.h"
#include "psx_bios_backend.h"
#include "psx_icache.h"
#include "psx_cyc.h"
#include "source_gpu_runtime.h"
#include "gpu.h"
#include "gpu_render.h"
#include "timers.h"
#include "dma.h"
#include "interrupts.h"
#include "dirty_ram_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
extern void psx_irq_set_cause_ptr(uint32_t*);
extern void memory_init(const char*);
extern uint8_t *memory_get_ram_ptr(void);
extern void memory_set_sr_ptr(const uint32_t*);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern void psx_rfe_escape_check(CPUState*);
extern CPUState *debug_cpu_ptr;
static unsigned events,compiled,sliced;
static CPUState *active;
static void trace(uint32_t pc) {
    if(pc==0x80000500 || pc==0x80000080)
        printf("event %08X %llu %08X %08X %u %u %u %08X\n",pc,(unsigned long long)psx_get_cycle_count(),active->cop0[14],active->cop0[12],active->gpr[24],active->gpr[25],psx_read_word(0x1000),active->cop0[13]);
    assert(++events<1000);
}
static void run(CPUState *cpu,uint32_t pc,uint32_t stop) {
    for(unsigned i=0;i<100;i++) {
        if(pc==stop || pc==0xbfc00200u){cpu->pc=pc;return;}
        cpu->pc=pc;psx_check_interrupts_at(cpu,pc);
        if(pc==0x80000540 && !psx_get_in_exception()) {
            if(psx_slice_block(cpu,pc,5,0))sliced++;
            else {
                compiled++;
                for(unsigned j=0;j<3;j++) {
                    if(!j)psx_icache_fetch(cpu,pc);else psx_cpu_step_boundary(cpu,pc+4*j);
                    psx_cyc_step(cpu,1u<<24);cpu->gpr[24]++;
                }
                psx_cpu_step_boundary(cpu,pc+12);psx_cyc_step(cpu,1u<<24);
                int taken=(psx_read_word(pc+12)>>26)==4?cpu->gpr[24]==0:cpu->gpr[24]!=0;
                psx_icache_fetch(cpu,pc+16);psx_cyc_step(cpu,0);
                cpu->pc=taken?0x80000554:0x80000554; /* ordinary BNE with one delay slot */
            }
        } else if((pc==0x80000500 || pc==0x80000554) && !psx_get_in_exception()) {
            /* Setup/tail are precision controls, leaving only the middle
             * pure-ALU block eligible for the compiled fast path under test. */
            assert(psx_slice_block(cpu,pc,4,1));
        } else assert(dirty_ram_dispatch(cpu,pc,stop));
        psx_cyc_batch_flush();psx_rfe_escape_check(cpu);pc=cpu->pc;assert(pc);
    }
    assert(!"authored dispatch bound");
}
static void dispatch(CPUState*c,uint32_t pc){run(c,pc,c->cop0[14]);}
static const PsxBiosImageInfo info={.image_id="AUTHORED-CACHE-IRQ",.image_sha256="authored"};
static const PsxBiosBackend backend={&info,dispatch,run,0,0};
int main(int argc,char **argv) {
    assert(argc==6);memory_init(argv[1]);
    FILE*f=fopen(argv[2],"rb");assert(f);assert(fread(memory_get_ram_ptr(),1,0x2000,f)==0x2000);fclose(f);
    timers_init();dma_init();interrupts_init();gr_init((uint16_t*)gpu_get_vram());gpu_init();
    CPUState c={0};active=&c;c.read_word=psx_read_word;c.read_half=psx_read_half;c.read_byte=psx_read_byte;c.write_word=psx_write_word;c.write_half=psx_write_half;c.write_byte=psx_write_byte;
    c.gpr[9]=atoi(argv[3]);c.gpr[11]=0x1f801120;c.gpr[12]=atoi(argv[4])?0x18:8;c.gpr[16]=0x1000;c.gpr[31]=0xbfc00200;c.cop0[12]=0x401;c.read_fudge=32;c.ld_which_t=32;
    debug_cpu_ptr=&c;memory_set_sr_ptr(&c.cop0[12]);psx_irq_set_cause_ptr(&c.cop0[13]);psx_write_word(0x1f801074,atoi(argv[5])?0:0x40);
    c.gpr[25]=0x80000500; /* declared synthetic ROM's entry-address register */
    dirty_ram_mark_executable_range(0,0x2000);psx_bios_activate(&backend);psx_icache_reset();g_psx_icache_active=1;psx_write_word(0xfffe0130,0x800);
    source_gpu_runtime_init();g_psx_precise_slice=1;
    g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=trace;
    run(&c,0x80000500,0xbfc00200);psx_cyc_batch_flush();
    printf("final %llu %u %u %u %u %08X %08X\n",(unsigned long long)psx_get_cycle_count(),c.gpr[24],psx_read_word(0x1000),compiled,sliced,c.cop0[14],psx_read_word(0x1004));
    fprintf(stderr,"timer count=%u mode=%X target=%u mask=%X stat=%X next=%u\n",timers_read(0x1f801120),timers_read(0x1f801124),timers_read(0x1f801128),psx_read_word(0x1f801074),psx_read_word(0x1f801070),timers_cycles_to_irq(~0u));
    assert(c.gpr[24]==3 && psx_read_word(0x1000)==9);
}
