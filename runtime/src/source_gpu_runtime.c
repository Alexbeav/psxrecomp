#include "source_gpu_runtime.h"
#include "psx_icache.h"
#include "psx_cycles.h"
#include "dma.h"
#include "source_cpu_boundary_probe.h"
#include "source_ram_page_probe.h"
#include "source_tas_stateio.h"
#include "boot_state.h"
#include "savestate.h"
#include "debug_server.h"
#include "dirty_ram_interp.h"
#include "psx_sha256.h"
#include "pst_wire.h"
#include "input_route_raster_clock_wire.h"
#include "source_stateio_identity.h"
#include "interrupts.h"               /* E survey: IRQ_TIMING transients */
#include "sio.h"                      /* E survey (Part 3): SIO FSM sizing */
#include "memcard.h"                  /* TAS checkpoint card images */
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
extern uint8_t *memory_get_ram_ptr(void);
/* TAS checkpoint production. Opt-in via PSX_TAS_SAVE_STATE_AT=<return>[,<return>]:
 * one save per listed frontend return, beside the RAM-page probe. The manifest
 * binds frame/cycle/RAM digest, the resolved configuration, the runtime binary
 * and the route content, so a later resume can prove identity; host-only caches
 * are re-derived by boot_state on load. */
/* A requested return whose boundary has no represented instruction continuation
 * (a return reached inside a nested exception dispatch, whose host call chain no
 * single continuation can describe) is not captured there. The request moves to
 * the next frontend return that can be captured, and that state's manifest names
 * both its own return and the earliest request it satisfies. */
