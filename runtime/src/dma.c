/* dma.c — PS1 DMA controller simulation (Phase 3).
 *
 * Implements all 7 channel register reads/writes, DPCR, DICR,
 * and transfer execution for:
 *   - Ch2 (GPU): block mode and linked-list mode
 *   - Ch6 (OTC): ordering table clear
 *
 * Most channels execute synchronously when CHCR start bit is written. MDEC
 * request-mode transfers are advanced from the guest cycle clock so games
 * which synchronize video decode through DMA busy/request state see realistic
 * backpressure.
 * Reference: nocash PSX specs, DuckStation src/core/dma.cpp
 */

#include "dma.h"
#include "source_gpu_runtime.h"
#include "cdrom.h"
#include "crash_trace.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "mdec.h"
#include "mod_memory.h"
#include "overlay_capture.h"
#include "spu.h"
#include "audio_trace.h"
#include "event_ring.h"
#include "psx_cycles.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory access — defined in memory.c */
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);

/* Interrupt status — defined in memory.c */
extern uint32_t i_stat;
/* Central IRQ-raise choke point (interrupts.c) — also records the device ring. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint64_t s_frame_count;

/* ---- Per-channel registers ---- */

typedef struct {
    uint32_t madr;  /* +0x00: Memory address */
    uint32_t bcr;   /* +0x04: Block control */
    uint32_t chcr;  /* +0x08: Channel control */
} DMAChannel;

static DMAChannel channels[7];

typedef struct {
    uint8_t active;
    uint8_t debug_started;
    uint32_t total_words;
    uint32_t remaining_words;
    uint32_t cycles_accum;
    uint32_t start_addr;   /* madr at transfer start (CD overlay capture) */
} DMAAsyncChannel;

static DMAAsyncChannel mdec_async[2];
static DMAAsyncChannel cdrom_async;

/* Optional source-core experiment. This deliberately models Octoshock 2.2.2
 * OTC service granularity and whole-CPU waiting, not PS1 bus arbitration.
 * Facts consulted: original dma.cpp RunChannelI/DMA_Update/DMA_Write.
 * No reference implementation text is incorporated. */
static int otc_source_model;
static struct {
    uint32_t remaining, address;
    uint64_t last_cycle, next_cycle;
} otc_source;
/* Optional manual CD DMA timing only. Register conventions remain native.
 * Source facts: 64 initial clocks, 9 clocks/word, 128-clock service and CPU
 * halt. These are original-core compatibility rules, not bus measurements. */
static int cd_source_model;
static struct {
    int32_t budget;
    uint64_t last_cycle, next_cycle;
} cd_source;
/* Optional source request timing for an already active VRAM upload only. */
static int gpu_upload_source_model;
static int gpu_ll_source_model;
static struct {
    uint32_t active, address, remaining, nodes;
    int32_t budget;
    uint64_t last_cycle, next_cycle;
} gpu_ll_source;
uint32_t g_dma_cpu_read_wait;
static uint32_t gpu_upload_live_read_wait;
static int cpu_read_wait_latched;
static struct {
    uint32_t remaining, block_size, in_block, address;
    int32_t budget;
    uint64_t last_cycle, next_cycle;
} gpu_upload_source;
/* Source SPU request DMA:64 initial clocks,48 per word, global128-clock
 * updates. Payload and register progress belong to those service calls. */
static struct {
    uint32_t remaining, block_size, in_block, address, total_words, start_addr;
    int32_t budget;
    uint64_t last_cycle, next_cycle;
} spu_source;

/* ---- CD DMA transfer log ---- */
/* Every forward CH3 DMA that lands below 0x1C0000 (game data region) records
 * (setloc_lba, dest_addr, size). Transfers to 0x1C0000+ are FMV/streaming
 * buffers and are excluded to keep the ring focused on overlay loads.
 * Surfaced via the cd_read_log TCP command; pairs with overlay_dump to map
 * overlay regions back to disc positions for extract_overlays.py. */
#define CD_DMA_LOG_CAP 65536
typedef struct { int lba; uint32_t dest; uint32_t size; } CdDmaEntry;
static CdDmaEntry cd_dma_log[CD_DMA_LOG_CAP];
static uint32_t   cd_dma_log_head  = 0;
static uint32_t   cd_dma_log_total = 0;

static void cd_dma_log_push(int lba, uint32_t dest, uint32_t size) {
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].lba  = lba;
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].dest = dest;
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].size = size;
    cd_dma_log_head = (cd_dma_log_head + 1) % CD_DMA_LOG_CAP;
    cd_dma_log_total++;
}

uint32_t cd_dma_log_get_total(void) { return cd_dma_log_total; }
void     cd_dma_log_get_entry(uint32_t idx, int *lba, uint32_t *dest, uint32_t *size) {
    uint32_t cap   = CD_DMA_LOG_CAP;
    uint32_t oldest = cd_dma_log_total > cap ? cd_dma_log_total - cap : 0;
    if (idx < oldest || idx >= cd_dma_log_total) { *lba = -1; return; }
    uint32_t slot = idx % cap;
    *lba  = cd_dma_log[slot].lba;
    *dest = cd_dma_log[slot].dest;
    *size = cd_dma_log[slot].size;
}

#define DMA_MDEC_IN_CYCLES_PER_WORD   1u
#define DMA_MDEC_OUT_CYCLES_PER_WORD 14u
#define DMA_GPU_CYCLES_PER_WORD       1u
#define DMA_CDROM_CYCLES_PER_WORD     1u
/* SPU DMA (ch4) per-word cost. Faithful to the Beetle/mednafen oracle, which
 * charges `extra_cyc_overhead = 47` per word plus the universal 1 cyc/word in
 * RunChannel (mednafen/psx/dma.cpp:267,294,456) => 48 cyc/word. Previously ch4
 * completed in ZERO cycles (instant complete_transfer), so the DMA-completion
 * IRQ fired the same cycle as the kick. Once the I-cache cycle model lands, the
 * BIOS SPU-init loop's instruction timing interleaves with that instant IRQ into
 * an exception re-entry storm (MMX6 boot wedge: kick ch4 -> 0-cyc done -> IRQ ->
 * ack -> re-kick, forever). Deferring completion the faithful ~48 cyc/word breaks
 * the storm and matches hardware (the SPU RAM payload still moves immediately;
 * only the busy-bit clear + completion IRQ are deferred). */
#define DMA_SPU_CYCLES_PER_WORD      48u

/* DMA-execution provenance flags (read by memory.c's psx_write_word d44_note probe
 * for the MMX6 VSync-callback-pointer corruption hunt). g_dma_exec_depth>0 means a
 * DMA is currently moving data through psx_write_word, so a RAM write seen there is
 * DMA-sourced (not a CPU/dirty-interp store, whose g_debug_last_store_pc is accurate).
 * cur_ch/cur_madr/cur_bcr name the in-flight channel + its destination, so a wrong
 * DMA destination clobbering kernel data becomes directly visible. */
int      g_dma_exec_depth = 0;
int      g_dma_cur_ch     = -1;
uint32_t g_dma_cur_madr   = 0;
uint32_t g_dma_cur_bcr    = 0;
/* Guest PC that kicked the in-flight DMA (the CHCR store that set start/busy),
 * so write-provenance (wtrace / parity note_write) attributes DMA-sourced RAM
 * writes to the code that INITIATED the transfer, not to g_debug_last_store_pc
 * (which for an async transfer is a stale, unrelated CPU store). Captured at the
 * kick in trigger_dma_transfer and re-published for async channels whose RAM
 * writes run in a later advance_*() step. 0 = unknown. */
uint32_t g_dma_initiator_pc = 0;
static uint32_t s_dma_ch_initiator_pc[7] = {0};

typedef struct {
    uint8_t active;
    uint32_t total_words;
    uint32_t cycles_remaining;
} DMADelayedComplete;

static DMADelayedComplete delayed_complete[7];

/* ---- Global registers ---- */

static uint32_t dpcr;  /* 0x1F8010F0: DMA control (enable bits) */
static uint32_t dicr;  /* 0x1F8010F4: DMA interrupt control */

#define DICR_WRITE_MASK 0x00FF807Fu
#define DICR_RESET_MASK 0x7F000000u

static DMATraceEntry dma_trace[DMA_TRACE_CAP];
static uint64_t dma_trace_seq;
static DMACDROMHistoryEntry cdrom_dma_history[DMA_CDROM_HISTORY_CAP];
static uint64_t cdrom_dma_history_seq;
static DMACDROMHistoryEntry cdrom_dma_active_entry;
static uint8_t cdrom_dma_history_active;

static uint32_t dicr_read_value(uint32_t v);

static void trace_dma(uint32_t kind, int ch, uint32_t total_words,
                      uint32_t dicr_before, uint32_t i_stat_before) {
    DMATraceEntry *e = &dma_trace[dma_trace_seq % DMA_TRACE_CAP];
    e->seq = dma_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = kind;
    e->channel = (uint32_t)ch;
    e->total_words = total_words;
    e->addr = 0;
    e->val = 0;
    e->mask = 0;
    e->madr = (ch >= 0 && ch < 7) ? channels[ch].madr : 0;
    e->bcr = (ch >= 0 && ch < 7) ? channels[ch].bcr : 0;
    e->chcr = (ch >= 0 && ch < 7) ? channels[ch].chcr : 0;
    e->dpcr = dpcr;
    e->dicr_before = dicr_read_value(dicr_before);
    e->dicr_after = dicr_read_value(dicr);
    e->i_stat_before = i_stat_before;
    e->i_stat_after = i_stat;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
}

static void trace_dma_reg_write(uint32_t addr, uint32_t val, uint32_t mask,
                                uint32_t dicr_before,
                                uint32_t i_stat_before) {
    DMATraceEntry *e = &dma_trace[dma_trace_seq % DMA_TRACE_CAP];
    e->seq = dma_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = 'W';
    e->channel = 0xFFFFFFFFu;
    e->total_words = 0;
    e->addr = addr;
    e->val = val;
    e->mask = mask;
    e->madr = 0;
    e->bcr = 0;
    e->chcr = 0;
    e->dpcr = dpcr;
    e->dicr_before = dicr_read_value(dicr_before);
    e->dicr_after = dicr_read_value(dicr);
    e->i_stat_before = i_stat_before;
    e->i_stat_after = i_stat;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
}

/* ---- Helpers ---- */

static void update_master_irq(void) {
    /* DICR bit 31 (master IRQ flag) is read-only, calculated as:
     * bit31 = bit15 OR (bit23 AND ((bits16-22 AND bits24-30) != 0))
     *
     * We store bits 0-30 in dicr and compute bit 31 on read.
     *
     * I_STAT bit 3 is set on the aggregate DICR bit31 0->1 transition.
     * Per-channel DICR flags are latched by complete_transfer() only
     * when both master IRQ and that channel IRQ are enabled. */
}

static uint32_t dicr_master_flag(uint32_t v) {
    return ((v & (1u << 15)) ||
            ((v & (1u << 23)) && (v & DICR_RESET_MASK))) ? (1u << 31) : 0;
}

static uint32_t dicr_read_value(uint32_t v) {
    return (v & ~(1u << 31)) | dicr_master_flag(v);
}

static void raise_dma_irq_on_master_edge(uint32_t before) {
    /* IRQ3 is latched only when DICR's aggregate master flag transitions
     * from 0 to 1. Pending channel flags keep bit 31 high, but do not
     * continuously re-latch I_STAT after software acknowledges IRQ3. */
    if (!dicr_master_flag(before) && dicr_master_flag(dicr)) {
        psx_irq_raise(3, (dicr >> 24) & 0x7Fu);  /* detail = DICR per-channel IRQ flags */
    }
}

static void start_cdrom_dma_capture(uint32_t requested_words) {
    CDROMDebugState s;
    cdrom_debug_snapshot(&s);

    memset(&cdrom_dma_active_entry, 0, sizeof(cdrom_dma_active_entry));
    cdrom_dma_active_entry.seq = cdrom_dma_history_seq++;
    cdrom_dma_active_entry.frame_start = (uint32_t)s_frame_count;
    cdrom_dma_active_entry.start_addr = channels[3].madr & 0x1FFFFCu;
    cdrom_dma_active_entry.final_addr = cdrom_dma_active_entry.start_addr;
    cdrom_dma_active_entry.requested_words = requested_words;
    cdrom_dma_active_entry.bcr = channels[3].bcr;
    cdrom_dma_active_entry.chcr = channels[3].chcr;
    cdrom_dma_active_entry.dpcr = dpcr;
    cdrom_dma_active_entry.dicr_start = dicr_read_value(dicr);
    cdrom_dma_active_entry.i_stat_start = i_stat;
    cdrom_dma_active_entry.func = g_debug_current_func_addr;
    cdrom_dma_active_entry.pc = g_debug_last_store_pc;
    cdrom_dma_active_entry.lba = s.last_sector_lba;
    cdrom_dma_active_entry.sector_size =
        (s.sector_size > 0) ? s.sector_size : s.last_sector_size;
    cdrom_dma_active_entry.sector_read_pos_start = s.sector_read_pos;
    cdrom_dma_active_entry.mode =
        s.last_sector_mode ? s.last_sector_mode : s.mode_reg;
    cdrom_dma_active_entry.sector_available_start =
        (uint8_t)(s.sector_available ? 1 : 0);
    cdrom_dma_history_active = 1;
}

