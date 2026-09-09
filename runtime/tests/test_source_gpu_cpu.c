/* Authored RAM program through the production dirty-RAM CPU and GPU service.
 * The declared 11-instruction synthetic ROM prologue has no device work except
 * cache-control/SR setup and costs55 clocks. This fixture begins at its RAM
 * entry, with those explicit prologue results; it is not a BIOS/retail test. */
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "psx_icache.h"
#include "psx_cyc.h"
#include "source_gpu_runtime.h"
#include "gpu.h"
#include "gpu_render.h"
#include "timers.h"
#include "interrupts.h"
#include "dma.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
extern CPUState *debug_cpu_ptr;
extern void memory_init(const char *);
extern uint8_t *memory_get_ram_ptr(void);
extern void memory_set_sr_ptr(const uint32_t *);
extern void psx_write_word(uint32_t,uint32_t);
extern uint32_t psx_read_word(uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern uint16_t psx_read_half(uint32_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern uint8_t psx_read_byte(uint32_t);
static uint32_t labels[128],label_count,seen,observed;
static CPUState cpu;
extern uint32_t __real_gpu_read_gpustat(void);
uint32_t __wrap_gpu_read_gpustat(void) {
    SourceGPUCommandProjection s;source_gpu_runtime_copy(0,&s);
    uint32_t value=__real_gpu_read_gpustat();
    printf("gpustat %llu %08x %d %u %u %llu\n",(unsigned long long)psx_get_cycle_count(),value,s.budget,s.phase,s.count,(unsigned long long)s.last_update);
    return value;
}
static void trace(uint32_t pc) {
    assert(++observed<200000);
    if(observed<20)printf("trace %08x %llu\n",pc,(unsigned long long)psx_get_cycle_count());
    for(unsigned i=0;i<label_count;i++)if(labels[i]==pc) {
        SourceGPUCommandProjection s;source_gpu_runtime_copy(0,&s);
        printf("checkpoint %u %08x %llu",i,pc,(unsigned long long)psx_get_cycle_count());
        for(unsigned g=0;g<32;g++)printf(" %u",cpu.gpr[g]);
        printf(" %u %u %u %u %llu %u %u\n",(uint32_t)s.budget,s.phase,s.count,
            (unsigned)source_gpu_command_ready(&s),(unsigned long long)s.last_update,
            s.command,s.count?s.queue[0]:0);
        char name[64];snprintf(name,sizeof(name),"vram-%u.bin",i);
        FILE *f=fopen(name,"wb");assert(f);
        assert(fwrite(gpu_get_vram(),1,1024u*512u*2u,f)==1024u*512u*2u);
        assert(fclose(f)==0);seen++;
        snprintf(name,sizeof(name),"ram-%u.bin",i);f=fopen(name,"wb");assert(f);
        assert(fwrite(memory_get_ram_ptr(),1,0x3000,f)==0x3000);assert(fclose(f)==0);
    }
}
int main(int argc,char **argv) {
    assert(argc==4);
    setvbuf(stdout,0,_IONBF,0);
    memory_init(argv[1]);timers_init();dma_init();interrupts_init();
    gr_init((uint16_t*)gpu_get_vram());gpu_init();
    FILE *f=fopen(argv[2],"rb");assert(f);
    assert(fread(memory_get_ram_ptr(),1,0x3000,f)==0x3000);fclose(f);
    f=fopen(argv[3],"r");assert(f);
    while(label_count<128 && fscanf(f,"%x",labels+label_count)==1)label_count++;
    fclose(f);assert(label_count);
    cpu.pc=0x80000500;cpu.gpr[11]=0xfffe0130;cpu.gpr[12]=0x800;
    cpu.read_word=psx_read_word;cpu.write_word=psx_write_word;
    cpu.read_half=psx_read_half;cpu.write_half=psx_write_half;
    cpu.read_byte=psx_read_byte;cpu.write_byte=psx_write_byte;
    cpu.gpr[25]=0x80000500;cpu.gpr[31]=0xbfc00200;
    cpu.read_fudge=32;cpu.ld_which_t=32;debug_cpu_ptr=&cpu;
    memory_set_sr_ptr(&cpu.cop0[12]);psx_icache_reset();g_psx_icache_active=1;
    psx_write_word(0xfffe0130,0x800);
    dirty_ram_reset_for_boot();dirty_ram_mark_executable_range(0x500,0xb00);
    source_gpu_runtime_init();psx_advance_cycles(55);psx_devices_service_to_now();
    g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=trace;
    for(unsigned blocks=0;cpu.pc && cpu.pc!=0xbfc00200;blocks++) {
        assert(blocks<50000);assert(dirty_ram_dispatch(&cpu,cpu.pc,0xbfc00200));
    }
    psx_cyc_batch_flush();assert(seen==label_count);
    printf("complete %u %u %llu\n",seen,observed,(unsigned long long)psx_get_cycle_count());
    return 0;
}