static unsigned s_tas_deferred_request;
static void tas_stateio_save(CPUState *cpu,unsigned frame,uint64_t cycle) {
    if(!cpu || (!source_tas_stateio_save_at_match(frame) && !s_tas_deferred_request)) return;
    if (!dirty_ram_checkpoint_pc(0)) {
        if(!s_tas_deferred_request) s_tas_deferred_request=frame;
        fprintf(stderr,"[tas-stateio] return %u has no represented instruction continuation; "
                       "capture deferred to the next return\n",frame);
        return;
    }
    const unsigned requested_frame=s_tas_deferred_request ? s_tas_deferred_request : frame;
    s_tas_deferred_request=0;
    const char *path=getenv("PSX_TAS_SAVE_STATE_PATH");
    char auto_path[4096];
    if(!path || !*path) {
        const char *dir=getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
        if(!dir || snprintf(auto_path,sizeof auto_path,"%s/tas-state-%06u.pst",dir,frame)>=(int)sizeof auto_path) {
            fprintf(stderr,"[tas-stateio] save refused: no state path or capture dir at return %u\n",frame);
            return;
        }
        path=auto_path;
    }
    uint32_t bios_checksum=0,entry_pc=0;
    savestate_get_integrity(&bios_checksum,&entry_pc);
    if(!boot_state_save(cpu,bios_checksum,entry_pc,path)) {
        fprintf(stderr,"[tas-stateio] save failed at return %u\n",frame);
        return;
    }
    char sha_hex[65]; unsigned long long state_bytes=0;
    FILE *state=fopen(path,"rb");
    if(!state) { fprintf(stderr,"[tas-stateio] save wrote no readable state at return %u\n",frame); return; }
    psx_sha256_ctx ctx; psx_sha256_init(&ctx);
    uint8_t buffer[65536]; size_t got;
    while((got=fread(buffer,1,sizeof buffer,state))>0) { psx_sha256_update(&ctx,buffer,got); state_bytes+=got; }
    fclose(state);
    uint8_t digest[32]; psx_sha256_final(&ctx,digest);
    for(unsigned i=0;i<32;i++) sprintf(sha_hex+i*2,"%02x",digest[i]);
    sha_hex[64]='\0';
    TasStateManifest m; memset(&m,0,sizeof m);
    m.frame=frame; m.requested_frame=requested_frame; m.cycle=cycle; m.bios_checksum=bios_checksum; m.entry_pc=entry_pc;
    m.input_consumed=debug_server_input_route_consumed();
    m.state_bytes=state_bytes;
    m.ram_digest=source_tas_stateio_ram_digest(memory_get_ram_ptr(),2097152u);
    /* v7 identity: whole resolved configuration, the exact runtime binary, and
     * the exact route content. */
    source_stateio_config_digest_hex(m.config_digest);
    if(!source_stateio_exe_sha256(m.exe_sha256)) {
        fprintf(stderr,"[tas-stateio] save refused: cannot hash the runtime binary\n");
        return;
    }
    {
        if(!source_stateio_route_sha256(m.route_sha256)) {
            fprintf(stderr,"[tas-stateio] save refused: cannot hash the input route\n");
            return;
        }
    }
    /* Card images are host files, not boot_state sections: save each present
     * slot's in-memory image beside the state, replaced atomically. */
    for(int slot=0;slot<2;slot++) {
        static uint8_t image[PSX_TAS_STATEIO_CARD_BYTES];
        char card_path[4160],temporary[4200];
        FILE *card;
        if(!memcard_is_present(slot)) { snprintf(m.card_sha256[slot],sizeof m.card_sha256[slot],"%s",PSX_TAS_STATEIO_NO_CARD); continue; }
        if(memcard_export_raw(slot,image)!=0 || !source_tas_stateio_card_path(card_path,sizeof card_path,path,slot) ||
           snprintf(temporary,sizeof temporary,"%s.tmp",card_path)>=(int)sizeof temporary || !(card=fopen(temporary,"wb"))) {
            fprintf(stderr,"[tas-stateio] save refused: cannot capture card %d at return %u\n",slot+1,frame);
            return;
        }
        int written=fwrite(image,1,sizeof image,card)==sizeof image;
        if(fclose(card)!=0) written=0;
        if(!written || !boot_state_replace_file(temporary,card_path)) {
            remove(temporary);
            fprintf(stderr,"[tas-stateio] save refused: cannot write card %d at return %u\n",slot+1,frame);
            return;
        }
        uint8_t card_digest[32]; psx_sha256_compute(image,sizeof image,card_digest);
        for(unsigned i=0;i<32;i++) sprintf(m.card_sha256[slot]+i*2,"%02x",card_digest[i]);
    }
    char manifest_path[4160];
    if(snprintf(manifest_path,sizeof manifest_path,"%s.json",path)>=(int)sizeof manifest_path ||
       !source_tas_stateio_manifest_write(manifest_path,&m,path,sha_hex)) {
        fprintf(stderr,"[tas-stateio] save manifest failed at return %u\n",frame);
        return;
    }
    fprintf(stderr,"[tas-stateio] saved return %u cycle %llu RAM %016llX -> %s\n",frame,
            (unsigned long long)cycle,(unsigned long long)m.ram_digest,path);
}
/* E-recon: bounded survey of source-GPU-service quiescence at frame boundaries.
 * Opt-in via PSX_E_SURVEY=1. Answers the pre-registered abort condition in the
 * campaign scope doc: is command_state near-zero at the checkpoint boundary? */