static void record_cdrom_dma_word(uint32_t word) {
    if (!cdrom_dma_history_active) return;

    DMACDROMHistoryEntry *e = &cdrom_dma_active_entry;
    if (e->first_count < DMA_CDROM_HISTORY_WORDS) {
        e->first_words[e->first_count++] = word;
    }

    if (e->last_count < DMA_CDROM_HISTORY_WORDS) {
        e->last_words[e->last_count++] = word;
    } else {
        memmove(e->last_words, e->last_words + 1,
                sizeof(e->last_words[0]) * (DMA_CDROM_HISTORY_WORDS - 1));
        e->last_words[DMA_CDROM_HISTORY_WORDS - 1] = word;
    }

    e->moved_words++;
}

static void finish_cdrom_dma_capture(uint32_t final_addr, uint8_t completed) {
    if (!cdrom_dma_history_active) return;

    CDROMDebugState s;
    cdrom_debug_snapshot(&s);

    cdrom_dma_active_entry.frame_end = (uint32_t)s_frame_count;
    cdrom_dma_active_entry.final_addr = final_addr & 0x1FFFFCu;
    cdrom_dma_active_entry.dicr_end = dicr_read_value(dicr);
    cdrom_dma_active_entry.i_stat_end = i_stat;
    cdrom_dma_active_entry.sector_read_pos_end = s.sector_read_pos;
    cdrom_dma_active_entry.sector_available_end =
        (uint8_t)(s.sector_available ? 1 : 0);
    cdrom_dma_active_entry.completed = completed ? 1 : 0;

    cdrom_dma_history[
        cdrom_dma_active_entry.seq % DMA_CDROM_HISTORY_CAP] =
        cdrom_dma_active_entry;
    cdrom_dma_history_active = 0;
}

static int channel_enabled(int ch) {
    /* DPCR: each channel has 4 bits, bit 3 of each group is enable.
     * Ch0 = bits 0-3, Ch1 = bits 4-7, etc. Enable = bit (ch*4 + 3). */
    return (dpcr >> (ch * 4 + 3)) & 1;
}

static int channel_irq_flag_armed(int ch) {
    /* DICR channel completion flags (24+n) are latched only when both
     * the per-channel interrupt bit and the master DMA interrupt bit are
     * enabled at completion time. Old flags still contribute to bit31
     * until acknowledged, but masked/master-disabled completions do not
     * create new stale flags. */
    return ((dicr >> (16 + ch)) & 1u) && ((dicr >> 23) & 1u);
}

static uint32_t transfer_word_count(int ch) {
    uint32_t chcr = channels[ch].chcr;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t block_size = channels[ch].bcr & 0xFFFFu;
    uint32_t block_count = (channels[ch].bcr >> 16) & 0xFFFFu;

    if (ch == 3 && block_size == 0) {
        if (cd_source_model) return 0x10000u;
        return cdrom_dma_sector_word_count();
    }

    if (sync_mode == 1) {
        if (block_size == 0) block_size = 0x10000u;
        if (block_count == 0) block_count = 1u;
        return block_size * block_count;
    }

    if (block_size == 0) block_size = 0x10000u;
    return block_size;
}

/* A single DMA kick can never legitimately move more data than the larger
 * of RAM (2 MB) or VRAM (1 MB): 0x80000 words. A bigger count means the
 * MADR/BCR/CHCR the guest programmed are corrupt; executing it would grind
 * through billions of masked-wrap accesses feeding garbage to the device
 * sinks. Halt diagnosably with rings intact instead. Linked-list GPU
 * transfers (ch2 sync mode 2) don't use BCR, so length isn't checked there
 * (the node walker has its own MAX_NODES cycle cap). */
#define DMA_MAX_TRANSFER_WORDS 0x80000u

static void validate_transfer_length(int ch) {
    uint32_t sync_mode = (channels[ch].chcr >> 9) & 3u;
    if (ch == 2 && sync_mode == 2) return;
    uint32_t words = transfer_word_count(ch);
    if (words > DMA_MAX_TRANSFER_WORDS) {
        static char reason[160];
        snprintf(reason, sizeof(reason),
                 "DMA ch%d insane transfer length 0x%X words "
                 "(MADR=0x%08X BCR=0x%08X CHCR=0x%08X)",
                 ch, words, channels[ch].madr, channels[ch].bcr,
                 channels[ch].chcr);
        psx_fatal_halt(reason);
    }
}

static void complete_transfer(int ch) {
    uint32_t dicr_before = dicr;
    uint32_t i_stat_before = i_stat;
    channels[ch].chcr &= ~((1u << 24) | (1u << 28));
    /* Source channel4 latches its enabled completion flag even when the
     * master IRQ output is disabled; the master gate belongs to IRQ output. */
    if (((ch==4 && source_gpu_runtime_active()) || (ch<2 && mdec_source_active())) ?
        ((dicr>>(16+ch))&1u) : channel_irq_flag_armed(ch)) {
        dicr |= (1u << (24 + ch));
        raise_dma_irq_on_master_edge(dicr_before);
    }
    trace_dma('C', ch, 0, dicr_before, i_stat_before);
    event_ring_record_aux(EV_DMA_DONE, (uint8_t)ch, channels[ch].chcr);
    event_ring_record_aux(EV_DEQ, (uint8_t)(SRC_DMA0 + ch), channels[ch].chcr);
}

/* Cold Octoshock2.3 MDEC request DMA. Decoder readiness is sampled once
 * per block; payload costs one source clock per word. The decoder's own
 * FIFO/work clock is advanced before channels0/1 at the common DMA boundary.
 * This profile intentionally rejects modes outside the authored contracts. */
static struct { uint32_t address, in_block; int32_t credit; } mdec_source_dma[2];
static uint64_t mdec_source_last_cycle;
static void source_mdec_dma_words(int ch,uint32_t elapsed) {
    DMAChannel *r=&channels[ch];
    mdec_source_dma[ch].credit+=(int32_t)elapsed;
    while(mdec_source_dma[ch].credit>0 && (r->chcr&(1u<<24))) {
        if(!mdec_source_dma[ch].in_block) {
            if(ch==0?!mdec_dma_write_ready():!mdec_dma_read_ready())break;
            mdec_source_dma[ch].address=r->madr;
            mdec_source_dma[ch].in_block=r->bcr&65535u;
            r->bcr=(r->bcr&65535u)|((r->bcr-0x10000u)&0xffff0000u);
        }
        uint32_t address=mdec_source_dma[ch].address;
        if(address&0x800000u) {
            fprintf(stderr,"[dma-model] unqualified MDEC RAM bus error\n");exit(2);
        }
        g_dma_cur_ch=ch;g_dma_initiator_pc=s_dma_ch_initiator_pc[ch];
        g_dma_cur_madr=address;g_dma_cur_bcr=r->bcr;
        if(ch==0)mdec_dma_write_word(psx_read_word(address&0x1ffffcu));
        else {
            uint32_t offset=0,value=mdec_source_dma_read(&offset);
            psx_write_word((address+(offset<<2))&0x1ffffcu,value);
        }
        mdec_source_dma[ch].address=(address+4u)&0xffffffu;
        mdec_source_dma[ch].credit--;
        if(!--mdec_source_dma[ch].in_block) {
            r->madr=mdec_source_dma[ch].address;
            if(!(r->bcr>>16))complete_transfer(ch);
        }
    }
    if(mdec_source_dma[ch].credit>0)mdec_source_dma[ch].credit=0;
}
static void start_source_mdec(int ch) {
    DMAChannel *r=&channels[ch];
    if(!source_gpu_runtime_active() || r->chcr!=(ch==0?0x01000201u:0x01000200u) ||
       (r->bcr&65535u)!=32 || !(r->bcr>>16) || (r->madr&3u)) {
        fprintf(stderr,"[dma-model] unqualified MDEC request ch%d MADR=%08X BCR=%08X CHCR=%08X\n",ch,r->madr,r->bcr,r->chcr);exit(2);
    }
    memset(&mdec_source_dma[ch],0,sizeof(mdec_source_dma[ch]));
    source_mdec_dma_words(ch,64);
}
static void service_source_mdec(uint64_t cycle) {
    if(!mdec_source_active())return;
    if(cycle<mdec_source_last_cycle || cycle-mdec_source_last_cycle>1000000u) {
        fprintf(stderr,"[dma-model] invalid MDEC service interval\n");exit(2);
    }
    uint32_t elapsed=(uint32_t)(cycle-mdec_source_last_cycle);
    mdec_source_last_cycle=cycle;
    mdec_source_advance(elapsed);
    source_mdec_dma_words(0,elapsed);source_mdec_dma_words(1,elapsed);
}
static uint32_t source_mdec_dma_bound(int deliverable) {
    if(mdec_source_active())for(int ch=0;ch<2;ch++)
        if((channels[ch].chcr&(1u<<24)) && (!deliverable || channel_irq_flag_armed(ch)))
            return source_gpu_runtime_cycles_to_event();
    return 0xffffffffu;
}

/* ---- Transfer execution ---- */

static void cancel_async_transfer(int ch) {
    if (ch >= 0 && ch < 2) {
        mdec_async[ch].active = 0;
        mdec_async[ch].debug_started = 0;
        mdec_async[ch].total_words = 0;
        mdec_async[ch].remaining_words = 0;
        mdec_async[ch].cycles_accum = 0;
    }
    if (ch == 3) {
        finish_cdrom_dma_capture(channels[3].madr & 0x1FFFFCu, 0);
        cdrom_async.active = 0;
        cdrom_async.debug_started = 0;
        cdrom_async.total_words = 0;
        cdrom_async.remaining_words = 0;
        cdrom_async.cycles_accum = 0;
    }
    if (ch >= 0 && ch < 7) {
        delayed_complete[ch].active = 0;
        delayed_complete[ch].total_words = 0;
        delayed_complete[ch].cycles_remaining = 0;
    }
}

static void schedule_delayed_complete(int ch, uint32_t total_words,
                                      uint32_t cycles_per_word) {
    if (total_words == 0 || cycles_per_word == 0) {
        complete_transfer(ch);
        return;
    }

    uint64_t cycles = (uint64_t)total_words * (uint64_t)cycles_per_word;
    if (cycles > UINT32_MAX) cycles = UINT32_MAX;

    delayed_complete[ch].active = 1;
    delayed_complete[ch].total_words = total_words;
    delayed_complete[ch].cycles_remaining = (uint32_t)cycles;
    event_ring_record_aux(EV_DMA_SCHED, (uint8_t)ch, channels[ch].chcr);
    /* The MMIO pre-barrier cached deadlines before this channel was armed.
     * Refresh through the scheduler owner so a later cached event cannot hide
     * this completion. This publishes no new CPU cycles; nested service is
     * guarded and its outer pass recomputes the deadline. */
    psx_devices_service_to_now();
}

static void advance_delayed_complete(int ch, uint32_t cycles) {
    DMADelayedComplete *d = &delayed_complete[ch];
    if (!d->active) return;
    if (cycles < d->cycles_remaining) {
        d->cycles_remaining -= cycles;
        return;
    }

    d->active = 0;
    d->total_words = 0;
    d->cycles_remaining = 0;
    complete_transfer(ch);
}

static void start_async_mdec_transfer(int ch) {
    DMAAsyncChannel *a = &mdec_async[ch];
    if (a->active) return;

    a->active = 1;
    a->debug_started = 0;
    a->total_words = transfer_word_count(ch);
    a->remaining_words = a->total_words;
    a->cycles_accum = 0;

    if (ch == 0) {
        mdec_debug_dma_in_start(channels[0].madr & 0x1FFFFCu, a->remaining_words);
    } else {
        mdec_debug_dma_out_start(channels[1].madr & 0x1FFFFCu, a->remaining_words);
    }
    a->debug_started = 1;
}

