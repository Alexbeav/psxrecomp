#include "source_gpu_runtime.h"
#include "psx_icache.h"
#include "psx_cycles.h"
#include "dma.h"
#include "source_cpu_boundary_probe.h"
#include "source_ram_page_probe.h"
#include <stdio.h>
#include <stdlib.h>
extern uint64_t g_psx_cycle_fast_limit;
static int enabled;
static SourceGPUServiceClock clock_state;
static SourceGPUCommandProjection command_state;
/* GPU Update consumes queued work before it advances raster state. The
 * event clock also predicts intermediate deadlines; draw parity must instead
 * follow only actual GPU service calls, plus ordinary GP1 register writes. */
static InputRouteRasterClock draw_raster;
static SourceGPUServiceClock return_clock;
static SourceGPUCommandProjection return_command;
static SourceGPUDispatchSink dispatch_sink;
void source_gpu_runtime_set_dispatch_sink(SourceGPUDispatchSink sink) {dispatch_sink=sink;}
/* Bounded leaf diagnosis. No guest reads, device service, or clock writes. */
static void command_probe(void) {
    static int initialized; static FILE *output;
    static unsigned long long low,high; static unsigned rows;
    if(!initialized) {
        initialized=1; const char *range=getenv("PSX_SOURCE_GPU_COMMAND_WINDOW");
        if(range) {
            if(sscanf(range,"%llu,%llu",&low,&high)!=2 || high<=low || high-low>1000000)abort();
            output=fopen("gpu-commands.tsv","wx");if(!output)abort();
            fputs("cycle\tbudget\tphase\tqueued\tkind\twords\n",output);
        }
    }
    uint64_t cycle=psx_get_cycle_count();
    if(output && cycle>=low && cycle<=high && command_state.dispatch.kind &&
       command_state.dispatch.kind!=SOURCE_GPU_DISPATCH_UPLOAD_WORD) {
        if(++rows>50000)abort();
        const SourceGPUCommandDispatch *d=&command_state.dispatch;
        unsigned first=d->kind==SOURCE_GPU_DISPATCH_QUAD_SECOND?
            d->count-source_gpu_polygon_stride(d->words[0]>>24):0;
        fprintf(output,"%llu\t%d\t%u\t%u\t%u\t",(unsigned long long)cycle,
                command_state.budget,command_state.phase,command_state.count,d->kind);
        for(unsigned i=first;i<d->count;i++)fprintf(output,"%s%08X",i>first?" ":"",d->words[i]);
        fputc('\n',output);
    }
    if(output && cycle>high){fclose(output);output=NULL;}
}
static void dispatch(void) {
    if(command_state.dispatch.kind && dispatch_sink)
        command_state.budget-=dispatch_sink(&command_state.dispatch);
    command_probe();
    command_state.dispatch.kind=SOURCE_GPU_DISPATCH_NONE;
}
static void fail(const char *reason) {
    fprintf(stderr,"[source-gpu-service] rejected draw mode=%04X (draw-to-display=%u)\n",command_state.draw_mode,(command_state.draw_mode>>10)&1u);
    if(command_state.count>=4 && command_state.queue[0]>>24==0x28) {
        fprintf(stderr,"[source-gpu-service] rejected first triangle vertices");
        for(unsigned i=1;i<4;i++)fprintf(stderr," %d,%d",source_gpu_command_coord(command_state.queue[i],0)+command_state.offset_x,source_gpu_command_coord(command_state.queue[i],16)+command_state.offset_y);
        fputc('\n',stderr);
    }
    /* Preserve the decisive packet on a bounded profile stop. */
    fprintf(stderr,"[source-gpu-service] queued words");
    for(unsigned i=0;i<command_state.count;i++)fprintf(stderr," %08X",command_state.queue[i]);
    fputc('\n',stderr);
    fprintf(stderr,"[source-gpu-service] %s at %llu; budget=%d phase=%u queued=%u front=%08X clip=%d,%d..%d,%d offset=%d,%d mode=%02X mask=%u\n",reason,(unsigned long long)psx_get_cycle_count(),command_state.budget,command_state.phase,command_state.count,command_state.count?command_state.queue[0]:0,command_state.clip_x0,command_state.clip_y0,command_state.clip_x1,command_state.clip_y1,command_state.offset_x,command_state.offset_y,command_state.display_mode,command_state.mask_bits);exit(2);
}
static void service(void *context,uint64_t cycle,unsigned kind) {
    (void)context;
    if(!source_gpu_command_update(&command_state,cycle))fail("unsupported command service");
    dispatch();
    if(cycle<draw_raster.cycle || cycle-draw_raster.cycle>UINT32_MAX)fail("invalid draw raster time");
    input_route_raster_advance(&draw_raster,(uint32_t)(cycle-draw_raster.cycle));
    command_state.skip_field=(draw_raster.y_start+draw_raster.readout_field)&1u;
    if(kind==SOURCE_GPU_EVENT_DMA || kind==SOURCE_GPU_EVENT_WRITE)
        dma_source_gpu_service_at(cycle);
}
static void cpu_boundary_inner(CPUState *cpu,uint32_t pc,uint64_t cycle);
/* Step-2 IRQ handoff for compiled code (TAS speed task). The precise
 * interpreter re-tests interrupt deliverability before every instruction; a
 * compiled block only did so at its leader, so a store to I_MASK/I_STAT (or
 * any write that makes a pending IRQ deliverable) inside the block moved the
 * take point to the next block boundary. Compiled code already reaches this
 * callback before every instruction under the source profile, so run the same
 * test here and deliver synchronously (the handler runs nested and returns,
 * exactly as compiled-code takes at block boundaries already do), with the
 * resume PC = this instruction so EPC matches the interpreter. Delay-slot
 * boundaries are skipped: a synchronous enable always becomes visible at a
 * non-slot boundary first (the branch itself, or the branch target when the
 * enabling store sits in the slot), and asynchronous device IRQs cannot land
 * inside a compiled block by the guard's deadline construction. */