static struct {
    int initialized, enabled;
    unsigned long long boundaries, queue_nonzero, words_nonzero, words_total, budget_nonzero;
    unsigned long long upload_live, ll_live, spu_live;
    unsigned long long irq_slot_nonzero, defer_switch_nonzero;
    unsigned long long sio_pad_live, sio_mc_live;
} s_e_survey;
static void e_survey_report(void) {
    if(!s_e_survey.enabled) return;
    fprintf(stderr,"[e-survey] boundaries=%llu queue_nonzero=%llu words_nonzero=%llu words_total=%llu budget_nonzero=%llu\n",
        s_e_survey.boundaries,s_e_survey.queue_nonzero,s_e_survey.words_nonzero,
        s_e_survey.words_total,s_e_survey.budget_nonzero);
    fprintf(stderr,"[e-survey] source-dma live: upload=%llu ll=%llu spu=%llu (of %llu boundaries)\n",
        s_e_survey.upload_live,s_e_survey.ll_live,s_e_survey.spu_live,s_e_survey.boundaries);
    fprintf(stderr,"[e-survey] IRQ_TIMING transient: source_irq_slot_nonzero=%llu s_defer_switch_nonzero=%llu (of %llu)\n",
        s_e_survey.irq_slot_nonzero,s_e_survey.defer_switch_nonzero,s_e_survey.boundaries);
    fprintf(stderr,"[e-survey] source-sio fsm live: pad_ack=%llu memcard=%llu (of %llu boundaries)\n",
        s_e_survey.sio_pad_live,s_e_survey.sio_mc_live,s_e_survey.boundaries);
    {
        uint32_t sz[5];
        sio_source_survey_sizes(sz);
        fprintf(stderr,"[e-survey] source-sio sizes: pads=%u memcard=%u fsm_pace=%u unemitted_scalars=%u mc_slots=%u\n",
            sz[0],sz[1],sz[2],sz[3],sz[4]);
    }
}
static void e_survey(void) {
    if(!s_e_survey.initialized) {
        s_e_survey.initialized=1;
        const char *e=getenv("PSX_E_SURVEY");
        s_e_survey.enabled=e && e[0]=='1' && !e[1];
        if(s_e_survey.enabled) atexit(e_survey_report);
    }
    if(!s_e_survey.enabled) return;
    ++s_e_survey.boundaries;
    s_e_survey.words_total+=command_state.count;
    if(command_state.count) {
        ++s_e_survey.queue_nonzero;
        for(unsigned i=0;i<command_state.count;i++) if(command_state.queue[i]) ++s_e_survey.words_nonzero;
    }
    if(command_state.budget) ++s_e_survey.budget_nonzero;
    {
        int up=0,ll=0,sp=0;
        dma_source_dma_live(&up,&ll,&sp);
        if(up) ++s_e_survey.upload_live;
        if(ll) ++s_e_survey.ll_live;
        if(sp) ++s_e_survey.spu_live;
    }
    {
        /* IRQ_TIMING transients: are they ever non-zero at a boundary? If never,
         * keep serializing them but assert zero on save, like queue[32]. */
        if (interrupts_source_irq_slot_live()) ++s_e_survey.irq_slot_nonzero;
        if (psx_defer_switch_pending()) ++s_e_survey.defer_switch_nonzero;
    }
    {
        int pad=0,mc=0;
        sio_source_fsm_live(&pad,&mc);
        if(pad) ++s_e_survey.sio_pad_live;
        if(mc) ++s_e_survey.sio_mc_live;
    }
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
            tas_stateio_save(cpu,clock_state.frame_returns,cycle);
            e_survey();
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
#ifdef PSX_NO_STEP_BOUNDARY
    /* Wave-5: the normal product compiles the per-instruction retirement boundary out
     * (PSX_STEP_BOUNDARY=OFF); the source model cannot observe retirement there. */
    fail("source GPU model needs the per-instruction retirement boundary; use the diagnostic product (PSX_STEP_BOUNDARY=ON)");
#endif
    if(enabled || psx_get_cycle_count()!=0 || g_psx_cpu_step_boundary_callback)fail("cold initialization/CPU owner required");
    source_gpu_service_cold(&clock_state);source_gpu_command_cold(&command_state);
    input_route_raster_reset(&draw_raster);command_state.field_valid=1;
    enabled=1;g_psx_cpu_step_boundary_callback=cpu_boundary;
    psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;
}
int source_gpu_runtime_active(void) {return enabled;}
/* Resume support: restore the frontend-return counter after a checkpoint load,
 * so subsequent probes and the input route line up with the original run. */
int source_gpu_runtime_set_frame_returns(uint32_t frame) {
    if(!enabled)return 0;
    clock_state.frame_returns=frame;
    return_clock=clock_state;
    return 1;
}

/* ---- BS_SEC_RASTER instances [1] and [2] -------------------------------- */
uint32_t source_gpu_raster_wire_bytes(void) { return INPUT_ROUTE_RASTER_WIRE_BYTES*2u; }
void source_gpu_raster_wire_write(uint8_t *out) {
    input_route_raster_wire_write(&clock_state.raster, out);
    input_route_raster_wire_write(&draw_raster, out+INPUT_ROUTE_RASTER_WIRE_BYTES);
}
int source_gpu_raster_wire_read(const uint8_t *in, uint32_t len) {
    if (len != INPUT_ROUTE_RASTER_WIRE_BYTES*2u) return 0;
    if (!input_route_raster_wire_read(&clock_state.raster, in, INPUT_ROUTE_RASTER_WIRE_BYTES))
        return 0;
    if (!input_route_raster_wire_read(&draw_raster, in+INPUT_ROUTE_RASTER_WIRE_BYTES,
                                      INPUT_ROUTE_RASTER_WIRE_BYTES))
        return 0;
    return 1;
}