static void finish_async_mdec_transfer(int ch, uint32_t final_addr, uint32_t total_words) {
    if (ch == 0) {
        mdec_debug_dma_in_end(final_addr, total_words);
    } else {
        mdec_debug_dma_out_end(final_addr, total_words);
    }
    cancel_async_transfer(ch);
    complete_transfer(ch);
}

static void start_async_cdrom_transfer(void) {
    DMAAsyncChannel *a = &cdrom_async;
    if (a->active) return;

    a->active = 1;
    a->debug_started = 0;
    a->total_words = transfer_word_count(3);
    a->remaining_words = a->total_words;
    a->cycles_accum = 0;
    a->start_addr = channels[3].madr & 0x1FFFFCu;
    start_cdrom_dma_capture(a->total_words);

    if (a->total_words == 0) {
        finish_cdrom_dma_capture(channels[3].madr & 0x1FFFFCu, 1);
        cancel_async_transfer(3);
        complete_transfer(3);
    }
}

static void finish_async_cdrom_transfer(uint32_t final_addr) {
    finish_cdrom_dma_capture(final_addr, 1);
    DMAAsyncChannel *a = &cdrom_async;
    uint32_t step       = (channels[3].chcr >> 1) & 1u;
    uint32_t load_start = a->start_addr;
    uint32_t size       = a->total_words * 4u;

    /* Log and capture game-data transfers only.
     * Transfers to 0x1C0000+ are FMV/streaming buffers; skip them. */
    if (!step && size > 0 && load_start < 0x1C0000u) {
        /* The DMA word loop wraps inside the 2 MB RAM mask; the capture
         * path below takes a flat ram+offset span and must not follow the
         * wrap past the end of host RAM. */
        if (size > 0x200000u - load_start)
            size = 0x200000u - load_start;
        int lba = cdrom_get_setloc_lba();
        if (lba >= 0) cd_dma_log_push(lba, load_start, size);

        /* B-1: capture overlay bytes into the write-once capture set.
         * overlay_capture_on_dma auto-activates after game handoff and is a
         * no-op unless the overlay cache is enabled in config. */
        extern uint8_t *memory_get_ram_ptr(void);
        uint8_t *ram = memory_get_ram_ptr();
        overlay_capture_on_dma(load_start, size, ram + load_start);
    }

    channels[3].madr = final_addr;
    cancel_async_transfer(3);
    complete_transfer(3);
}

static void advance_mdec_channel(int ch, uint32_t cycles) {
    DMAAsyncChannel *a = &mdec_async[ch];
    if (!a->active) return;
    if (!((channels[ch].chcr >> 24) & 1u) || !channel_enabled(ch)) return;

    uint32_t chcr = channels[ch].chcr;
    uint32_t direction = chcr & 1u;
    uint32_t step = (chcr >> 1) & 1u;
    int32_t addr_step = step ? -4 : 4;
    uint32_t cycles_per_word = (ch == 0) ? DMA_MDEC_IN_CYCLES_PER_WORD : DMA_MDEC_OUT_CYCLES_PER_WORD;

    if ((ch == 0 && direction == 0) || (ch == 1 && direction != 0)) {
        uint32_t words = a->total_words;
        uint32_t addr = channels[ch].madr & 0x1FFFFCu;
        finish_async_mdec_transfer(ch, addr, words);
        return;
    }

    if (ch == 0) {
        if (!mdec_dma_write_ready()) return;
    } else {
        if (!mdec_dma_read_ready()) return;
    }

    if (cycles > UINT32_MAX - a->cycles_accum) {
        a->cycles_accum = UINT32_MAX;
    } else {
        a->cycles_accum += cycles;
    }

    uint32_t words_budget = a->cycles_accum / cycles_per_word;
    if (words_budget == 0) return;

    uint32_t addr = channels[ch].madr & 0x1FFFFCu;
    uint32_t moved = 0;
    /* ch1 (MDEC→RAM) writes guest RAM here in a deferred step; mark it as
     * DMA-sourced so write-provenance tags these writes with ch1 + the kick PC
     * (ch0 only reads RAM, so it needs no marking). Mirrors the CDROM async. */
    int mdec_writes_ram = (ch == 1);
    if (mdec_writes_ram) {
        g_dma_exec_depth++;
        g_dma_cur_ch = 1; g_dma_cur_bcr = channels[1].bcr;
        g_dma_initiator_pc = s_dma_ch_initiator_pc[1];
    }
    /* Contiguous +4 ch0 (RAM→MDEC): feed via LE burst helper. Guest halfwords
     * + decode triggers match the per-word loop; wraps / decrementing MADR
     * and ch1 (needs psx_write_word watchers) stay on the word path. */
    if (ch == 0 && addr_step == 4) {
        extern uint8_t *memory_get_ram_ptr(void);
        uint8_t *ram = memory_get_ram_ptr();
        while (a->remaining_words > 0 && words_budget > 0) {
            if (!mdec_dma_write_ready()) break;
            uint32_t n = a->remaining_words < words_budget
                       ? a->remaining_words : words_budget;
            uint32_t max_by_ram = (0x200000u - addr) / 4u;
            if (max_by_ram == 0) break;
            if (n > max_by_ram) n = max_by_ram;
            uint32_t got =
                mdec_dma_write_words((const uint32_t *)(ram + addr), n);
            if (got == 0) break;
            addr = (addr + got * 4u) & 0x1FFFFCu;
            a->remaining_words -= got;
            words_budget -= got;
            moved += got;
        }
    } else {
        while (a->remaining_words > 0 && words_budget > 0) {
            if (ch == 0) {
                if (!mdec_dma_write_ready()) break;
                mdec_dma_write_word(psx_read_word(addr));
            } else {
                if (!mdec_dma_read_ready()) break;
                g_dma_cur_madr = addr;
                psx_write_word(addr, mdec_dma_read_word());
            }

            addr = (addr + addr_step) & 0x1FFFFCu;
            a->remaining_words--;
            words_budget--;
            moved++;
        }
    }
    if (mdec_writes_ram) { g_dma_cur_ch = -1; g_dma_exec_depth--; }

    if (moved == 0) return;

    a->cycles_accum -= moved * cycles_per_word;
    channels[ch].madr = addr;

    if (a->remaining_words == 0) {
        finish_async_mdec_transfer(ch, addr, a->total_words);
    }
}

static void execute_ch0_mdec_in(void) {
    uint32_t chcr = channels[0].chcr;
    uint32_t direction = chcr & 1;           /* 1=from RAM to MDEC */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(0);
    uint32_t addr = channels[0].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction != 0) {
        mdec_debug_dma_in_start(addr, total_words);
        if (addr_step == 4 && total_words > 0 &&
            addr + total_words * 4u <= 0x200000u) {
            extern uint8_t *memory_get_ram_ptr(void);
            uint32_t got = mdec_dma_write_words(
                (const uint32_t *)(memory_get_ram_ptr() + addr), total_words);
            addr = (addr + got * 4u) & 0x1FFFFCu;
            /* If the FIFO stalled mid-burst, finish any remainder word-wise. */
            for (uint32_t i = got; i < total_words; i++) {
                mdec_dma_write_word(psx_read_word(addr));
                addr = (addr + 4u) & 0x1FFFFCu;
            }
        } else {
            for (uint32_t i = 0; i < total_words; i++) {
                mdec_dma_write_word(psx_read_word(addr));
                addr = (addr + addr_step) & 0x1FFFFCu;
            }
        }
        mdec_debug_dma_in_end(addr, total_words);
        channels[0].madr = addr;
    }

    complete_transfer(0);
}

static void execute_ch1_mdec_out(void) {
    uint32_t chcr = channels[1].chcr;
    uint32_t direction = chcr & 1;           /* 0=from MDEC to RAM */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(1);
    uint32_t addr = channels[1].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction == 0) {
        mdec_debug_dma_out_start(addr, total_words);
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, mdec_dma_read_word());
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        mdec_debug_dma_out_end(addr, total_words);
        channels[1].madr = addr;
    }

    complete_transfer(1);
}

static void source_gpu_upload_words(void) {
    int queued=source_gpu_runtime_active();
    unsigned to_gpu=channels[2].chcr&1u;
    g_dma_cur_ch=2;g_dma_initiator_pc=s_dma_ch_initiator_pc[2];
    g_dma_cur_madr=channels[2].madr;g_dma_cur_bcr=channels[2].bcr;
    while(gpu_upload_source.remaining && gpu_upload_source.budget>0) {
        if(!queued && gpu_dma_vram_upload_words()<gpu_upload_source.remaining) {
            fprintf(stderr,"[dma-model] source GPU upload changed during transfer\n");exit(2);
        }
        if(!gpu_upload_source.in_block) {
            /* Request feedback is sampled at block boundaries, including
             * when the packet header is still queued behind a draw. */
            if(queued && to_gpu && gpu_dma_source_ll_ready()!=1)break;
            gpu_upload_source.in_block=gpu_upload_source.block_size;
            gpu_upload_source.address=channels[2].madr&0xfffffcu;
            channels[2].bcr-=1u<<16;
            gpu_upload_source.budget-=7;
        }
        if(gpu_upload_source.address&0x800000u) {
            fprintf(stderr,"[dma-model] unsupported source GPU request payload address\n");exit(2);
        }
        uint32_t at=gpu_upload_source.address&0x1ffffcu;
        if(to_gpu) {
            uint32_t value=psx_read_word(at);
            gpu_set_gp0_source(at);gpu_write_gp0(value);
        } else psx_write_word(at,gpu_read_gpuread());
        gpu_upload_source.address=(gpu_upload_source.address+((channels[2].chcr&2u)?-4u:4u))&0xffffffu;
        gpu_upload_source.in_block--;gpu_upload_source.remaining--;
        gpu_upload_source.budget--;
        if(!gpu_upload_source.in_block)channels[2].madr=gpu_upload_source.address;
        if(!gpu_upload_source.remaining)complete_transfer(2);
    }
    if(gpu_upload_source.budget>0)gpu_upload_source.budget=0;
    uint32_t n=gpu_upload_source.block_size?gpu_upload_source.block_size-1u:0u;
    int can= !queued || !to_gpu || gpu_dma_source_ll_ready()==1;
    gpu_upload_live_read_wait=gpu_upload_source.remaining && can?(n>200u?200u:n):0u;
    if(!cpu_read_wait_latched)g_dma_cpu_read_wait=gpu_upload_live_read_wait;
}
static void start_source_gpu_upload(void) {
    uint32_t bs=channels[2].bcr&0xffffu,bc=channels[2].bcr>>16;
    uint64_t total=(uint64_t)bs*bc;
    unsigned mode=channels[2].chcr&0x00ffffffu;
    int queued=source_gpu_runtime_active();
    if((queued?(mode&~3u)!=0x200u:mode!=0x201u) || !bs || !bc ||
       (channels[2].madr&0x800000u) || (!queued && total>gpu_dma_vram_upload_words())) {
        fprintf(stderr,"[dma-model] unsupported source GPU request: CHCR=%08X BCR=%08X MADR=%08X upload=%u\n",channels[2].chcr,channels[2].bcr,channels[2].madr,gpu_dma_vram_upload_words());exit(2);
    }
    gpu_upload_source.remaining=(uint32_t)total;
    gpu_upload_source.block_size=bs;gpu_upload_source.in_block=0;
    gpu_upload_source.budget=64;
    gpu_upload_source.last_cycle=psx_cycle_count;
    gpu_upload_source.next_cycle=psx_cycle_count+128u-(psx_cycle_count&127u);
    source_gpu_upload_words();
}
static void advance_source_gpu(void) {
    while(gpu_upload_source.remaining && psx_cycle_count>=gpu_upload_source.next_cycle) {
        gpu_upload_source.budget+=(int32_t)(gpu_upload_source.next_cycle-gpu_upload_source.last_cycle);
        gpu_upload_source.last_cycle=gpu_upload_source.next_cycle;
        gpu_upload_source.next_cycle+=128;
        source_gpu_upload_words();
    }
}
/* Bounded source compatibility experiment. Independently implemented from
 * observed original-core facts: 64 startup clocks, 15/10 per header, one per
 * payload, ready checked at a node boundary, 128-clock subsequent service.
 * Only NOP/environment payloads and the qualified parser-ready states are
 * admitted. General draw/FIFO timing, forced stop and restore are excluded. */