int g_psx_slice_irq_handoff;   /* set by the guard when the phase variant is on */
int g_psx_slice_slot_take;     /* PSX_SLICE_SLOT_TAKE=1: take IRQs at delay-slot boundaries like exec_delay_slot */
uint64_t g_sd_handoff_slot_taken;
#include "psx_cyc.h"
#include "psx_icache.h"
#include "psx_instr_cost.h"   /* psx_cyc_dep_res_mask (static inline) */
int g_interp_boundary_in_progress; /* set by the interpreter around its own boundary call: it owns the IRQ test there */
extern int g_psx_irq_delivering;   /* interrupts.c: delivery is fetching the handler's first instruction */
extern void psx_check_interrupts(CPUState *cpu);
extern int memory_peek_instruction_word(uint32_t address,uint32_t *value);
uint64_t g_sd_handoff_taken, g_sd_handoff_slot_skipped; uint32_t g_sd_handoff_first_pc;
static int compiled_boundary_in_delay_slot(uint32_t pc) {
    uint32_t prev;
    if(!memory_peek_instruction_word(pc-4u,&prev)) return 0;
    uint32_t op=prev>>26,fn=prev&63u,rs=(prev>>21)&31u;
    if(op==2u || op==3u) return 1;                          /* j / jal */
    if(op==1u || (op>=4u && op<=7u)) return 1;              /* bcond / beq bne blez bgtz */
    if(op==0u && (fn==8u || fn==9u)) return 1;              /* jr / jalr */
    if(op>=0x10u && op<=0x13u && rs==8u) return 1;          /* bcZf / bcZt */
    return 0;
}
static int s_handoff_active;
static void compiled_irq_handoff(CPUState *cpu,uint32_t pc) {
    extern int g_precise_mode;
    /* g_dirty_interp_active stays set across native calls made from the dirty
     * interpreter, so it cannot discriminate; the interpreter flags its own
     * boundary calls instead. */
    if(!g_psx_slice_irq_handoff || g_precise_mode || g_interp_boundary_in_progress) return;
    if(s_handoff_active || g_psx_irq_delivering) return;   /* delivery's own handler fetch reaches this callback */
    extern int psx_precise_irq_before_public(CPUState *cpu,uint32_t pc);
    if(!psx_precise_irq_before_public(cpu,pc)) return;
    extern uint64_t g_irq_deliver_count; extern uint32_t g_dirty_safe_resume_pc;
    if(compiled_boundary_in_delay_slot(pc)) {
        /* Delay-slot boundary. The interpreter (exec_delay_slot) takes here with
         * EPC = branch and BD set, then re-executes the branch and the slot after
         * RFE. Mirror it (PSX_SLICE_SLOT_TAKE=1): recompute the branch's target and
         * taken flag from the branch word and the current registers (the slot has
         * not executed, so the branch's operands are as the branch saw them),
         * deliver through the same routine, then charge the branch's re-execution
         * (boundary + fetch + interlock step) before the compiled slot proceeds.
         * jalr with rd == rs and COP branches cannot be recomputed: skip as before. */
        extern int g_psx_slice_slot_take;
        if(!g_psx_slice_slot_take) { g_sd_handoff_slot_skipped++; return; }
        uint32_t bpc=pc-4u,bw,sw; if(!memory_peek_instruction_word(bpc,&bw) || !memory_peek_instruction_word(pc,&sw)) { g_sd_handoff_slot_skipped++; return; }
        uint32_t op=bw>>26,fn=bw&63u,rs=(bw>>21)&31u,rt=(bw>>16)&31u,rd=(bw>>11)&31u; int32_t simm=(int16_t)(bw&0xFFFFu);
        uint32_t target=0; int taken=0, ok=1;
        int32_t vs=(int32_t)cpu->gpr[rs], vt=(int32_t)cpu->gpr[rt];
        if(op==2u || op==3u) { target=(bpc&0xF0000000u)|((bw&0x03FFFFFFu)<<2); taken=1; }
        else if(op==0u && (fn==8u || fn==9u)) { if(fn==9u && rd==rs) ok=0; target=cpu->gpr[rs]; taken=1; }
        else if(op==4u) { target=pc+((uint32_t)simm<<2); taken=vs==vt; }
        else if(op==5u) { target=pc+((uint32_t)simm<<2); taken=vs!=vt; }
        else if(op==6u) { target=pc+((uint32_t)simm<<2); taken=vs<=0; }
        else if(op==7u) { target=pc+((uint32_t)simm<<2); taken=vs>0; }
        else if(op==1u) { target=pc+((uint32_t)simm<<2); taken=(rt&1u)?(vs>=0):(vs<0); if(rt&0x10u) { /* bltzal/bgezal: link written at branch */ } }
        else ok=0;
        if(!ok) { g_sd_handoff_slot_skipped++; return; }
        extern int psx_check_interrupts_delay_slot(CPUState *cpu,uint32_t slot_pc,uint32_t target,int taken,uint32_t instruction);
        uint64_t before=g_irq_deliver_count;
        s_handoff_active=1;
        int took=psx_check_interrupts_delay_slot(cpu,pc,target,taken,sw);
        s_handoff_active=0;
        if(took && g_irq_deliver_count!=before) {
            g_sd_handoff_slot_taken++;
            /* re-execution of the branch, as the interpreter does after RFE */
            cpu_boundary_inner(cpu,bpc,psx_get_cycle_count());
            psx_icache_fetch(cpu,bpc);
            psx_cyc_step(cpu,psx_cyc_dep_res_mask(bw));
            cpu->pc=0;   /* compiled code continues with the slot and its latched target */
        }
        return;
    }
    uint64_t before=g_irq_deliver_count;
    uint32_t previous=g_dirty_safe_resume_pc;
    cpu->pc=pc;g_dirty_safe_resume_pc=pc;
    s_handoff_active=1;
    psx_check_interrupts(cpu);              /* handler runs nested; returns here */
    s_handoff_active=0;
    g_dirty_safe_resume_pc=previous;
    if(g_irq_deliver_count!=before) { g_sd_handoff_taken++; if(!g_sd_handoff_first_pc) g_sd_handoff_first_pc=pc; }
}
static void cpu_boundary(CPUState *cpu,uint32_t pc,uint64_t cycle) {
    /* Same order as psx_run_precise: test deliverability BEFORE the boundary
     * work (which can charge DMA-halt/fetch cycles and would otherwise move
     * the take timestamp by that amount), then again after it, because the
     * boundary work itself can raise an interrupt. Full-route requalification
     * of the after-only version shifted two returns by 1-2 cycles inside a
     * DMA-halted wait loop. */
    compiled_irq_handoff(cpu,pc);
    cpu_boundary_inner(cpu,pc,cycle);
    compiled_irq_handoff(cpu,pc);
}
static void cpu_boundary_inner(CPUState *cpu,uint32_t pc,uint64_t cycle) {
    for(;;) {
        if(clock_state.frame_pending) {
            psx_devices_service_to_now();
            if(!source_gpu_service_cpu_boundary(&clock_state,cycle,service,0))fail("invalid CPU boundary");
            return_clock=clock_state;return_command=command_state;
            source_cpu_return_probe(cpu,pc,cycle,clock_state.frame_returns);
            source_ram_page_probe(clock_state.frame_returns,cycle);
            { extern uint64_t g_psx_device_gen; g_psx_device_gen++; }   /* frame end re-arms the GPU service clock */
            psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
        }
        dma_cpu_read_wait_boundary();
        source_cpu_boundary_probe(cpu,pc,cycle);
        if(!dma_cpu_source_halted())return;
        /* Original RunReal fetches before selecting its halt operation.
         * Cache fill therefore overlaps DMA, while only the base/absorb step
         * runs. No opcode effects or deferred-load writeback occur here.
         * COP2 bypasses the original interrupt/halt dispatch table; allow the
         * existing decoder to execute it, with the halt retained for its next
         * instruction. This is source compatibility, not PS1 bus arbitration. */
        uint32_t instruction=cpu->read_word(pc);
        if((instruction>>26)==0x12u)return;
        psx_icache_fetch_miss(cpu,pc);
        if(cpu->read_absorb[cpu->read_absorb_which])
            --cpu->read_absorb[cpu->read_absorb_which];
        else psx_advance_cycles(1u);
        psx_cyc_batch_flush();
        psx_devices_service_to_now();
        cycle=psx_get_cycle_count();
    }
}

