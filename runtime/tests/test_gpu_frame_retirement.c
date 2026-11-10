/* Bounded authored uncached-RAM loop at a retained source pre-fetch boundary.
 * Include the actual interpreter to observe its instruction/slot ownership;
 * link remaining production device/scheduler objects without rendering init.
 * No candidate command clock is enabled by this test. */
#include "../src/dirty_ram_interp.c"
#include "gpu.h"
#include "timers.h"
#include "dma.h"
#include "psx_bios_backend.h"
#include "source_gpu_runtime.h"
#include <assert.h>
extern void memory_init(const char *);
extern void memory_set_sr_ptr(const uint32_t *);
extern uint32_t psx_read_word(uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern uint8_t psx_read_byte(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
extern CPUState *debug_cpu_ptr;
extern void psx_rfe_escape_check(CPUState *);
static CPUState *observed;
static unsigned boundary_pending, boundary_count, store_count, handler_count;
#ifdef TEST_GPU_CONSUMER
static void (*consumer)(CPUState*,uint32_t,uint64_t);
#endif
static uint32_t ram_word(unsigned offset) {
    uint32_t value;memcpy(&value,memory_get_ram_ptr()+offset,4);return value;
}
static void counted_write(uint32_t address,uint32_t value) {
    if((address&0x1fffffff)==0x1000)store_count++;
    if((address&0x1fffffff)==0x1004)handler_count++;
    psx_write_word(address,value);
}
static void authored_call(CPUState *cpu,uint32_t pc,uint32_t stop) {
    for(unsigned i=0;i<100;i++) {
        if(pc==stop || pc==0xbfc00200u){cpu->pc=pc;return;}
        cpu->pc=pc;
        psx_check_interrupts_at(cpu,pc);
        assert(dirty_ram_dispatch(cpu,pc,stop));
        psx_rfe_escape_check(cpu);pc=cpu->pc;assert(pc);
    }
    assert(!"authored dispatch bound");
}
static void authored_dispatch(CPUState *cpu,uint32_t pc) {
    authored_call(cpu,pc,cpu->cop0[14]);
}
static const PsxBiosImageInfo authored_info={.image_id="AUTHORED-FRAME-IRQ",.image_sha256="authored"};
static const PsxBiosBackend authored_backend={&authored_info,authored_dispatch,authored_call,0,0};
static void blank(void) {
    boundary_pending=1;
    printf("vblank %llu\n",(unsigned long long)psx_get_cycle_count());
}
static void sample(uint32_t pc) {
    printf("before_fetch %08X %llu\n",pc,(unsigned long long)psx_get_cycle_count());
}
static void functional_boundary(CPUState *cpu,uint32_t pc,uint64_t cycle) {
    assert(cpu==observed);
#ifdef TEST_GPU_CONSUMER
    consumer(cpu,pc,cycle);
    SourceGPUServiceClock clock;SourceGPUCommandProjection s;
    source_gpu_runtime_copy_return(&clock,&s);
    if(clock.frame_returns>boundary_count) {
        printf("gpu_return %llu %u %u %u %u %llu %u %u\n",(unsigned long long)clock.cycle,(uint32_t)s.budget,s.phase,s.count,(unsigned)source_gpu_command_ready(&s),(unsigned long long)s.last_update,s.command,s.count?s.queue[0]:0);
    }
#endif
    if(boundary_pending && observed) {
        boundary_pending=0;boundary_count++;
        printf("boundary %08X %llu %u %u %u %u %u\n",pc,
            (unsigned long long)cycle,observed->gpr[8],
            ram_word(0x1000),ram_word(0x1004),observed->cop0[12],observed->cop0[14]);
        assert(cycle==1117177);
        assert(!ram_word(0x1000) && !ram_word(0x1004));
    }
}
int main(int argc,char **argv) {
    assert(argc==3 || argc==6);memory_init(argv[1]);
    unsigned ram_size=argc==3?0x3000:0x2000;
    FILE *ram=fopen(argv[2],"rb");assert(ram);assert(fread(memory_get_ram_ptr(),1,ram_size,ram)==ram_size);fclose(ram);
    timers_init();dma_init();interrupts_init();
    if(argc==3)assert(psx_read_word(0xa0000200)==0x08000080 && psx_read_word(0xa0000204)==0);
    CPUState cpu={0};cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
    cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
    cpu.read_fudge=32;cpu.ld_which_t=32;cpu.cop0[13]=0x400;
    debug_cpu_ptr=&cpu;memory_set_sr_ptr(&cpu.cop0[12]);psx_icache_reset();g_psx_icache_active=1;
    if(argc==6) {
        int irq=atoi(argv[3]),branch=atoi(argv[4]);observed=&cpu;
        cpu.gpr[8]=10;cpu.gpr[16]=0x1000;cpu.gpr[17]=0x1004;
        cpu.gpr[24]=0xbfc00200u;cpu.gpr[29]=0x1ff000;
        cpu.cop0[12]=irq==2?1:0x101;cpu.cop0[13]=irq?0x100:0;
        cpu.write_word=counted_write;cpu.pc=0xa0000508u;
        dirty_ram_mark_executable_range(0,0x2000);psx_bios_activate(&authored_backend);
#ifdef TEST_GPU_CONSUMER
        source_gpu_runtime_init();consumer=g_psx_cpu_step_boundary_callback;
#endif
        psx_advance_cycles(1117172);psx_devices_service_to_now();
        gpu_set_vblank_callback(blank);g_input_instruction_histogram_active=atoi(argv[5]);
        g_input_instruction_histogram_callback=sample;g_precise_mode=1;g_dirty_interp_active=1;
        g_psx_cpu_step_boundary_callback=functional_boundary;
        if(irq==1)psx_check_interrupts_at(&cpu,cpu.pc);
        assert(cpu.pc==0xa0000508u);
        uint32_t next=0;int moved=exec_one(&cpu,cpu.pc,&next);psx_cyc_batch_flush();
        uint32_t continuation=moved?cpu.pc:next;
        if(!branch)psx_cpu_step_boundary(&cpu,continuation);
        assert(boundary_count==1 && !boundary_pending);
        assert(cpu.gpr[8]==11 && !ram_word(0x1000));
        authored_call(&cpu,continuation,0xbfc00200u);psx_cyc_batch_flush();
        assert(cpu.gpr[8]==11 && ram_word(0x1000)==11 && store_count==1);
        assert(handler_count==(irq==1) && ram_word(0x1004)==(irq==1));
        assert(cpu.cop0[14]==(irq==1?0xa0000508u:0));
        printf("final %08X %u %u %u %u %u\n",cpu.pc,cpu.gpr[8],ram_word(0x1000),store_count,handler_count,boundary_count);
        return 0;
    }
    /* Source callback1117166 observes this uncached J after its four-cycle fetch.
     * Advance real devices from cold to the corresponding pre-fetch boundary. */
    psx_advance_cycles(1117162);psx_devices_service_to_now();
    gpu_set_vblank_callback(blank);
    g_input_instruction_histogram_active=1;g_input_instruction_histogram_callback=sample;
    g_precise_mode=1;g_dirty_interp_active=1;
    uint32_t pc=0xa0000200;
    for(unsigned i=0;i<3;++i) {
        uint32_t next=0;cpu.pc=pc;
        int moved=exec_one(&cpu,pc,&next);psx_cyc_batch_flush();
        printf("pair_return next_out=%08X cpu_pc=%08X %llu %d\n",next,cpu.pc,(unsigned long long)psx_get_cycle_count(),moved);
        pc=moved?cpu.pc:next;assert(pc==0xa0000200);
    }
    return 0;
}