static void source_gpu_ll_words(void) {
    g_dma_cur_ch=2;g_dma_initiator_pc=s_dma_ch_initiator_pc[2];
    while(gpu_ll_source.active && gpu_ll_source.budget>0) {
        if(!gpu_ll_source.remaining) {
            int ready=gpu_dma_source_ll_ready();
            if(ready<0) {
                fprintf(stderr,"[dma-model] unsupported source linked-list GPU parser state\n");exit(2);
            }
            if(!ready)break;
            if(channels[2].madr&0x800000u) {
                fprintf(stderr,"[dma-model] unsupported source linked-list address %08X\n",channels[2].madr);exit(2);
            }
            uint32_t at=channels[2].madr&0x1ffffcu;
            uint32_t header=psx_read_word(at);
            gpu_ll_source.remaining=header>>24;
            gpu_ll_source.address=(channels[2].madr+4u)&0xffffffu;
            channels[2].madr=header&0xffffffu;
            gpu_set_gp0_linked_list_node(at,gpu_ll_source.nodes++);
            gpu_ll_source.budget-=gpu_ll_source.remaining?15:10;
        } else {
            if(gpu_ll_source.address&0x800000u) {
                fprintf(stderr,"[dma-model] unsupported source linked-list payload address\n");exit(2);
            }
            uint32_t at=gpu_ll_source.address&0x1ffffcu;
            uint32_t value=psx_read_word(at),opcode=value>>24;
            if(!source_gpu_runtime_active() && (gpu_dma_source_ll_ready()!=1 ||
               !(opcode==0 || opcode==1 || (opcode>=0xe1 && opcode<=0xe6)))) {
                fprintf(stderr,"[dma-model] unsupported source linked-list payload %08X at %08X\n",value,at);exit(2);
            }
            gpu_set_gp0_source(at);gpu_write_gp0(value);
            gpu_ll_source.address=(gpu_ll_source.address+4u)&0xffffffu;
            gpu_ll_source.remaining--;gpu_ll_source.budget--;
        }
        if(!gpu_ll_source.remaining && channels[2].madr==0xffffffu) {
            gpu_ll_source.active=0;complete_transfer(2);
        }
    }
    /* Source discards unused positive clocks at a ready stall/completion. */
    if(gpu_ll_source.budget>0)gpu_ll_source.budget=0;
}
static void start_source_gpu_ll(void) {
    if((channels[2].chcr&0x00ffffffu)!=0x401u) {
        fprintf(stderr,"[dma-model] unsupported source linked-list CHCR=%08X\n",channels[2].chcr);exit(2);
    }
    memset(&gpu_ll_source,0,sizeof(gpu_ll_source));
    gpu_ll_source.active=1;gpu_ll_source.budget=64;
    gpu_ll_source.last_cycle=psx_cycle_count;
    gpu_ll_source.next_cycle=psx_cycle_count+128u-(psx_cycle_count&127u);
    source_gpu_ll_words();
    psx_devices_service_to_now();
}
static void advance_source_gpu_ll(void) {
    while(gpu_ll_source.active && psx_cycle_count>=gpu_ll_source.next_cycle) {
        gpu_ll_source.budget+=(int32_t)(gpu_ll_source.next_cycle-gpu_ll_source.last_cycle);
        gpu_ll_source.last_cycle=gpu_ll_source.next_cycle;
        gpu_ll_source.next_cycle+=128u;source_gpu_ll_words();
    }
}
static void advance_source_cdrom(void);
static void source_cdrom_words(void);
static void advance_source_otc(void);
static void advance_source_otc_words(uint32_t budget);
static void advance_source_spu(void);
static void source_spu_words(void);
void dma_source_gpu_service_at(uint64_t cycle) {
    if(cycle!=psx_cycle_count) {
        fprintf(stderr,"[dma-model] source GPU service outside scheduler cycle\n");exit(2);
    }
    g_dma_exec_depth++;
    service_source_mdec(cycle);
    advance_source_gpu();
    if(gpu_upload_source.remaining && cycle>gpu_upload_source.last_cycle) {
        gpu_upload_source.budget+=(int32_t)(cycle-gpu_upload_source.last_cycle);
        gpu_upload_source.last_cycle=cycle;source_gpu_upload_words();
    }
    advance_source_gpu_ll();
    if(gpu_ll_source.active && cycle>gpu_ll_source.last_cycle) {
        gpu_ll_source.budget+=(int32_t)(cycle-gpu_ll_source.last_cycle);
        gpu_ll_source.last_cycle=cycle;source_gpu_ll_words();
    }
    /* Original DMA_Update services every channel on register writes and
     * frontend returns, including the fraction since the last 128-clock
     * event. Preserve that global event phase and charge each interval once. */
    advance_source_cdrom();
    if(cd_source_model && cdrom_async.active && cycle>cd_source.last_cycle) {
        cd_source.budget+=(int32_t)(cycle-cd_source.last_cycle);
        cd_source.last_cycle=cycle;source_cdrom_words();
    }
    advance_source_spu();
    if(spu_source.remaining && cycle>spu_source.last_cycle) {
        spu_source.budget+=(int32_t)(cycle-spu_source.last_cycle);
        spu_source.last_cycle=cycle;source_spu_words();
    }
    advance_source_otc();
    if(otc_source_model && otc_source.remaining && cycle>otc_source.last_cycle) {
        uint32_t elapsed=(uint32_t)(cycle-otc_source.last_cycle);
        otc_source.last_cycle=cycle;advance_source_otc_words(elapsed);
    }
    g_dma_exec_depth--;
}
uint32_t dma_cpu_read_penalty(void) {
    return g_dma_cpu_read_wait;
}
void dma_cpu_read_wait_boundary(void) {
    /* Source RunReal does not service an event crossed by fetch/base cycles
     * until the instruction's memory access (MMIO) or retirement (RAM/ROM).
     * Its ReadMemory therefore sees the DMA steal from instruction entry.
     * Our scheduler may already have completed DMA during the fetch refill;
     * preserve that entry wait, including for compiled RAM fast-path routing. */
    cpu_read_wait_latched=1;
    g_dma_cpu_read_wait=gpu_upload_live_read_wait;
}

static uint32_t execute_ch2_gpu(void) {
    uint32_t chcr = channels[2].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM (to device) */
    uint32_t step = (chcr >> 1) & 1;         /* 0=forward(+4), 1=backward(-4) */
    uint32_t sync_mode = (chcr >> 9) & 3;    /* 0=burst, 1=block, 2=linked-list */
    uint32_t actual_words = 0;

    if (direction == 0) {
        /* GPU → RAM (VRAM read): read pixel data via GPUREAD.
         * A prior GP0(C0h) command must have set up the VRAM read region. */
        if (sync_mode == 1) {
            uint32_t block_size = channels[2].bcr & 0xFFFF;
            uint32_t block_count = (channels[2].bcr >> 16) & 0xFFFF;
            uint32_t total_words = block_size * block_count;
            uint32_t addr = channels[2].madr & 0x1FFFFCu;
            int32_t  addr_step = step ? -4 : 4;
            for (uint32_t i = 0; i < total_words; i++) {
                uint32_t pixel_data = gpu_read_gpuread();
                psx_write_word(addr, pixel_data);
                addr = (addr + addr_step) & 0x1FFFFCu;
            }
            channels[2].madr = addr;
            actual_words = total_words;
        }
        return actual_words;
    }

    /* direction == 1: RAM → GPU */
    if (sync_mode == 1) {
        /* Block mode: BCR bits 0-15 = block size (words), bits 16-31 = block count */
        uint32_t block_size = channels[2].bcr & 0xFFFF;
        uint32_t block_count = (channels[2].bcr >> 16) & 0xFFFF;
        uint32_t total_words = block_size * block_count;
        uint32_t addr = channels[2].madr & 0x1FFFFCu; /* mask to RAM, word-aligned */
        int32_t  addr_step = step ? -4 : 4;

        for (uint32_t i = 0; i < total_words; i++) {
            uint32_t word = psx_read_word(addr);
            gpu_set_gp0_source(addr);
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
        actual_words = total_words;
    } else if (sync_mode == 2) {
        /* Linked-list mode: follow ordering table in RAM.
         * Each node: bits 24-31 = number of words following header,
         *            bits 0-23  = next node address (0xFFFFFF = end).
         * The words following the header are sent to GP0.
         *
         * PSX_ND_SIB_FLAP_LAST=1 (opt-in): skip additive PolyGT3 (GP0 0x36)
         * whose signed 11-bit SX max is >= 280 AND whose OT rank is in the ND
         * digit-rain band (~1832). Was a flap-overpaint workaround; after the
         * AVSZ3 MAC0 fix it shreds the crate glow fountain — leave unset/0.
         *
         * OT-rank gate is mandatory: the same SX filter matches menu/char-select
         * 0x36 (e.g. ot~1066) and would strip Crash face/trophy semis. Rank is
         * the empty-node count used by gpu_set_gp0_linked_list_node.
         *
         * SX must be parsed as signed 11-bit (gpu.c parse_vertex). Legacy alias:
         * PSX_ND_OT_OPAQUE_LAST. */
        static int s_nd_flap_last = -1;
        if (s_nd_flap_last < 0) {
            const char *e = getenv("PSX_ND_SIB_FLAP_LAST");
            if (!e || !*e)
                e = getenv("PSX_ND_OT_OPAQUE_LAST");
            s_nd_flap_last = (e && *e && *e != '0') ? 1 : 0;
            if (s_nd_flap_last)
                fprintf(stdout, "psxrecomp: PSX_ND_SIB_FLAP_LAST enabled\n");
        }

        gpu_ws_begin_linked_list();
        uint32_t start_addr =
            psx_mod_gpu_dma_resolve_address(channels[2].madr);
        gpu_ws_prepass_linked_list(start_addr);
        const uint32_t MAX_NODES = 0x40000; /* prevent infinite loops */
        uint32_t last_addr = start_addr;
        int hit_limit = 0;
        /* Mirror gpu.c empty-node OT rank (0 after first empty header). */
        uint32_t ot_rank = 0xffffffffu;

        uint32_t addr = start_addr;
        uint32_t safety = 0;
        for (;;) {
            if (safety++ > MAX_NODES) {
                hit_limit = 1;
                last_addr = addr;
                break;
            }

            uint32_t header = psx_read_word(addr);
            uint32_t num_words = (header >> 24) & 0xFF;
            uint32_t word_addr =
                psx_mod_gpu_dma_resolve_address(addr + 4u);

            if (num_words == 0u)
                ot_rank = (ot_rank == 0xffffffffu) ? 0u : (ot_rank + 1u);

            int emit = 1;
            if (s_nd_flap_last && num_words >= 6u &&
                ot_rank >= 1600u && ot_rank < 2100u) {
                uint32_t cmd = psx_read_word(word_addr);
                uint32_t op = (cmd >> 24) & 0xFFu;
                if (op == 0x36u) {
                    /* Match gpu.c parse_vertex: signed 11-bit SX. Digit-rain
                     * packets often stash junk in the high bits of the XY word;
                     * int16 SX mis-classifies them. Skip glow whose s11 bbox
                     * reaches the sibling right-flap band (sx_max >= 280). */
                    int32_t sx_max = -0x8000;
                    for (uint32_t vi = 1; vi <= 5; vi += 2) {
                        uint32_t xy = psx_read_word(
                            psx_mod_gpu_dma_resolve_address(word_addr +
                                                            vi * 4u));
                        int32_t sx = (int32_t)(xy & 0x7FFu);
                        if (sx & 0x400)
                            sx -= 0x800;
                        if (sx > sx_max)
                            sx_max = sx;
                    }
                    if (sx_max >= 280)
                        emit = 0;
                }
            }

            if (emit) {
                gpu_set_gp0_linked_list_node(addr, num_words);
                actual_words += 1u;
                for (uint32_t i = 0; i < num_words; i++) {
                    uint32_t word = psx_read_word(word_addr);
                    gpu_set_gp0_source(word_addr);
                    gpu_write_gp0(word);
                    word_addr =
                        psx_mod_gpu_dma_resolve_address(word_addr + 4u);
                }
                actual_words += num_words;
            }

            uint32_t next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu) {
                last_addr = 0x00FFFFFFu;
                break;
            }
            addr = psx_mod_gpu_dma_resolve_address(next);
            last_addr = addr;
        }
        (void)hit_limit;

        channels[2].madr = last_addr;
        gpu_ws_end_linked_list();
    } else {
        /* Burst mode (sync_mode == 0) */
        uint32_t word_count = channels[2].bcr & 0xFFFF;
        if (word_count == 0) word_count = 0x10000; /* 0 means 0x10000 */
        uint32_t addr = channels[2].madr & 0x1FFFFCu;
        int32_t  addr_step = step ? -4 : 4;

        for (uint32_t i = 0; i < word_count; i++) {
            uint32_t word = psx_read_word(addr);
            gpu_set_gp0_source(addr);
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
        actual_words = word_count;
    }

    return actual_words;
}

static void source_cdrom_words(void) {
    DMAAsyncChannel *a=&cdrom_async;
    uint32_t addr=channels[3].madr&0x1FFFFCu;
    int32_t step=(channels[3].chcr&2u)?-4:4;
    if(a->remaining_words==a->total_words) {
        uint32_t bytes=a->total_words*4u;
        if(bytes>0x200000u-addr)bytes=0x200000u-addr;
        if(addr<0x1C0000u)overlay_capture_before_dma(addr,bytes);
    }
    while(a->active && a->remaining_words && cd_source.budget>0) {
        uint32_t word=cdrom_dma_read_padded();
        g_dma_cur_ch=3;g_dma_cur_madr=addr;g_dma_cur_bcr=channels[3].bcr;
        g_dma_initiator_pc=s_dma_ch_initiator_pc[3];
        psx_write_word(addr,word);record_cdrom_dma_word(word);
        dirty_ram_mark_executable_range(addr,4);
        addr=(addr+step)&0x1FFFFCu;
        a->remaining_words--;cd_source.budget-=9;
        channels[3].madr=addr;
        if(!a->remaining_words)finish_async_cdrom_transfer(addr);
    }
}

static void start_source_cdrom(void) {
    cd_source.budget=64;cd_source.last_cycle=psx_cycle_count;
    cd_source.next_cycle=psx_cycle_count+128u-(psx_cycle_count&127u);
    source_cdrom_words();
}

static void advance_source_cdrom(void) {
    while(cd_source_model && cdrom_async.active && psx_cycle_count>=cd_source.next_cycle) {
        cd_source.budget+=(int32_t)(cd_source.next_cycle-cd_source.last_cycle);
        cd_source.last_cycle=cd_source.next_cycle;cd_source.next_cycle+=128u;
        source_cdrom_words();
    }
}

static void execute_ch3_cdrom(void) {
    uint32_t chcr = channels[3].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM */

    if(cd_source_model && ((chcr&0x601u)!=0 || (channels[3].madr&0x800000u))) {
        fprintf(stderr,"[dma-model] source CD timing requires valid manual to-RAM transfer: frame=%llu cycle=%llu CHCR=%08X MADR=%08X BCR=%08X\n",
                (unsigned long long)s_frame_count,(unsigned long long)psx_cycle_count,
                chcr,channels[3].madr,channels[3].bcr);
        exit(2);
    }
    if (direction != 0) {
        channels[3].chcr &= ~((1u << 24) | (1u << 28));
        return;
    }

    start_async_cdrom_transfer();
    if(cd_source_model) {
        start_source_cdrom();
        /* Source's CPU owns fetch attempts during a manual DMA halt, just as
         * for OTC. Let the CHCR store retire before that boundary runs.
         * Standalone controller callers retain their synchronous contract. */
        if(source_gpu_runtime_active())return;
        while(cdrom_async.active && !(chcr&0x100u)) {
            uint32_t wait=cd_source.next_cycle>psx_cycle_count?
                (uint32_t)(cd_source.next_cycle-psx_cycle_count):1u;
            psx_advance_cycles(wait);
            /* The MMIO pre-barrier can cache a later device deadline before
             * this transfer is armed. Service each new DMA boundary even when
             * that cached deadline has not expired, as the OTC halt does. */
            psx_devices_service_to_now();
        }
    }
}

/* Returns the number of words moved so the caller can schedule a faithful
 * delayed completion (DMA_SPU_CYCLES_PER_WORD). The payload moves immediately
 * (SPU RAM is correct the instant this returns); only the busy-bit clear and
 * completion IRQ are deferred by schedule_delayed_complete. */
static uint32_t execute_ch4_spu(void) {
    uint32_t chcr = channels[4].chcr;
    uint32_t direction = chcr & 1;           /* 1=from RAM to SPU, 0=SPU to RAM */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(4);
    uint32_t addr = channels[4].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction != 0) {
        for (uint32_t i = 0; i < total_words; i++) {
            spu_dma_write(psx_read_word(addr));
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        /* One aggregated event per SPU-bound transfer (per-word would flood
         * the ring: a full sound-bank upload is ~128k words). */
        audio_trace_event(AUDIO_EV_DMA_WRITE, total_words,
                          channels[4].madr & 0x1FFFFCu);
    } else {
        /* SPU RAM -> CPU RAM. This direction previously zero-filled the
         * destination, which is not a transfer at all: SPU RAM is readable
         * memory and games do read it back. Titles that carry state through
         * SPU RAM across an Exec boundary (a checksummed block surviving an
         * EXE swap, since SPU RAM is one of the few regions main RAM's reload
         * does not touch) got zeros, failed their own integrity check, and
         * fell back to a cold-boot path. */
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, spu_dma_read());
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        audio_trace_event(AUDIO_EV_DMA_READ, total_words,
                          channels[4].madr & 0x1FFFFCu);
    }

    channels[4].madr = addr;
    return total_words;
}