void source_gpu_runtime_init(void) {
    if(enabled || psx_get_cycle_count()!=0 || g_psx_cpu_step_boundary_callback)fail("cold initialization/CPU owner required");
    source_gpu_service_cold(&clock_state);source_gpu_command_cold(&command_state);
    input_route_raster_reset(&draw_raster);command_state.field_valid=1;
    enabled=1;g_psx_cpu_step_boundary_callback=cpu_boundary;
    psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
}
int source_gpu_runtime_active(void) {return enabled;}
int source_gpu_runtime_ready(void) {return enabled?source_gpu_command_ready(&command_state):-2;}
uint32_t source_gpu_runtime_status_bits(void) {
    uint32_t bits=(command_state.dma_direction&2u)?1u<<25:0;
    if(!command_state.phase && !command_state.count && command_state.budget>=0)bits|=1u<<26;
    if(source_gpu_command_ready(&command_state)>0)bits|=1u<<28;
    if(command_state.phase==8)bits|=1u<<27;
    return bits;
}
void source_gpu_runtime_advance(void) {
    if(enabled && !source_gpu_service_to(&clock_state,psx_cycle_count,service,0))fail("reversed device time");
}
uint32_t source_gpu_runtime_cycles_to_event(void) {
    if(!enabled)return UINT32_MAX;
    if(clock_state.frame_pending)return 0;
    uint64_t next=source_gpu_service_next(&clock_state);
    return next>psx_cycle_count?(uint32_t)(next-psx_cycle_count):1u;
}
/* Slice deadline for the precise-slice guard (step 2 of the TAS speed task).
 * The service clock's own 128-cycle re-arm is the source emulator's update
 * quantum, not a guest-visible instant: the OWN ticks only progress GPU
 * command work and the draw raster, which every MMIO access re-syncs before
 * observing, and the service loop replays the same tick sequence at the next
 * service point. What a compiled block must NOT run across: the raster phase
 * edge (scanline end / HBlank edge -- the frame request is set there and the
 * frontend return is consumed at the following instruction boundary), and any
 * tick while a source DMA transfer is live or halting the CPU (word traffic
 * and CPU-halt cycles are instruction-ordered). Everything else is caught up. */