/* ---- BS_SEC_GPU_SERVICE ------------------------------------------------- */
/* Service clock, command projection, and all 32 queued command words. */
uint32_t source_gpu_service_wire_bytes(void) { return SOURCE_GPU_SERVICE_WIRE_BYTES; }
int source_gpu_service_queue_empty(void) { return command_state.count == 0; }
/* Amendment C: the two derived copies are refreshed once per frame boundary on
 * the normal path, so after a restore they would lag by a frame. A bit-exact
 * replay cannot afford a frame of stale reads. Re-derive them once post-load. */
void source_gpu_runtime_rederive_returns(void) {
    return_clock = clock_state;
    return_command = command_state;
}
/* E negative control: corrupt one restored service/projection field so the
 * ladder comparison must fail. Test/diagnostic only. */
int source_gpu_service_perturb(const char *field) {
    if (!field || !*field || !enabled) return 0;
    if (strcmp(field, "service_budget_debt") == 0) {
        command_state.budget -= 1000000;
        return 1;
    }
    if (strcmp(field, "service_cycle") == 0) {
        clock_state.cycle += 1u;
        fprintf(stderr, "[tas-stateio] negative control: service cycle -> %llu\n",
                (unsigned long long)clock_state.cycle);
        return 1;
    }
    if (strcmp(field, "service_frame_returns") == 0) {
        clock_state.frame_returns += 1u;
        fprintf(stderr, "[tas-stateio] negative control: service frame_returns -> %u\n",
                clock_state.frame_returns);
        return 1;
    }
    if (strcmp(field, "service_budget") == 0) {
        command_state.budget += 1;
        fprintf(stderr, "[tas-stateio] negative control: service budget -> %d\n",
                command_state.budget);
        return 1;
    }
    return 0;
}
void source_gpu_service_wire_write(uint8_t *out) {
    PstW w; pst_w_init(&w, out, SOURCE_GPU_SERVICE_WIRE_BYTES);
    /* psx_cycle_count is restored exactly by BS_SEC_CLOCK, so every absolute
     * stamp here (cycle, deadlines, frame_request_cycle, last_update) is
     * written as-is — no rebase. #7's existing delta-rebase is an identity op. */
    pst_w_u64(&w, clock_state.cycle);             pst_w_u64(&w, clock_state.gpu_deadline);
    pst_w_u64(&w, clock_state.dma_deadline);      pst_w_u64(&w, clock_state.frame_request_cycle);
    pst_w_u32(&w, clock_state.zero_reached);      pst_w_u32(&w, clock_state.frame_pending);
    pst_w_u32(&w, clock_state.frame_returns);
    pst_w_i32(&w, command_state.budget);
    pst_w_u32(&w, command_state.count);           pst_w_u32(&w, command_state.phase);
    pst_w_u32(&w, command_state.command);         pst_w_u64(&w, command_state.last_update);
    pst_w_i32(&w, command_state.clip_x0);         pst_w_i32(&w, command_state.clip_y0);
    pst_w_i32(&w, command_state.clip_x1);         pst_w_i32(&w, command_state.clip_y1);
    pst_w_i32(&w, command_state.offset_x);        pst_w_i32(&w, command_state.offset_y);
    pst_w_u32(&w, command_state.draw_mode);       pst_w_u32(&w, command_state.texture_window);
    pst_w_u32(&w, command_state.mask_bits);       pst_w_u32(&w, command_state.display_mode);
    pst_w_u32(&w, command_state.dma_direction);
    pst_w_u32(&w, command_state.field_valid);     pst_w_u32(&w, command_state.skip_field);
    pst_w_u32(&w, command_state.first_triangles); pst_w_u32(&w, command_state.second_triangles);
    /* INCMD_PLINE: a poly-line split across a frontend return is still open. */
    pst_w_u32(&w, command_state.pline);           pst_w_u32(&w, command_state.pline_command);
    pst_w_u32(&w, command_state.pline_color);     pst_w_u32(&w, command_state.pline_vertex);
    for (unsigned i=0;i<12u;i++) pst_w_u32(&w, command_state.polygon_words[i]);
    pst_w_u32(&w, command_state.transfer_words);
    pst_w_u32(&w, command_state.dispatch.kind);   pst_w_u32(&w, command_state.dispatch.count);
    for (unsigned i=0;i<12u;i++) pst_w_u32(&w, command_state.dispatch.words[i]);
    pst_w_i32(&w, command_state.error);
    for (unsigned i=0;i<32u;i++) pst_w_u32(&w, command_state.queue[i]);
}
int source_gpu_service_wire_read(const uint8_t *in, uint32_t len) {
    PstR r;
    if (!in || len != SOURCE_GPU_SERVICE_WIRE_BYTES) return 0;
    { PstR count_wire;uint32_t count;
      pst_r_init(&count_wire,in+48,4);
      if (!pst_r_u32(&count_wire,&count) || count>32u) return 0; }
    pst_r_init(&r, in, len);
    if (!pst_r_u64(&r,&clock_state.cycle)              || !pst_r_u64(&r,&clock_state.gpu_deadline) ||
        !pst_r_u64(&r,&clock_state.dma_deadline)       || !pst_r_u64(&r,&clock_state.frame_request_cycle) ||
        !pst_r_u32(&r,&clock_state.zero_reached)       || !pst_r_u32(&r,&clock_state.frame_pending) ||
        !pst_r_u32(&r,&clock_state.frame_returns)      || !pst_r_i32(&r,&command_state.budget) ||
        !pst_r_u32(&r,&command_state.count)            || !pst_r_u32(&r,&command_state.phase) ||
        !pst_r_u32(&r,&command_state.command)          || !pst_r_u64(&r,&command_state.last_update) ||
        !pst_r_i32(&r,&command_state.clip_x0)          || !pst_r_i32(&r,&command_state.clip_y0) ||
        !pst_r_i32(&r,&command_state.clip_x1)          || !pst_r_i32(&r,&command_state.clip_y1) ||
        !pst_r_i32(&r,&command_state.offset_x)         || !pst_r_i32(&r,&command_state.offset_y) ||
        !pst_r_u32(&r,&command_state.draw_mode)        || !pst_r_u32(&r,&command_state.texture_window) ||
        !pst_r_u32(&r,&command_state.mask_bits)        || !pst_r_u32(&r,&command_state.display_mode) ||
        !pst_r_u32(&r,&command_state.dma_direction)    || !pst_r_u32(&r,&command_state.field_valid) ||
        !pst_r_u32(&r,&command_state.skip_field)       || !pst_r_u32(&r,&command_state.first_triangles) ||
        !pst_r_u32(&r,&command_state.second_triangles) ||
        !pst_r_u32(&r,&command_state.pline)            || !pst_r_u32(&r,&command_state.pline_command) ||
        !pst_r_u32(&r,&command_state.pline_color)      || !pst_r_u32(&r,&command_state.pline_vertex))
        return 0;
    /* The tail is POSITIONAL: this order must match source_gpu_service_wire_write
     * exactly. A divergence here is invisible to the total-length check and to
     * the _Static_assert(sizeof), which is how the shipped version read `error`
     * and both word arrays at the wrong offsets (test_boot_state_section_wire). */
    for (unsigned i=0;i<12u;i++)
        if (!pst_r_u32(&r,&command_state.polygon_words[i])) return 0;
    if (!pst_r_u32(&r,&command_state.transfer_words) ||
        !pst_r_u32(&r,&command_state.dispatch.kind) ||
        !pst_r_u32(&r,&command_state.dispatch.count))
        return 0;
    for (unsigned i=0;i<12u;i++)
        if (!pst_r_u32(&r,&command_state.dispatch.words[i])) return 0;
    if (!pst_r_i32(&r,&command_state.error))
        return 0;
    for (unsigned i=0;i<32u;i++)
        if (!pst_r_u32(&r,&command_state.queue[i])) return 0;
    return 1;
}
int source_gpu_runtime_ready(void) {return enabled?source_gpu_command_ready(&command_state):-2;}
uint32_t source_gpu_runtime_status_bits(void) {
    uint32_t bits=(command_state.dma_direction&2u)?1u<<25:0;
    if(!command_state.phase && !command_state.count && command_state.budget>=0)bits|=1u<<26;
    if(source_gpu_command_ready(&command_state)>0)bits|=1u<<28;
    if(command_state.phase==8)bits|=1u<<27;
    return bits;
}
void source_gpu_runtime_advance(void) {
    if(enabled && !source_gpu_service_to(&clock_state,psx_cycle_count,service,0))
        fail("reversed device time");
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