static void source_spu_words(void) {
    g_dma_cur_ch=4;g_dma_initiator_pc=s_dma_ch_initiator_pc[4];
    while(spu_source.remaining && spu_source.budget>0) {
        if(!spu_source.in_block) {
            spu_source.in_block=spu_source.block_size;
            spu_source.address=channels[4].madr&0xffffffu;
            channels[4].bcr-=1u<<16;
        }
        if(spu_source.address&0x800000u) {
            fprintf(stderr,"[dma-model] source SPU request payload address unsupported\n");exit(2);
        }
        uint32_t at=spu_source.address&0x1ffffcu;
        g_dma_cur_madr=spu_source.address;g_dma_cur_bcr=channels[4].bcr;
        if(channels[4].chcr&1u)spu_dma_write(psx_read_word(at));
        else psx_write_word(at,spu_dma_read());
        spu_source.address=(spu_source.address+((channels[4].chcr&2u)?-4u:4u))&0xffffffu;
        spu_source.budget-=48;spu_source.in_block--;spu_source.remaining--;
        if(!spu_source.in_block)channels[4].madr=spu_source.address;
        if(!spu_source.remaining) {
            audio_trace_event((channels[4].chcr&1u)?AUDIO_EV_DMA_WRITE:AUDIO_EV_DMA_READ,
                              spu_source.total_words,spu_source.start_addr);
            complete_transfer(4);
        }
    }
    if(spu_source.budget>0)spu_source.budget=0;
}
static void start_source_spu(void) {
    uint32_t bs=channels[4].bcr&0xffffu,bc=channels[4].bcr>>16;
    if((channels[4].chcr&0x00fffffcu)!=0x200u || !bs || !bc ||
       (channels[4].madr&0x800000u)) {
        fprintf(stderr,"[dma-model] source SPU request unsupported CHCR=%08X BCR=%08X MADR=%08X\n",
                channels[4].chcr,channels[4].bcr,channels[4].madr);exit(2);
    }
    spu_source.remaining=spu_source.total_words=bs*bc;
    spu_source.block_size=bs;spu_source.in_block=0;spu_source.budget=64;
    spu_source.start_addr=channels[4].madr&0x1ffffcu;
    spu_source.last_cycle=psx_cycle_count;
    spu_source.next_cycle=psx_cycle_count+128u-(psx_cycle_count&127u);
    source_spu_words();
}
static void advance_source_spu(void) {
    while(spu_source.remaining && psx_cycle_count>=spu_source.next_cycle) {
        spu_source.budget+=(int32_t)(spu_source.next_cycle-spu_source.last_cycle);
        spu_source.last_cycle=spu_source.next_cycle;spu_source.next_cycle+=128;
        source_spu_words();
    }
}

static void advance_source_otc_words(uint32_t budget) {
    while (budget-- && otc_source.remaining) {
        if (otc_source.address & 0x800000u) {
            otc_source.remaining = 0;
            channels[6].chcr &= ~((1u << 24) | (1u << 28));
            uint32_t before = dicr;
            dicr |= 0x8000u;
            raise_dma_irq_on_master_edge(before);
            return;
        }
        uint32_t value = otc_source.remaining == 1u ? 0xFFFFFFu :
                         (otc_source.address - 4u) & 0xFFFFFFu;
        g_dma_cur_ch = 6; g_dma_cur_madr = otc_source.address;
        g_dma_cur_bcr = channels[6].bcr;
        g_dma_initiator_pc = s_dma_ch_initiator_pc[6];
        psx_write_word(otc_source.address & 0x1FFFFCu, value);
        otc_source.address = (otc_source.address - 4u) & 0xFFFFFFu;
        if (--otc_source.remaining == 0) complete_transfer(6);
    }
}

static void start_source_otc(void) {
    otc_source.remaining = channels[6].bcr & 0xFFFFu;
    if (!otc_source.remaining) otc_source.remaining = 65536u;
    otc_source.address = channels[6].madr & 0xFFFFFCu;
    otc_source.last_cycle = psx_cycle_count;
    otc_source.next_cycle = psx_cycle_count + 128u - (psx_cycle_count & 127u);
    advance_source_otc_words(64u);
}

static void advance_source_otc(void) {
    while (otc_source.remaining && psx_cycle_count >= otc_source.next_cycle) {
        uint32_t elapsed = (uint32_t)(otc_source.next_cycle - otc_source.last_cycle);
        otc_source.last_cycle = otc_source.next_cycle;
        otc_source.next_cycle += 128u;
        advance_source_otc_words(elapsed);
    }
}

static void execute_ch6_otc(void) {
    if (otc_source_model) {
        start_source_otc();
        /* The source CPU fetches while halted. Its functional instruction
         * boundary owns that overlap when the source CPU/GPU profile is on.
         * Standalone controller callers retain the synchronous contract. */
        if(source_gpu_runtime_active())return;
        /* The ordinary CHCR store does not return to guest execution until
         * the source-style halt ends. All devices keep advancing. Source's
         * next instruction fetch may overlap this halt; this adapter does
         * not claim fetch/cache phase or physical write-queue equivalence. */
        while (otc_source.remaining) {
            uint32_t wait = otc_source.next_cycle > psx_cycle_count ?
                (uint32_t)(otc_source.next_cycle - psx_cycle_count) : 1u;
            psx_advance_cycles(wait);
            psx_devices_service_to_now();
        }
        return;
    }
    /* OTC (Ordering Table Clear): writes a backward-linked list to RAM.
     * Node N = address of node N-1, node 0 = 0xFFFFFF (end marker).
     * Direction is always to-RAM, step is always backward.
     * BCR bits 0-15 = number of entries. */
    uint32_t num_entries = channels[6].bcr & 0xFFFF;
    if (num_entries == 0) num_entries = 0x10000;
    uint32_t addr = channels[6].madr & 0x1FFFFCu;

    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t val;
        if (i == num_entries - 1) {
            /* Last entry (first in memory): end marker */
            val = 0x00FFFFFFu;
        } else {
            /* Points to the previous entry (addr - 4) */
            val = (addr - 4) & 0x00FFFFFFu;
        }
        psx_write_word(addr, val);
        addr = (addr - 4) & 0x1FFFFCu;
    }

    complete_transfer(6);
}

int dma_cpu_otc_halted(void) {
    return otc_source_model && otc_source.remaining!=0u;
}

int dma_cpu_source_halted(void) {
    return dma_cpu_otc_halted() ||
           (cd_source_model && cdrom_async.active &&
            !(channels[3].chcr&0x100u));
}

static void execute_ch5_pio(void) {
    /* PIO (Parallel I/O) — used for expansion port / parallel port transfers.
     * Very simple: just move words directly to/from RAM with no device interaction.
     * Direction: 0 = to RAM (read from device), 1 = from RAM (write to device).
     * For now, we just complete the transfer immediately as PIO devices are
     * typically slow and the game handles timing via busy-wait on the port. */
    uint32_t chcr = channels[5].chcr;
    uint32_t direction = chcr & 1u;
    uint32_t step = (chcr >> 1) & 1u;
    uint32_t total_words = transfer_word_count(5);
    uint32_t addr = channels[5].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction == 1) {
        /* from RAM to device: just read and discard */
        for (uint32_t i = 0; i < total_words; i++) {
            (void)psx_read_word(addr);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
    } else {
        /* to RAM from device: write zeros (device not emulated) */
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, 0);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
    }
    channels[5].madr = addr;
    complete_transfer(5);
}