uint32_t source_gpu_runtime_cycles_to_slice_deadline(void) {
    if(!enabled)return UINT32_MAX;
    if(clock_state.frame_pending)return 0;
    uint64_t next=clock_state.cycle+source_gpu_service_until_phase(&clock_state);
    /* Keep the tick while: a source transfer is moving words, any DMA channel
     * is busy (its completion IRQ is raised from the DMA tick and is not in
     * cycles_to_next_event), the CPU is halted for DMA, or an IRQ source the
     * countdown does not model (GPU bit1, SPU bit9, PIO bit10) is unmasked. */
    extern int dma_source_transfer_active(void);
    extern unsigned dma_channels_busy_mask(void);
    extern uint32_t i_mask;
    if(dma_source_transfer_active() || dma_channels_busy_mask() || dma_cpu_source_halted() || (i_mask & 0x602u)) {
        uint64_t tick=source_gpu_service_next(&clock_state);
        if(tick<next)next=tick;
    }
    return next>psx_cycle_count?(uint32_t)(next-psx_cycle_count):1u;
}

void source_gpu_runtime_dma_write(void) {
    if(enabled && !source_gpu_service_dma_write(&clock_state,psx_cycle_count,service,0))fail("invalid DMA write time");
}
void source_gpu_runtime_gp0(uint32_t word) {
    if(!enabled)return;
    unsigned previous=command_state.first_triangles;
    if(!source_gpu_command_write(&command_state,word))fail("unsupported GP0 word");
    dispatch();
    if(!previous && command_state.first_triangles)
        fprintf(stderr,"[source-gpu-service] first quad at %llu; budget=%d phase=%u queued=%u clip=%d,%d..%d,%d offset=%d,%d mode=%02X mask=%u\n",(unsigned long long)psx_get_cycle_count(),command_state.budget,command_state.phase,command_state.count,command_state.clip_x0,command_state.clip_y0,command_state.clip_x1,command_state.clip_y1,command_state.offset_x,command_state.offset_y,command_state.display_mode,command_state.mask_bits);
}
void source_gpu_runtime_read(void) {
    if(enabled)source_gpu_command_read(&command_state);
}
void source_gpu_runtime_gp1(uint32_t word) {
    if(!enabled)return;
    source_gpu_runtime_advance();
    if(!source_gpu_command_gp1(&command_state,word) || !input_route_raster_gp1(&clock_state.raster,word) || !input_route_raster_gp1(&draw_raster,word))fail("unsupported GP1 control");
    command_state.skip_field=(draw_raster.y_start+draw_raster.readout_field)&1u;
    psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
}
void source_gpu_runtime_copy(SourceGPUServiceClock *clock,SourceGPUCommandProjection *command) {
    if(clock)*clock=clock_state;
    if(command)*command=command_state;
}
void source_gpu_runtime_copy_return(SourceGPUServiceClock *clock,SourceGPUCommandProjection *command) {
    if(clock)*clock=return_clock;
    if(command)*command=return_command;
}