static void try_execute(int ch) {
    uint32_t chcr = channels[ch].chcr;

    /* Transfer starts when bit 24 (start/busy) is set AND channel is enabled in DPCR */
    if (!((chcr >> 24) & 1)) return;
    if (!channel_enabled(ch)) return;

    /* Source CD and SPU retain the trigger in their live registers until
     * completion, observable while the CPU is allowed to keep running.
     * Default and standalone controller callers retain their old image. */
    if(!((ch==4 || (ch==3 && cd_source_model)) && source_gpu_runtime_active()))
        channels[ch].chcr &= ~(1u << 28);
    trace_dma('S', ch, transfer_word_count(ch), dicr, i_stat);
    event_ring_record_aux(EV_DMA_KICK, (uint8_t)ch, channels[ch].chcr);
    event_ring_record_aux(EV_ENQ, (uint8_t)(SRC_DMA0 + ch), transfer_word_count(ch));

    /* After the kick is in the rings, refuse corrupt-length transfers. */
    validate_transfer_length(ch);

    /* Capture the kick PC (this CHCR store) so both the immediate (sync) writes
     * below and any deferred async writes for this channel attribute to it. */
    s_dma_ch_initiator_pc[ch] = g_debug_last_store_pc;
    g_dma_initiator_pc        = g_debug_last_store_pc;
    g_dma_exec_depth++;
    g_dma_cur_ch = ch; g_dma_cur_madr = channels[ch].madr; g_dma_cur_bcr = channels[ch].bcr;
    switch (ch) {
        case 0:
            if(mdec_source_active())start_source_mdec(0);
            else start_async_mdec_transfer(0);
            break;
        case 1:
            if(mdec_source_active())start_source_mdec(1);
            else start_async_mdec_transfer(1);
            break;
        case 2:
            if(gpu_upload_source_model && ((channels[2].chcr>>9)&3u)==1u)
                start_source_gpu_upload();
            else if(gpu_ll_source_model && ((channels[2].chcr>>9)&3u)==2u)
                start_source_gpu_ll();
            else schedule_delayed_complete(2, execute_ch2_gpu(),
                                           DMA_GPU_CYCLES_PER_WORD);
            break;
        case 3:
            execute_ch3_cdrom();
            break;
        case 4:
            if(source_gpu_runtime_active())start_source_spu();
            else schedule_delayed_complete(4, execute_ch4_spu(),
                                           DMA_SPU_CYCLES_PER_WORD);
            break;
        case 5:
            execute_ch5_pio();
            break;
        case 6:
            execute_ch6_otc();
            break;
        default: {
            /* Other channels not implemented yet — fatal if transfer is triggered */
            static char reason[96];
            snprintf(reason, sizeof(reason),
                     "DMA ch%d: transfer triggered but not implemented (CHCR=0x%08X)",
                     ch, chcr);
            psx_fatal_halt(reason);
        }
    }
    g_dma_exec_depth--;
    g_dma_cur_ch = -1;
}

/* ---- Public interface ---- */

uint32_t dma_get_dicr(void) { return dicr_read_value(dicr); }
uint32_t dma_get_dpcr(void) { return dpcr; }
int dma_cdrom_transfer_active(void) {
    return cdrom_async.active &&
           ((channels[3].chcr >> 24) & 1u) &&
           channel_enabled(3) &&
           ((channels[3].chcr & 1u) == 0);
}

/* Cycle-budgeted precise event slicing: guest CPU cycles until DMA raises a
 * DELIVERABLE IRQ (bit3 unmasked in i_mask). UINT32_MAX if none. Conservative
 * under-estimate: for async channels uses a 1-cycle/word floor minus cycles
 * already accumulated (always <= the true remaining, since per-word cost >= 1),
 * and the exact countdown for delayed-complete channels. Over-slicing on a
 * channel whose DICR completion is masked is safe. See PRECISE_IRQ_SLICE.md. */
static uint32_t source_gpu_dma_irq_bound(void) {
    uint32_t best=0xFFFFFFFFu;
    if(gpu_ll_source.active)
        best=gpu_ll_source.next_cycle>psx_cycle_count?
            (uint32_t)(gpu_ll_source.next_cycle-psx_cycle_count):1u;
    if(gpu_upload_source.remaining) {
        uint32_t d=gpu_upload_source.next_cycle>psx_cycle_count?
            (uint32_t)(gpu_upload_source.next_cycle-psx_cycle_count):1u;
        if(d<best)best=d;
    }
    return best;
}

uint32_t dma_cycles_to_irq(uint32_t i_mask) {
    if (!(i_mask & (1u << 3))) return 0xFFFFFFFFu;   /* IRQ_DMA masked */
    /* Source GPU transfers complete during periodic DMA service, without a
     * delayed_complete entry. A compiled block must stop before that service
     * can raise its completion IRQ. This is a conservative bound, not a new
     * transfer duration: readiness can postpone completion beyond the event. */
    uint32_t best = source_gpu_dma_irq_bound();
    uint32_t mdec_bound=source_mdec_dma_bound(0);if(mdec_bound<best)best=mdec_bound;
    if(spu_source.remaining) {
        uint32_t d=spu_source.next_cycle>psx_cycle_count?
            (uint32_t)(spu_source.next_cycle-psx_cycle_count):1u;
        if(d<best)best=d;
    }
    const DMAAsyncChannel *async_ch[3] = { &mdec_async[0], &mdec_async[1], &cdrom_async };
    for (int i = 0; i < 3; i++) {
        const DMAAsyncChannel *a = async_ch[i];
        if (!a->active || a->remaining_words == 0) continue;
        /* floor: rw*per_word - accum >= rw - accum (per_word >= 1). Clamp >=0. */
        uint32_t est = a->remaining_words > a->cycles_accum
                         ? (a->remaining_words - a->cycles_accum) : 0u;
        if (est < best) best = est;
    }
    for (int ch = 0; ch < 7; ch++) {
        if (delayed_complete[ch].active && delayed_complete[ch].cycles_remaining < best)
            best = delayed_complete[ch].cycles_remaining;
    }
    return best;
}

uint32_t dma_cycles_to_internal_event(void) {
    uint32_t best = source_mdec_dma_bound(0);
    if(gpu_ll_source.active) {
        best=gpu_ll_source.next_cycle>psx_cycle_count?
            (uint32_t)(gpu_ll_source.next_cycle-psx_cycle_count):1u;
    }
    if(gpu_upload_source.remaining) {
        best=gpu_upload_source.next_cycle>psx_cycle_count?
            (uint32_t)(gpu_upload_source.next_cycle-psx_cycle_count):1u;
    }
    if (otc_source.remaining) {
        uint32_t d = otc_source.next_cycle > psx_cycle_count ?
            (uint32_t)(otc_source.next_cycle - psx_cycle_count) : 1u;
        if(d<best)best=d;
    }
    if(cd_source_model && cdrom_async.active) {
        uint32_t d=cd_source.next_cycle>psx_cycle_count?
            (uint32_t)(cd_source.next_cycle-psx_cycle_count):1u;
        if(d<best)best=d;
    }
    if(spu_source.remaining) {
        uint32_t d=spu_source.next_cycle>psx_cycle_count?
            (uint32_t)(spu_source.next_cycle-psx_cycle_count):1u;
        if(d<best)best=d;
    }

    /* Async MDEC channels move one word whenever their cycle accumulator
     * reaches the channel cost. Completion-only scheduling batches many RAM
     * writes at a later service boundary, which is observably different when
     * the CPU polls the destination buffer. */
    for (int ch = 0; ch < 2; ch++) {
        const DMAAsyncChannel *a = &mdec_async[ch];
        if (!a->active || a->remaining_words == 0 ||
            !((channels[ch].chcr >> 24) & 1u) || !channel_enabled(ch))
            continue;
        uint32_t direction = channels[ch].chcr & 1u;
        if ((ch == 0 && direction == 0) || (ch == 1 && direction != 0))
            return 1u; /* invalid route is completed on the next DMA tick */
        if ((ch == 0 && !mdec_dma_write_ready()) ||
            (ch == 1 && !mdec_dma_read_ready()))
            continue;
        uint32_t cpw = ch == 0 ? DMA_MDEC_IN_CYCLES_PER_WORD
                               : DMA_MDEC_OUT_CYCLES_PER_WORD;
        uint32_t d = a->cycles_accum < cpw ? cpw - a->cycles_accum : 1u;
        if (d < best) best = d;
    }

    /* CD-ROM DMA writes guest RAM incrementally. Expose each word on time;
     * waiting only for the channel-complete IRQ can delay hundreds of writes. */
    if (!cd_source_model && cdrom_async.active && cdrom_async.remaining_words != 0 &&
        ((channels[3].chcr >> 24) & 1u) && channel_enabled(3)) {
        if ((channels[3].chcr & 1u) != 0) {
            return 1u; /* unsupported RAM->CD direction cancels next tick */
        }
        if (cdrom_dma_ready()) {
            uint32_t d = cdrom_async.cycles_accum < DMA_CDROM_CYCLES_PER_WORD
                       ? DMA_CDROM_CYCLES_PER_WORD - cdrom_async.cycles_accum
                       : 1u;
            if (d < best) best = d;
        }
    }

    for (int ch = 0; ch < 7; ch++) {
        if (delayed_complete[ch].active &&
            delayed_complete[ch].cycles_remaining < best)
            best = delayed_complete[ch].cycles_remaining;
    }
    return best;
}

uint32_t dma_cycles_to_deliverable_irq(uint32_t i_mask) {
    if (!(i_mask & (1u << 3))) return 0xFFFFFFFFu;
    uint32_t best = channel_irq_flag_armed(2)?source_gpu_dma_irq_bound():0xFFFFFFFFu;
    uint32_t mdec_bound=source_mdec_dma_bound(1);if(mdec_bound<best)best=mdec_bound;
    if(spu_source.remaining && channel_irq_flag_armed(4)) {
        uint32_t d=spu_source.next_cycle>psx_cycle_count?
            (uint32_t)(spu_source.next_cycle-psx_cycle_count):1u;
        if(d<best)best=d;
    }
    const int async_num[3] = { 0, 1, 3 };
    const DMAAsyncChannel *async_ch[3] = {
        &mdec_async[0], &mdec_async[1], &cdrom_async
    };
    for (int i = 0; i < 3; i++) {
        const DMAAsyncChannel *a = async_ch[i];
        if (!channel_irq_flag_armed(async_num[i]) ||
            !a->active || a->remaining_words == 0) continue;
        uint32_t est = a->remaining_words > a->cycles_accum
                         ? (a->remaining_words - a->cycles_accum) : 0u;
        if (est < best) best = est;
    }
    for (int ch = 0; ch < 7; ch++) {
        if (channel_irq_flag_armed(ch) && delayed_complete[ch].active &&
            delayed_complete[ch].cycles_remaining < best)
            best = delayed_complete[ch].cycles_remaining;
    }
    return best;
}

void dma_advance(uint32_t cycles) {
    if (cycles == 0) return;
    g_dma_exec_depth++;   /* async to-RAM DMA writes below run through psx_write_word */
    advance_source_otc();
    advance_source_cdrom();
    advance_source_gpu();
    advance_source_gpu_ll();
    advance_mdec_channel(0, cycles);
    advance_source_spu();
    advance_mdec_channel(1, cycles);
    DMAAsyncChannel *a = &cdrom_async;
    if (!cd_source_model && dma_cdrom_transfer_active()) {
        uint32_t chcr = channels[3].chcr;
        uint32_t direction = chcr & 1u;
        uint32_t step = (chcr >> 1) & 1u;
        int32_t addr_step = step ? -4 : 4;

        if (direction != 0) {
            cancel_async_transfer(3);
            channels[3].chcr &= ~((1u << 24) | (1u << 28));
        } else if (cdrom_dma_ready()) {
            if (cycles > UINT32_MAX - a->cycles_accum) {
                a->cycles_accum = UINT32_MAX;
            } else {
                a->cycles_accum += cycles;
            }

            uint32_t words_budget = a->cycles_accum / DMA_CDROM_CYCLES_PER_WORD;
            uint32_t addr = channels[3].madr & 0x1FFFFCu;
            uint32_t moved = 0;
            g_dma_cur_ch = 3; g_dma_cur_bcr = channels[3].bcr;
            g_dma_initiator_pc = s_dma_ch_initiator_pc[3];  /* deferred: restore kick PC */
            /* Snapshot outgoing executed code at the last possible coherent
             * moment: after the async wait, immediately before the first RAM
             * word. Scheduling-time capture was too early because guest code
             * can continue executing while the CD device is not ready. */
            if (a->remaining_words == a->total_words && words_budget > 0 &&
                addr < 0x1C0000u) {
                uint32_t bytes = a->total_words * 4u;
                if (bytes > 0x200000u - addr) bytes = 0x200000u - addr;
                overlay_capture_before_dma(addr, bytes);
            }
            while (a->remaining_words > 0 && words_budget > 0 && cdrom_dma_ready()) {
                uint32_t word = cdrom_dma_read();
                g_dma_cur_madr = addr;
                psx_write_word(addr, word);
                record_cdrom_dma_word(word);
                dirty_ram_mark_executable_range(addr, 4);
                addr = (addr + addr_step) & 0x1FFFFCu;
                a->remaining_words--;
                words_budget--;
                moved++;
            }

            if (moved > 0) {
                a->cycles_accum -= moved * DMA_CDROM_CYCLES_PER_WORD;
                channels[3].madr = addr;
                if (a->remaining_words == 0) {
                    finish_async_cdrom_transfer(addr);
                }
            }
        }
    }
    /* Drive every delayed-complete channel (ch2 GPU + ch4 SPU today; any future
     * delayed channel is covered automatically — inactive slots no-op). */
    for (int ch = 0; ch < 7; ch++)
        advance_delayed_complete(ch, cycles);
    g_dma_cur_ch = -1;
    g_dma_exec_depth--;
}

void dma_init(void) {
    memset(mdec_source_dma,0,sizeof(mdec_source_dma));mdec_source_last_cycle=0;
    g_dma_cpu_read_wait=0;
    gpu_upload_live_read_wait=0;cpu_read_wait_latched=0;
    memset(&gpu_ll_source,0,sizeof(gpu_ll_source));
    memset(&gpu_upload_source,0,sizeof(gpu_upload_source));
    memset(&spu_source,0,sizeof(spu_source));
    const char *gpu_model=getenv("PSX_GPU_DMA_MODEL");
    gpu_upload_source_model=gpu_model && *gpu_model;
    gpu_ll_source_model=gpu_model && (!strcmp(gpu_model,"octoshock-2.2.2-bounded-linked-list") || !strcmp(gpu_model,"octoshock-2.2.2-bounded-quad"));
    if(gpu_upload_source_model && ((!gpu_ll_source_model && strcmp(gpu_model,"octoshock-2.2.2-vram-upload")) || !getenv("PSX_INPUT_ROUTE_FILE"))) {
        fprintf(stderr,"[dma-model] invalid source GPU upload model or missing route\n");exit(2);
    }
    memset(&otc_source, 0, sizeof(otc_source));
    memset(&cd_source,0,sizeof(cd_source));
    const char *cd_model=getenv("PSX_CD_DMA_MODEL");
    cd_source_model=cd_model && *cd_model;
    if(cd_source_model && (strcmp(cd_model,"octoshock-2.2.2") || !getenv("PSX_INPUT_ROUTE_FILE"))) {
        fprintf(stderr,"[dma-model] invalid source CD model or missing route\n");exit(2);
    }
    const char *model = getenv("PSX_INPUT_ROUTE_DMA_MODEL");
    otc_source_model = model && *model;
    if (otc_source_model &&
        (strcmp(model, "octoshock-2.2.2-otc") || !getenv("PSX_INPUT_ROUTE_FILE"))) {
        fprintf(stderr, "[dma-model] invalid source OTC model or missing input route\n");
        exit(2);
    }
    memset(channels, 0, sizeof(channels));
    memset(mdec_async, 0, sizeof(mdec_async));
    memset(&cdrom_async, 0, sizeof(cdrom_async));
    memset(delayed_complete, 0, sizeof(delayed_complete));
    /* Original2.2.2 Power clears DMAControl. Keep that source-profile cold
     * value distinct from the default hardware-style priority reset image. */
    dpcr = gpu_ll_source_model ? 0u : 0x07654321u;
    dicr = 0;
    dma_debug_clear_trace();
    dma_debug_clear_cdrom_history();
}

uint32_t dma_read(uint32_t addr) {
    /* DPCR */
    if (addr == 0x1F8010F0u) return dpcr;
    /* DICR */
    if (addr == 0x1F8010F4u) {
        return dma_get_dicr();
    }

    /* Per-channel registers: 0x1F801080 + ch*0x10 + offset */
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        uint32_t offset = addr - 0x1F801080u;
        int ch = offset / 0x10;
        int reg = offset % 0x10;

        if (ch > 6) goto bad;
        switch (reg) {
            case 0x00: return channels[ch].madr;
            case 0x04: return channels[ch].bcr;
            case 0x08: return channels[ch].chcr;
            case 0x0C: return 0;
            default: goto bad;
        }
    }

bad:
    /* Unmapped words inside the DMA register block (0x1F8010F8/0xFC, channel
     * reg offset 0x0C variants): real hardware open-buses them and Tomba2's
     * late-attract wild I/O sweep (BIOS bzero/read over a 0xDF80xxxx pointer)
     * reads straight through here. Beetle parity: return 0, no fault. */
    {
        extern uint64_t g_io_openbus_reads;
        g_io_openbus_reads++;
    }
    return 0;
}

void dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask) {
    source_gpu_runtime_dma_write();
    if(mdec_source_active())for(int ch=0;ch<2;ch++)
        if((channels[ch].chcr&(1u<<24)) &&
           ((addr>=0x1f801080u+16u*ch && addr<=0x1f80108bu+16u*ch) ||
            (addr==0x1f8010f0u && ((dpcr^val)&mask&(15u<<(4*ch)))))) {
            fprintf(stderr,"[dma-model] active MDEC register replacement unsupported\n");exit(2);
        }
    if(spu_source.remaining &&
       ((addr>=0x1f8010c0u && addr<=0x1f8010cbu) ||
        (addr==0x1f8010f0u && ((dpcr^val)&mask&0xf0000u)))) {
        fprintf(stderr,"[dma-model] active source SPU request register replacement unsupported\n");exit(2);
    }
    if(gpu_ll_source.active) {
        advance_source_gpu_ll();
        if(gpu_ll_source.active && psx_cycle_count>gpu_ll_source.last_cycle) {
            gpu_ll_source.budget+=(int32_t)(psx_cycle_count-gpu_ll_source.last_cycle);
            gpu_ll_source.last_cycle=psx_cycle_count;source_gpu_ll_words();
        }
        /* Source DMA_Write services elapsed work first. With no payload left,
         * clearing start stops before the next header: no GPU FIFO flush, no
         * completion IRQ, and no retained positive DMA credit. Other active
         * register changes and partial-payload cancellation remain guarded. */
        if(gpu_ll_source.active && !gpu_ll_source.remaining &&
           addr==0x1f8010a8u && mask==0xffffffffu &&
           channels[2].chcr==0x01000401u && val==0x00000401u) {
            gpu_ll_source.active=0;
            gpu_ll_source.budget=0;
        }
        if(gpu_ll_source.active &&
           ((addr>=0x1f8010a0u && addr<=0x1f8010abu) ||
            (addr==0x1f8010f0u && ((dpcr^val)&mask&0xf00u)))) {
            SourceGPUCommandProjection state;source_gpu_runtime_copy(0,&state);
            fprintf(stderr,"[dma-model] active source linked-list register replacement unsupported at %llu addr=%08X value=%08X mask=%08X pc=%08X MADR=%08X BCR=%08X CHCR=%08X payload=%08X remaining=%u nodes=%u budget=%d last=%llu next=%llu GPU=%d,%u,%u,%u,%08X\n",
                (unsigned long long)psx_cycle_count,addr,val,mask,g_debug_last_store_pc,
                channels[2].madr,channels[2].bcr,channels[2].chcr,gpu_ll_source.address,
                gpu_ll_source.remaining,gpu_ll_source.nodes,gpu_ll_source.budget,
                (unsigned long long)gpu_ll_source.last_cycle,(unsigned long long)gpu_ll_source.next_cycle,
                state.budget,state.phase,state.count,state.command,state.count?state.queue[0]:0);
            exit(2);
        }
    }
    if(gpu_upload_source.remaining) {
        /* The source synchronizes elapsed DMA work before changing any DMA
         * register. In particular, completion at this timestamp must see the
         * previous DICR enables. Reads do not perform this partial service. */
        advance_source_gpu();
        if(gpu_upload_source.remaining && psx_cycle_count>gpu_upload_source.last_cycle) {
            gpu_upload_source.budget+=(int32_t)(psx_cycle_count-gpu_upload_source.last_cycle);
            gpu_upload_source.last_cycle=psx_cycle_count;
            source_gpu_upload_words();
        }
    }
    if(gpu_upload_source.remaining && addr==0x1f8010f0u && ((dpcr^val)&mask&0xf00u)) {
        fprintf(stderr,"[dma-model] active source GPU channel control change unsupported\n");exit(2);
    }
    /* DPCR */
    if (addr == 0x1F8010F0u) {
        dpcr = (dpcr & ~mask) | (val & mask);
        return;
    }
    /* DICR: selected low/control bits are writable; bits 24-30 are write-1-to-acknowledge. */
    if (addr == 0x1F8010F4u) {
        /* Bits 0-5: unknown/unused but writable */
        /* Bits 6-14: unused/read-only */
        /* Bit 15: bus-error flag */
        /* Bits 16-22: per-channel IRQ enable */
        /* Bit 23: master IRQ enable */
        /* Bits 24-30: IRQ flags, write 1 to clear */
        /* Bit 31: master flag, read-only (computed) */
        uint32_t dicr_before = dicr;
        uint32_t i_stat_before = i_stat;
        uint32_t write_mask = DICR_WRITE_MASK & mask;
        uint32_t reset_mask = DICR_RESET_MASK & mask;
        dicr = (dicr & ~write_mask) | (val & write_mask);
        dicr &= ~(val & reset_mask);
        raise_dma_irq_on_master_edge(dicr_before);
        trace_dma_reg_write(addr, val, mask, dicr_before, i_stat_before);
        return;
    }

    if(gpu_upload_source.remaining && addr>=0x1f8010a0u && addr<=0x1f8010abu) {
        fprintf(stderr,"[dma-model] source GPU upload register replacement unsupported\n");exit(2);
    }
    /* Per-channel registers */
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        uint32_t offset = addr - 0x1F801080u;
        int ch = offset / 0x10;
        int reg = offset % 0x10;

        if (ch > 6) goto bad;
        switch (reg) {
            case 0x00:
                channels[ch].madr = (channels[ch].madr & ~mask) | (val & mask);
                if(ch<2 && mdec_source_active())channels[ch].madr&=0xffffffu;
                return;
            case 0x04:
                channels[ch].bcr = (channels[ch].bcr & ~mask) | (val & mask);
                return;
            case 0x08:
                channels[ch].chcr = (channels[ch].chcr & ~mask) | (val & mask);
                if ((mask & (1u << 24)) && !((channels[ch].chcr >> 24) & 1u)) {
                    cancel_async_transfer(ch);
                }
                /* Writing CHCR's start bit set triggers transfer. */
                if ((mask & (1u << 24)) && ((channels[ch].chcr >> 24) & 1)) {
                    try_execute(ch);
                }
                return;
            case 0x0C:
                return;
            default:
                goto bad;
        }
    }

bad:
    /* See the read-side note: open-bus, Beetle parity. */
    {
        extern uint64_t g_io_openbus_writes;
        g_io_openbus_writes++;
    }
}

void dma_write(uint32_t addr, uint32_t val) {
    dma_write_masked(addr, val, 0xFFFFFFFFu);
}

uint64_t dma_debug_get_trace(const DMATraceEntry** out_entries) {
    if (out_entries) *out_entries = dma_trace;
    return dma_trace_seq;
}

void dma_debug_clear_trace(void) {
    memset(dma_trace, 0, sizeof(dma_trace));
    dma_trace_seq = 0;
}

uint64_t dma_debug_get_cdrom_history(const DMACDROMHistoryEntry** out_entries) {
    if (out_entries) *out_entries = cdrom_dma_history;
    return cdrom_dma_history_seq;
}

void dma_debug_clear_cdrom_history(void) {
    memset(cdrom_dma_history, 0, sizeof(cdrom_dma_history));
    memset(&cdrom_dma_active_entry, 0, sizeof(cdrom_dma_active_entry));
    cdrom_dma_history_seq = 0;
    cdrom_dma_history_active = 0;
}

void dma_debug_get_state(DMADebugState* out) {
    if (!out) return;
    out->dpcr = dpcr;
    out->dicr = dma_get_dicr();
    for (int i = 0; i < 7; i++) {
        out->channels[i].madr = channels[i].madr;
        out->channels[i].bcr = channels[i].bcr;
        out->channels[i].chcr = channels[i].chcr;
        out->channels[i].active =
            ((i < 2) ? mdec_async[i].active : 0) ||
            ((i == 3) ? cdrom_async.active : 0) ||
            delayed_complete[i].active;
        out->channels[i].remaining_words =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].remaining_words :
            (i == 3 && cdrom_async.active) ? cdrom_async.remaining_words :
            delayed_complete[i].total_words;
        out->channels[i].cycles_accum =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].cycles_accum :
            (i == 3 && cdrom_async.active) ? cdrom_async.cycles_accum :
            delayed_complete[i].cycles_remaining;
    }
}

/* ---- boot snapshot: complete DMA hardware state (LE field wire; see boot_state.h) ---- */
#include "pst_wire.h"

/* DMAChannel = 3×u32 (no pad). Async/delayed structs have host padding — field LE. */
#define DMA_ASYNC_WIRE (1u + 1u + 4u + 4u + 4u + 4u) /* 18 */
#define DMA_DELAY_WIRE (1u + 4u + 4u)                 /* 9 */
#define DMA_SNAP_WIRE_BYTES ( \
    (7u * 12u) + 4u + 4u + (2u * DMA_ASYNC_WIRE) + DMA_ASYNC_WIRE + (7u * DMA_DELAY_WIRE))

static int dma_w_async(PstW *w, const DMAAsyncChannel *a) {
    return pst_w_u8(w, a->active) && pst_w_u8(w, a->debug_started) &&
           pst_w_u32(w, a->total_words) && pst_w_u32(w, a->remaining_words) &&
           pst_w_u32(w, a->cycles_accum) && pst_w_u32(w, a->start_addr);
}
static int dma_r_async(PstR *r, DMAAsyncChannel *a) {
    return pst_r_u8(r, &a->active) && pst_r_u8(r, &a->debug_started) &&
           pst_r_u32(r, &a->total_words) && pst_r_u32(r, &a->remaining_words) &&
           pst_r_u32(r, &a->cycles_accum) && pst_r_u32(r, &a->start_addr);
}
static int dma_w_delay(PstW *w, const DMADelayedComplete *d) {
    return pst_w_u8(w, d->active) && pst_w_u32(w, d->total_words) &&
           pst_w_u32(w, d->cycles_remaining);
}
static int dma_r_delay(PstR *r, DMADelayedComplete *d) {
    return pst_r_u8(r, &d->active) && pst_r_u32(r, &d->total_words) &&
           pst_r_u32(r, &d->cycles_remaining);
}

uint32_t dma_snapshot_bytes(void) { return DMA_SNAP_WIRE_BYTES; }

void dma_snapshot_write(uint8_t *p) {
    if(mdec_source_active()){fprintf(stderr,"[dma-model] source MDEC capture unsupported\n");exit(2);}
    if(spu_source.remaining) {
        fprintf(stderr,"[dma-model] active source SPU request capture unsupported\n");exit(2);
    }
    if(gpu_ll_source.active) {
        fprintf(stderr,"[dma-model] active source linked-list capture unsupported\n");exit(2);
    }
    if(gpu_upload_source.remaining) {
        fprintf(stderr,"[dma-model] active source GPU upload capture unsupported\n");exit(2);
    }
    if(cd_source_model && cdrom_async.active) {
        fprintf(stderr,"[dma-model] state capture during source CD transfer is unsupported\n");exit(2);
    }
    if (otc_source.remaining) {
        fprintf(stderr, "[dma-model] state capture during experimental OTC transfer is unsupported\n");
        exit(2);
    }
    PstW w;
    pst_w_init(&w, p, DMA_SNAP_WIRE_BYTES);
    for (int i = 0; i < 7; i++) {
        pst_w_u32(&w, channels[i].madr);
        pst_w_u32(&w, channels[i].bcr);
        pst_w_u32(&w, channels[i].chcr);
    }
    pst_w_u32(&w, dpcr);
    pst_w_u32(&w, dicr);
    dma_w_async(&w, &mdec_async[0]);
    dma_w_async(&w, &mdec_async[1]);
    dma_w_async(&w, &cdrom_async);
    for (int i = 0; i < 7; i++)
        dma_w_delay(&w, &delayed_complete[i]);
}

/* BS_SEC_DMA_SRC: the four source-DMA state machines (bounded-quad GPU upload,
 * GPU linked list, SPU request, OTC). Measured live at 98.1%/98.4%/57.7% (and
 * OTC in the same family) of frame boundaries, so the queue[32] quiescence
 * precedent does NOT apply -- these must be serialized, not guarded.
 * Every member is a flat scalar: `address`/`start_addr` are GUEST physical
 * addresses (masked to 0xffffff/0x1ffffc), never host pointers.
 * Cycle classification (amendment B): `last_cycle`/`next_cycle` are absolute
 * stamps in psx_cycle_count's base, which BS_SEC_CLOCK restores exactly, so they
 * are written as-is. */
/* Encoded size = the sum of the field widths below (no struct padding on the
 * wire), NOT the sum of sizeof(gpu_upload_source) + sizeof(gpu_ll_source) +
 * sizeof(spu_source) + sizeof(otc_source) = 40+40+48+24 = 152. Declaring 152
 * left the last 12 bytes of the section unwritten, and boot_state.c hands this
 * function an uninitialized buffer, so those bytes were stack garbage in every
 * save. The _Static_asserts below still pin each struct's sizeof. */
#define DMA_SRC_WIRE_BYTES 140u
uint32_t dma_src_wire_bytes(void) { return DMA_SRC_WIRE_BYTES; }
/* Any source-DMA timing model active -> the section is required. */
int dma_src_active(void) {
    return gpu_upload_source_model || gpu_ll_source_model || cd_source_model || otc_source_model;
}
void dma_src_wire_write(uint8_t *out) {
    PstW w; pst_w_init(&w, out, DMA_SRC_WIRE_BYTES);
    pst_w_u32(&w, gpu_upload_source.remaining);
    pst_w_u32(&w, gpu_upload_source.block_size);
    pst_w_u32(&w, gpu_upload_source.in_block);
    pst_w_u32(&w, gpu_upload_source.address);
    pst_w_i32(&w, gpu_upload_source.budget);
    pst_w_u64(&w, gpu_upload_source.last_cycle);
    pst_w_u64(&w, gpu_upload_source.next_cycle);
    pst_w_u32(&w, gpu_ll_source.active);
    pst_w_u32(&w, gpu_ll_source.address);
    pst_w_u32(&w, gpu_ll_source.remaining);
    pst_w_u32(&w, gpu_ll_source.nodes);
    pst_w_i32(&w, gpu_ll_source.budget);
    pst_w_u64(&w, gpu_ll_source.last_cycle);
    pst_w_u64(&w, gpu_ll_source.next_cycle);
    pst_w_u32(&w, spu_source.remaining);
    pst_w_u32(&w, spu_source.block_size);
    pst_w_u32(&w, spu_source.in_block);
    pst_w_u32(&w, spu_source.address);
    pst_w_u32(&w, spu_source.total_words);
    pst_w_u32(&w, spu_source.start_addr);
    pst_w_i32(&w, spu_source.budget);
    pst_w_u64(&w, spu_source.last_cycle);
    pst_w_u64(&w, spu_source.next_cycle);
    pst_w_u32(&w, otc_source.remaining);
    pst_w_u32(&w, otc_source.address);
    pst_w_u64(&w, otc_source.last_cycle);
    pst_w_u64(&w, otc_source.next_cycle);
}
int dma_src_perturb(const char *field) {
    if (!field || strcmp(field,"dma_upload_cycle")) return 0;
    gpu_upload_source.last_cycle += 1u;
    return 1;
}
int dma_src_wire_read(const uint8_t *in, uint32_t len) {
    PstR r;
    if (len != DMA_SRC_WIRE_BYTES) return 0;
    pst_r_init(&r, in, len);
    if (!pst_r_u32(&r, &gpu_upload_source.remaining)) return 0;
    if (!pst_r_u32(&r, &gpu_upload_source.block_size)) return 0;
    if (!pst_r_u32(&r, &gpu_upload_source.in_block)) return 0;
    if (!pst_r_u32(&r, &gpu_upload_source.address)) return 0;
    if (!pst_r_i32(&r, &gpu_upload_source.budget)) return 0;
    if (!pst_r_u64(&r, &gpu_upload_source.last_cycle)) return 0;
    if (!pst_r_u64(&r, &gpu_upload_source.next_cycle)) return 0;
    if (!pst_r_u32(&r, &gpu_ll_source.active)) return 0;
    if (!pst_r_u32(&r, &gpu_ll_source.address)) return 0;
    if (!pst_r_u32(&r, &gpu_ll_source.remaining)) return 0;
    if (!pst_r_u32(&r, &gpu_ll_source.nodes)) return 0;
    if (!pst_r_i32(&r, &gpu_ll_source.budget)) return 0;
    if (!pst_r_u64(&r, &gpu_ll_source.last_cycle)) return 0;
    if (!pst_r_u64(&r, &gpu_ll_source.next_cycle)) return 0;
    if (!pst_r_u32(&r, &spu_source.remaining)) return 0;
    if (!pst_r_u32(&r, &spu_source.block_size)) return 0;
    if (!pst_r_u32(&r, &spu_source.in_block)) return 0;
    if (!pst_r_u32(&r, &spu_source.address)) return 0;
    if (!pst_r_u32(&r, &spu_source.total_words)) return 0;
    if (!pst_r_u32(&r, &spu_source.start_addr)) return 0;
    if (!pst_r_i32(&r, &spu_source.budget)) return 0;
    if (!pst_r_u64(&r, &spu_source.last_cycle)) return 0;
    if (!pst_r_u64(&r, &spu_source.next_cycle)) return 0;
    if (!pst_r_u32(&r, &otc_source.remaining)) return 0;
    if (!pst_r_u32(&r, &otc_source.address)) return 0;
    if (!pst_r_u64(&r, &otc_source.last_cycle)) return 0;
    if (!pst_r_u64(&r, &otc_source.next_cycle)) return 0;
    return 1;
}
/* Layout guards: a future field addition becomes a build break instead of a
 * silent drop (the E5 R10 failure mode). */
_Static_assert(sizeof gpu_upload_source == 40, "gpu_upload_source layout changed; update dma_src_wire_write");
_Static_assert(sizeof gpu_ll_source == 40, "gpu_ll_source layout changed; update dma_src_wire_write");
_Static_assert(sizeof spu_source == 48, "spu_source layout changed; update dma_src_wire_write");
_Static_assert(sizeof otc_source == 24, "otc_source layout changed; update dma_src_wire_write");

/* E survey (M2 apparatus): report whether each source-DMA state machine holds
 * live mid-transfer state at the frame boundary that arms a checkpoint. If a
 * machine is provably idle there, a quiescence precondition can replace both
 * serializing it and the model-based refusal; if it is live, it must be
 * serialized field by field. */
void dma_source_dma_live(int *upload, int *ll, int *spu) {
    if (upload)
        *upload = (gpu_upload_source.remaining != 0u) || (gpu_upload_source.in_block != 0u) ||
                  (gpu_upload_source.address != 0u) || (gpu_upload_source.budget != 0);
    if (ll)
        *ll = (gpu_ll_source.active != 0u) || (gpu_ll_source.remaining != 0u) ||
              (gpu_ll_source.nodes != 0u) || (gpu_ll_source.budget != 0);
    if (spu)
        *spu = (spu_source.remaining != 0u) || (spu_source.in_block != 0u) ||
               (spu_source.budget != 0);
}

int dma_snapshot_read(const uint8_t *p, uint32_t len) {
    /* The three model-based refusals that used to live here (source GPU upload,
     * source CD timing, source OTC) are now PRESENCE-based, exactly as #5 was:
     * boot_state refuses a comparison-profile state that arrives without
     * BS_SEC_DMA_SRC, and refuses BS_SEC_DMA_SRC when no source model is active.
     * Every restore path funnels through boot_state_load_buffer -> apply_section
     * (savestate, rewind, netplay rings, selfcheck, TAS resume), so the leaf
     * guard is redundant; keeping it would refuse the very state we now write. */
    /* MDEC is a separate surface (#2) and keeps its own model-based refusal
     * until that surface is serialized. */
    if (mdec_source_active()) return 0;
    PstR r;
    if (len != DMA_SNAP_WIRE_BYTES) return 0;
    pst_r_init(&r, p, len);
    for (int i = 0; i < 7; i++) {
        if (!pst_r_u32(&r, &channels[i].madr) || !pst_r_u32(&r, &channels[i].bcr) ||
            !pst_r_u32(&r, &channels[i].chcr))
            return 0;
    }
    if (!pst_r_u32(&r, &dpcr) || !pst_r_u32(&r, &dicr)) return 0;
    if (!dma_r_async(&r, &mdec_async[0]) || !dma_r_async(&r, &mdec_async[1]) ||
        !dma_r_async(&r, &cdrom_async))
        return 0;
    for (int i = 0; i < 7; i++)
        if (!dma_r_delay(&r, &delayed_complete[i])) return 0;
    return 1;
}
