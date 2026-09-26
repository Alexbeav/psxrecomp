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
 */

#include "dma.h"
#include "source_gpu_runtime.h"
#include "cdrom.h"
#include "crash_trace.h"
#include "dirty_ram_interp.h"
#include "dma_gpu_ll.h"
#include "gpu.h"
#include "mdec.h"
#include "mod_memory.h"
#include "overlay_capture.h"
#include "psx_cycles.h"
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
static DMAGPULinkedList gpu_linked_list;
static DMAGpuOtStats gpu_ot_stats;
static uint64_t gpu_ot_start_cycle;
static uint32_t gpu_ot_polls_this_walk;
/* Source-profile selectors, parsed in dma_init. */
static int otc_source_model;
static int cd_source_model;
static int gpu_upload_source_model;
static int gpu_ll_source_model;

uint32_t g_dma_cpu_read_wait;  /* upload load wait published to the CPU (psx_cyc.h) */

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
/* SPU DMA (ch4) per-word cost: 48 cycles per word [ORACLE FIXTURE D3] (read and
 * write, block sizes 16/32 and counts 1/4; PSX-SPX gives 4, marked uncertain).
 * Previously ch4
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

static void gpu_ot_record_walk_stats(uint64_t cycles) {
    gpu_ot_stats.nodes_last = gpu_linked_list.nodes_processed;
    gpu_ot_stats.words_last = gpu_linked_list.total_words;
    gpu_ot_stats.cycles_last = cycles;
    if (gpu_ot_stats.nodes_last > gpu_ot_stats.nodes_max)
        gpu_ot_stats.nodes_max = gpu_ot_stats.nodes_last;
    if (gpu_ot_stats.words_last > gpu_ot_stats.words_max)
        gpu_ot_stats.words_max = gpu_ot_stats.words_last;
    if (gpu_ot_stats.cycles_last > gpu_ot_stats.cycles_max)
        gpu_ot_stats.cycles_max = gpu_ot_stats.cycles_last;
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
    /* Source profile: [ORACLE FIXTURE D19] a channel's flag latches at completion
     * iff its enable (bit 16+n) is set then; the master enable (bit 23) is not
     * required (ch0, ch1, ch2, ch6). PSX-SPX "DICR" says the flag is set only
     * when both bit 16+n and bit 23 are set. Bit 31 and the IRQ3 edge follow
     * the master rule below either way (D19 rules 2-3). */
    if ((gpu_upload_source_model || gpu_ll_source_model || cd_source_model ||
         otc_source_model || mdec_source_active())
            ? ((dicr >> (16 + ch)) & 1u) : channel_irq_flag_armed(ch)) {
        dicr |= (1u << (24 + ch));
        raise_dma_irq_on_master_edge(dicr_before);
    }
    trace_dma('C', ch, 0, dicr_before, i_stat_before);
    event_ring_record_aux(EV_DMA_DONE, (uint8_t)ch, channels[ch].chcr);
    event_ring_record_aux(EV_DEQ, (uint8_t)(SRC_DMA0 + ch), channels[ch].chcr);
}

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
    if (ch == 2 && gpu_linked_list.active) {
        channels[2].madr = gpu_linked_list.current_addr;
        uint64_t cycles = psx_cycle_count - gpu_ot_start_cycle;
        gpu_ot_record_walk_stats(cycles);
        DMAGpuOtCancel *c = &gpu_ot_stats.cancel_ring[
            gpu_ot_stats.cancel_ring_count % DMA_GPU_OT_CANCEL_RING];
        c->pc     = g_debug_last_store_pc;
        c->chcr   = channels[2].chcr;
        c->nodes  = gpu_linked_list.nodes_processed;
        c->words  = gpu_linked_list.total_words;
        c->cycles = cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;
        c->polls  = gpu_ot_polls_this_walk;
        gpu_ot_stats.cancel_ring_count++;
        gpu_ot_stats.cancels++;
        gpu_ws_end_linked_list();
        dma_gpu_ll_cancel(&gpu_linked_list);
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

static uint32_t gpu_ll_resolve_address(void *opaque, uint32_t address) {
    (void)opaque;
    return psx_mod_gpu_dma_resolve_address(address);
}

static uint32_t gpu_ll_read_word(void *opaque, uint32_t address) {
    (void)opaque;
    return psx_read_word(address);
}

static void gpu_ll_observe_header(void *opaque, uint32_t addr,
                                  uint32_t header) {
    (void)opaque;
    gpu_ws_validate_linked_list_header(addr, header);
}

static int gpu_ll_begin_node(void *opaque, uint32_t addr, uint32_t num_words) {
    (void)opaque;

    gpu_ws_validate_linked_list_node(addr, num_words);
    gpu_set_gp0_linked_list_node(addr, num_words);
    return 1;
}

static void gpu_ll_emit_word(void *opaque, uint32_t address, uint32_t word) {
    (void)opaque;
    gpu_set_gp0_source(address);
    gpu_write_gp0(word);
}

static void gpu_ll_complete(void *opaque, int hit_limit) {
    (void)opaque;
    gpu_ot_stats.completes++;
    gpu_ot_record_walk_stats(psx_cycle_count - gpu_ot_start_cycle);
    channels[2].madr = hit_limit ? gpu_linked_list.current_addr
                                 : 0x00FFFFFFu;
    gpu_ws_end_linked_list();
    complete_transfer(2);
}

static const DMAGPULinkedListOps gpu_ll_ops = {
    gpu_ll_resolve_address,
    gpu_ll_read_word,
    gpu_ll_observe_header,
    gpu_ll_begin_node,
    gpu_ll_emit_word,
    gpu_ll_complete
};

static void start_async_gpu_linked_list(void) {
    if (gpu_linked_list.active) {
        gpu_ot_stats.starts_dropped++;
        return;
    }
    uint32_t start_addr = psx_mod_gpu_dma_resolve_address(channels[2].madr);
    gpu_ws_begin_linked_list();
    /* This scan only prepares optional widescreen grouping. It does not send
     * packets to GP0. The event-driven walker below performs every guest-visible
     * header and payload read at its consumption boundary. */
    gpu_ws_prepass_linked_list(start_addr);
    dma_gpu_ll_start(&gpu_linked_list, start_addr, 0x40000u);
    event_ring_record_aux(EV_DMA_SCHED, 2u, channels[2].chcr);
    gpu_ot_stats.starts++;
    gpu_ot_start_cycle = psx_cycle_count;
    gpu_ot_polls_this_walk = 0;
}

/* ---- Source-profile DMA machines (TAS/source mode) ----
 *
 * Selected by PSX_GPU_DMA_MODEL (GPU upload, GPU linked list, SPU),
 * PSX_CD_DMA_MODEL (CD-ROM) and PSX_INPUT_ROUTE_DMA_MODEL (OTC), each with
 * PSX_INPUT_ROUTE_FILE. The default paths above and below are unchanged.
 *
 * Register semantics follow PSX-SPX "DMA Channels" (SyncMode 0/1/2, MADR/BCR
 * update rules, the linked-list header and end code, OTC fill order). The
 * timing below is fitted to authored oracle fixtures; each constant cites its
 * rows. Receipts are under evidence/T172/clean-rewrite-fixtures-20260926.
 *
 * One machine per channel. A machine owns a cycle credit. The kick grants
 * DSM_KICK_CREDIT; after that, credit is granted for elapsed guest cycles.
 * Work items (words, block overheads, node headers) are taken while the
 * credit is positive, so the last item may overdraw it.
 *   - OTC, GPU upload, CD and SPU are credited only at the global 128-cycle
 *     service edges ([ORACLE FIXTURE D9a]: the OTC halt steps linearly with the
 *     kick phase over one 128-cycle span; D9e reproduces all 12 halting CD rows
 *     with the same edge phase; D9b/D3 completions quantise the same way).
 *   - The linked list is credited continuously and banks no positive credit
 *     while the GPU is not ready ([NOT OBSERVED]: D9d timing is gated by the
 *     GPU FIFO model; the PS1B-182 route replays are its acceptance).
 *   - MDEC in/out (ch0/ch1) follow the same edges. A block starts only while the
 *     MDEC requests it (input FIFO empty / output FIFO full) and moves at once
 *     ([ORACLE FIXTURE D11a] stall counts).
 *   - The source MDEC is clocked at every service edge, whether or not a DMA
 *     runs, and at an MDEC DMA kick ([ORACLE FIXTURE D15b]: 8/8 status
 *     timelines exact; D17a: the same model on the kick-phase sweep, where a
 *     per-cycle clock fails; D17b: with no access at all the decoder still
 *     advances). [NOT FITTED: D18a] scoring MADR1 step times (+-10 cycles)
 *     with one global edge offset, 59 of the 128 DMA0 kick phases miss. A
 *     kick-anchored 32-cycle visibility rule would leave 24, but it may be the
 *     sampling loop's own period (D21 checks). The remaining misses are one
 *     block one service edge early (47 steps at about -123 cycles); no slip
 *     exceeds one edge. */

int source_gpu_runtime_active(void);
static void try_execute(int ch);

#define DSM_QUANTUM          128u /* [ORACLE FIXTURE D9a, D9e] service edge period  */
#define DSM_KICK_CREDIT       64  /* [ORACLE FIXTURE D9a] OTC n<=64 completes at the
                                   * kick; [D10] no upload halt iff BA*(BS+7)<=64     */
#define DSM_OTC_WORD           1  /* [DOC] "DMA Transfer Rates"; D9a exact            */
#define DSM_GPU_WORD           1  /* [DOC]; D10 halt = BS+5 for one block             */
#define DSM_GPU_BLOCK          7  /* [ORACLE FIXTURE D10, D10b] no halt iff
                                   * BA*(BS+7) <= 64: 1x57/1x58, 2x25/2x26, 3x14/3x15,
                                   * 4x9/4x10 and 8x1/8x2 split exactly there (all 33
                                   * D10b shapes; BA*BS <= 57 misses 17)              */
#define DSM_CD_WORD            9  /* [ORACLE FIXTURE D9e] 128..512-word slope 8.99 and
                                   * 9.03 at both 1F801018 settings. PSX-SPX gives
                                   * 24 (BIOS) or 40 (games); the default path keeps
                                   * its own cost.                                     */
#define DSM_SPU_WORD          48  /* [ORACLE FIXTURE D3] read and write, all sizes    */
#define DSM_LL_NODE           15  /* [NOT OBSERVED] per-node header cost              */
#define DSM_UPLOAD_WAIT_CAP  201u /* [ORACLE FIXTURE D10] load wait = min(BS,201)-1   */
#define DSM_MDEC_WORD          1  /* [DOC] MDEC in/out 1; D11 timelines do not depend
                                   * on it between 1 and 4                             */
#define DSM_MDEC_FEED_STEP   128u /* the MDEC credit cap: feeding in steps no longer
                                   * than this equals a per-cycle clock               */

enum { DSM_OTC, DSM_GPU, DSM_LL, DSM_CD, DSM_SPU, DSM_MDEC_IN, DSM_MDEC_OUT, DSM_COUNT };
static const uint8_t dsm_channel[DSM_COUNT] = { 6, 2, 2, 3, 4, 0, 1 };
static const char *const dsm_name[DSM_COUNT] = { "otc", "upload", "ll", "cd", "spu",
                                                 "mdec_in", "mdec_out" };

typedef struct {
    uint8_t  running;      /* the machine owns this channel's transfer            */
    uint8_t  halts_cpu;    /* CPU held until the machine finishes                 */
    uint8_t  stage;        /* upload: block overhead paid; LL: inside a node     */
    uint8_t  held;         /* LL: stopped on a GPU that is not ready              */
    uint32_t cursor;       /* next RAM word address                               */
    uint32_t words_left;   /* words still to move (LL: in the current node)       */
    uint32_t blk_words;    /* SyncMode 1 block length                             */
    uint32_t blk_pos;      /* words moved in the current block                    */
    uint32_t node_count;   /* LL nodes started                                    */
    uint32_t link;         /* LL: next-node pointer of the current header         */
    int32_t  credit;       /* cycles available to spend                           */
    uint64_t served_until; /* guest cycle credited so far                         */
} DmaSrcMachine;

static DmaSrcMachine dsm[DSM_COUNT];
static int dsm_spu_model;      /* SPU follows the GPU source profile (spu.c uses the same switch) */
static uint32_t dsm_wait_live; /* upload load wait at this instant */
static uint64_t dsm_mdec_clock; /* guest cycle up to which the source MDEC has been clocked */

static void dsm_fail(const char *what) {
    fprintf(stderr, "[dma-model] %s\n", what);
    exit(2);
}

static int dsm_profile_any(void) {
    return gpu_upload_source_model || gpu_ll_source_model || cd_source_model ||
           otc_source_model || mdec_source_active();
}

/* Clock the source MDEC up to `now` (mdec.h: its service is owned by source
 * DMA). Callers pass a service edge or a kick time. Steps of at most the MDEC
 * credit cap equal one feed per edge; after many steps the decoder can only be
 * waiting, so the rest is one step. */
static void dsm_mdec_feed(uint64_t now) {
    if (!mdec_source_active()) { dsm_mdec_clock = now; return; }
    for (unsigned n = 0; dsm_mdec_clock < now; n++) {
        uint64_t step = now - dsm_mdec_clock;
        if (step > DSM_MDEC_FEED_STEP && n < 4096u) step = DSM_MDEC_FEED_STEP;
        if (step > 0x7FFFFFFFu) step = 0x7FFFFFFFu;
        mdec_source_advance((uint32_t)step);
        dsm_mdec_clock += step;
    }
}

static int dsm_live_any(void) {
    for (int k = 0; k < DSM_COUNT; k++) if (dsm[k].running) return 1;
    return 0;
}

/* The source CPU owner (source_gpu_runtime) halts the CPU and latches the load
 * wait at instruction boundaries. Without it, a halting kick advances the
 * guest clock itself and the wait is published at once. */
static int dsm_cpu_owner(void) {
    return source_gpu_runtime_active();
}

static void dsm_publish_wait(void) {
    dsm_wait_live = dsm[DSM_GPU].running
        ? (dsm[DSM_GPU].blk_words < DSM_UPLOAD_WAIT_CAP ? dsm[DSM_GPU].blk_words
                                                        : DSM_UPLOAD_WAIT_CAP) - 1u
        : 0u;
    if (!dsm_cpu_owner()) g_dma_cpu_read_wait = dsm_wait_live;
}

static void dsm_credit_add(DmaSrcMachine *m, uint64_t cycles) {
    int64_t c = (int64_t)m->credit + (int64_t)(cycles > 0x40000000u ? 0x40000000u : cycles);
    m->credit = c > 0x40000000 ? 0x40000000 : (int32_t)c;
}

/* 1 while a guest write to a DMA register services the machines, 2 during an
 * explicit service point. */
static int dsm_write_service;

/* Credit whole service edges passed since the last grant. A guest write to any
 * DMA register instead grants the exact elapsed cycles ([ORACLE FIXTURE D12]:
 * after a DICR/DPCR/idle-MADR write the upload's MADR and BCR read ahead of the
 * no-access control by the elapsed credit, while completion stays on the
 * service edge; a DICR read leaves them unchanged). */
static void dsm_credit_edges(DmaSrcMachine *m, uint64_t now) {
    /* MDEC in/out take credit only at edges and kicks even on a register write
     * ([ORACLE FIXTURE D18d]: 512/512 timelines unchanged by a write). */
    int exact = dsm_write_service == 2 ||
                (dsm_write_service && m != &dsm[DSM_MDEC_IN] && m != &dsm[DSM_MDEC_OUT]);
    uint64_t edge = exact ? now : now - now % DSM_QUANTUM;
    if (edge <= m->served_until) return;
    dsm_credit_add(m, edge - m->served_until);
    m->served_until = edge;
}

static void dsm_begin(int k, uint32_t cursor, uint32_t words) {
    DmaSrcMachine *m = &dsm[k];
    memset(m, 0, sizeof *m);
    m->running = 1;
    m->cursor = cursor & 0x1FFFFCu;
    m->words_left = words;
    m->credit = DSM_KICK_CREDIT;
    m->served_until = psx_cycle_count;
}

static void dsm_end(int k) {
    dsm[k].running = 0;
    dsm[k].halts_cpu = 0;
    dsm[k].held = 0;
    dsm[k].credit = 0;
    if (k == DSM_GPU) dsm_publish_wait();
    complete_transfer(dsm_channel[k]);
}

static int32_t dsm_step(int k) {
    int32_t step = ((channels[dsm_channel[k]].chcr >> 1) & 1u) ? -4 : 4;
    return step;
}

/* OTC (ch6): fills backwards; the last entry written is the end code. MADR and
 * BCR are not updated (SyncMode 0) [DOC]; D9a end MADR = start. */
static void dsm_run_otc(void) {
    DmaSrcMachine *m = &dsm[DSM_OTC];
    while (m->words_left && m->credit > 0) {
        uint32_t v = m->words_left == 1u ? 0x00FFFFFFu : ((m->cursor - 4u) & 0x00FFFFFFu);
        psx_write_word(m->cursor, v);
        m->cursor = (m->cursor - 4u) & 0x1FFFFCu;
        m->words_left--;
        m->credit -= DSM_OTC_WORD;
    }
    if (!m->words_left) dsm_end(DSM_OTC);
}

/* GPU VRAM upload (ch2 SyncMode 1, RAM to GPU). Each block pays its overhead
 * before its first word. BA counts blocks not yet started; MADR holds the
 * current block's start and the end address at the finish [DOC]. */
static void dsm_run_upload(void) {
    DmaSrcMachine *m = &dsm[DSM_GPU];
    int32_t step = dsm_step(DSM_GPU);
    while (m->words_left && m->credit > 0) {
        if (!m->stage) {
            uint32_t ba = channels[2].bcr >> 16;
            channels[2].bcr = (channels[2].bcr & 0xFFFFu) | (((ba - 1u) & 0xFFFFu) << 16);
            m->credit -= DSM_GPU_BLOCK;
            m->stage = 1;
            continue;
        }
        uint32_t word = psx_read_word(m->cursor);
        gpu_set_gp0_source(m->cursor);
        gpu_write_gp0(word);
        m->cursor = (m->cursor + (uint32_t)step) & 0x1FFFFCu;
        m->words_left--;
        m->credit -= DSM_GPU_WORD;
        if (++m->blk_pos == m->blk_words) {
            m->blk_pos = 0;
            m->stage = 0;
            channels[2].madr = m->cursor;
        }
    }
    if (!m->words_left) {
        channels[2].madr = m->cursor;
        dsm_end(DSM_GPU);
    }
}

/* Profile scope: without the source GPU projection, the bounded linked-list
 * profile only carries single-word commands whose cost the GPU side does not
 * need to model (NOP, cache clear, the E1h-E6h environment commands). */
static void dsm_ll_check_word(uint32_t word) {
    if (source_gpu_runtime_active()) return;
    uint32_t op = word >> 24;
    if (op == 0x00u || op == 0x01u || (op >= 0xE1u && op <= 0xE6u)) return;
    dsm_fail("source GPU linked list: command outside the bounded profile");
}

/* GPU linked list (ch2 SyncMode 2): header = next pointer (bits 0-23) + word
 * count (bits 24-31); bit 23 of the pointer ends the list; MADR holds the
 * current node and the end code at the finish [DOC]. Every header and payload
 * word is read from RAM when it is consumed. */
static void dsm_run_ll(uint64_t now) {
    DmaSrcMachine *m = &dsm[DSM_LL];
    for (;;) {
        int ready = gpu_dma_source_ll_ready();
        if (ready < 0) dsm_fail("source GPU linked list: GPU state outside the profile");
        if (now > m->served_until) {
            if (ready) dsm_credit_add(m, now - m->served_until);
            m->served_until = now;
        }
        if (!ready) {
            if (m->credit > 0) m->credit = 0;
            m->held = 1;
            return;
        }
        m->held = 0;
        if (m->credit <= 0) return;
        if (!m->stage) {
            uint32_t header = psx_read_word(m->cursor);
            m->words_left = header >> 24;
            m->link = header & 0x00FFFFFFu;
            gpu_set_gp0_linked_list_node(m->cursor, m->words_left);
            channels[2].madr = m->cursor;
            m->node_count++;
            m->credit -= DSM_LL_NODE;
            m->stage = 1;
            m->cursor = (m->cursor + 4u) & 0x1FFFFCu;
        } else if (m->words_left) {
            uint32_t word = psx_read_word(m->cursor);
            dsm_ll_check_word(word);
            gpu_set_gp0_source(m->cursor);
            gpu_write_gp0(word);
            m->cursor = (m->cursor + 4u) & 0x1FFFFCu;
            m->words_left--;
            m->credit -= 1;
        }
        if (m->stage && !m->words_left) {
            if (m->link & 0x00800000u) {
                channels[2].madr = m->link;
                dsm_end(DSM_LL);
                return;
            }
            m->stage = 0;
            m->cursor = m->link & 0x1FFFFCu;
        }
    }
}

/* CD-ROM (ch3 SyncMode 0, to RAM). Missing sector data reads as zero. MADR is
 * not updated ([DOC]; [ORACLE FIXTURE D9e] end MADR = start in all 16 rows). */
static void dsm_run_cd(void) {
    DmaSrcMachine *m = &dsm[DSM_CD];
    int32_t step = dsm_step(DSM_CD);
    while (m->words_left && m->credit > 0) {
        uint32_t word = cdrom_dma_read_padded();
        psx_write_word(m->cursor, word);
        record_cdrom_dma_word(word);
        dirty_ram_mark_executable_range(m->cursor, 4);
        m->cursor = (m->cursor + (uint32_t)step) & 0x1FFFFCu;
        m->words_left--;
        cdrom_async.remaining_words = m->words_left;
        m->credit -= DSM_CD_WORD;
    }
    if (!m->words_left) {
        uint32_t start = channels[3].madr;
        m->running = 0;
        m->halts_cpu = 0;
        finish_async_cdrom_transfer(m->cursor);
        channels[3].madr = start;
    }
}

/* SPU (ch4 SyncMode 1). MADR advances per block and BA reaches zero [DOC];
 * D3 end MADR = start + 4 x words. */
static void dsm_run_spu(void) {
    DmaSrcMachine *m = &dsm[DSM_SPU];
    int32_t step = dsm_step(DSM_SPU);
    int to_spu = channels[4].chcr & 1u;
    while (m->words_left && m->credit > 0) {
        if (to_spu) spu_dma_write(psx_read_word(m->cursor));
        else psx_write_word(m->cursor, spu_dma_read());
        m->cursor = (m->cursor + (uint32_t)step) & 0x1FFFFCu;
        m->words_left--;
        m->credit -= DSM_SPU_WORD;
        if (++m->blk_pos == m->blk_words) {
            m->blk_pos = 0;
            channels[4].madr = m->cursor;
            channels[4].bcr = (channels[4].bcr & 0xFFFFu) |
                              ((((channels[4].bcr >> 16) - 1u) & 0xFFFFu) << 16);
        }
    }
    if (!m->words_left) {
        channels[4].madr = m->cursor;
        dsm_end(DSM_SPU);
    }
}

/* MDEC in (ch0, RAM to MDEC) and out (ch1, MDEC to RAM), SyncMode 1. A block
 * starts only while the MDEC requests it and then moves whole within its
 * credit ([ORACLE FIXTURE D11a]: DMA0 stalls after 10/16/16/32 words for BS
 * 1/8/16/32; D11b: DMA1 never finishes a last unit shorter than 32 words).
 * Output words land at the current address plus the MDEC's row offset, which
 * places 15/24 bpp macroblocks in raster order. MADR advances per block and BA
 * counts blocks not yet started [DOC]. */
static void dsm_run_mdec(int k) {
    DmaSrcMachine *m = &dsm[k];
    int ch = dsm_channel[k];
    int32_t step = dsm_step(k);
    while (m->words_left && m->credit > 0) {
        if (!m->stage) {
            if (ch == 0 ? !mdec_dma_write_ready() : !mdec_dma_read_ready()) {
                /* No banked credit while the MDEC is not requesting (the linked-list
                 * rule; D15b/D17a fits are unchanged by it). */
                if (m->credit > 0) m->credit = 0;
                break;
            }
            channels[ch].bcr = (channels[ch].bcr & 0xFFFFu) |
                               ((((channels[ch].bcr >> 16) - 1u) & 0xFFFFu) << 16);
            m->stage = 1;
        }
        if (ch == 0) {
            mdec_dma_write_word(psx_read_word(m->cursor));
        } else {
            uint32_t offset;
            uint32_t word = mdec_source_dma_read(&offset);
            uint32_t addr = (m->cursor + 4u * offset) & 0x1FFFFCu;
            g_dma_cur_madr = addr;
            psx_write_word(addr, word);
        }
        m->cursor = (m->cursor + (uint32_t)step) & 0x1FFFFCu;
        m->words_left--;
        m->credit -= DSM_MDEC_WORD;
        if (++m->blk_pos == m->blk_words) {
            m->blk_pos = 0;
            m->stage = 0;
            channels[ch].madr = m->cursor;
        }
    }
    if (!m->words_left) {
        channels[ch].madr = m->cursor;
        dsm_end(k);
    }
}

static void dsm_service(int k, uint64_t now) {
    DmaSrcMachine *m = &dsm[k];
    if (!m->running) return;
    if (!((channels[dsm_channel[k]].chcr >> 24) & 1u) || !channel_enabled(dsm_channel[k]))
        return;
    g_dma_cur_ch = dsm_channel[k];
    g_dma_initiator_pc = s_dma_ch_initiator_pc[dsm_channel[k]];
    switch (k) {
        case DSM_OTC: dsm_credit_edges(m, now); dsm_run_otc(); break;
        case DSM_GPU: dsm_credit_edges(m, now); dsm_run_upload(); break;
        case DSM_LL:  dsm_run_ll(now); break;
        case DSM_CD:  dsm_credit_edges(m, now); dsm_run_cd(); break;
        case DSM_SPU: dsm_credit_edges(m, now); dsm_run_spu(); break;
        case DSM_MDEC_IN:
        case DSM_MDEC_OUT: dsm_credit_edges(m, now); dsm_run_mdec(k); break;
    }
    g_dma_cur_ch = -1;
}

/* Service order is the dma_advance order: the MDEC clock first, then the
 * channels. [ORACLE FIXTURE D7]: OTC runs before the GPU payload when both
 * start together; D11c: MDEC in is served before MDEC out in both kick orders. */
static void dsm_service_all(uint64_t now) {
    /* [ORACLE FIXTURE D18d] register writes do not clock the decoder: a
     * DMA5-MADR write, a timer-1 write and a no-op MDEC control write leave all
     * 512 timelines byte-identical. Only kicks, edges and explicit service do. */
    dsm_mdec_feed(now - now % DSM_QUANTUM);
    dsm_service(DSM_OTC, now);
    dsm_service(DSM_CD, now);
    dsm_service(DSM_GPU, now);
    dsm_service(DSM_LL, now);
    dsm_service(DSM_MDEC_IN, now);
    dsm_service(DSM_SPU, now);
    dsm_service(DSM_MDEC_OUT, now);
}

/* A halting kick without the source CPU owner: advance the guest clock to
 * each service edge until the machine finishes. */
static void dsm_hold_here(int k) {
    while (dsm[k].running) {
        uint64_t now = psx_cycle_count;
        uint64_t edge = now - now % DSM_QUANTUM + DSM_QUANTUM;
        psx_advance_cycles((uint32_t)(edge - now));
        dsm_service(k, psx_cycle_count);
    }
}

static void dsm_start_otc(void) {
    uint32_t n = channels[6].bcr & 0xFFFFu;
    dsm_begin(DSM_OTC, channels[6].madr, n ? n : 0x10000u);
    dsm[DSM_OTC].halts_cpu = 1; /* burst: the CPU waits ([DOC] "CPU Operation during DMA"; D9a) */
    dsm_run_otc();
}

static void dsm_start_upload(void) {
    uint32_t chcr = channels[2].chcr;
    if (!(chcr & 1u) || ((chcr >> 9) & 3u) != 1u)
        dsm_fail("source GPU upload: only SyncMode 1 from RAM is in the profile");
    if (chcr & (1u << 8)) dsm_fail("source GPU upload: chopping is outside the profile");
    uint32_t bs = channels[2].bcr & 0xFFFFu, ba = channels[2].bcr >> 16;
    if (!bs) bs = 0x10000u;
    if (!ba) ba = 0x10000u;
    uint32_t words = bs * ba;
    if (gpu_dma_vram_upload_words() < words)
        dsm_fail("source GPU upload: the GPU is not expecting this many VRAM words");
    dsm_begin(DSM_GPU, channels[2].madr, words);
    dsm[DSM_GPU].blk_words = bs;
    dsm_run_upload();
    dsm_publish_wait();
}

static void dsm_start_ll(void) {
    if (!(channels[2].chcr & 1u)) dsm_fail("source GPU linked list: direction to RAM");
    if ((channels[2].madr & 0x00FFFFFFu) >= 0x200000u)
        dsm_fail("source GPU linked list: start address outside main RAM");
    dsm_begin(DSM_LL, channels[2].madr, 0);
    dsm_run_ll(psx_cycle_count);
}

static void dsm_start_cd(void) {
    uint32_t chcr = channels[3].chcr;
    dsm_begin(DSM_CD, channels[3].madr, cdrom_async.total_words);
    /* Manual, non-chopped: the CPU waits ([ORACLE FIXTURE D9e]: halt = done - ~20). */
    dsm[DSM_CD].halts_cpu = !(chcr & (1u << 8));
    if (!cdrom_async.total_words) return;
    dsm_run_cd();
}

/* Kick paths that halt the CPU: hold here unless the source CPU owner does. */
static void dsm_kick_hold(int k) {
    if (dsm[k].running && dsm[k].halts_cpu && !dsm_cpu_owner()) dsm_hold_here(k);
}

static void dsm_start_spu(void) {
    uint32_t bs = channels[4].bcr & 0xFFFFu, ba = channels[4].bcr >> 16;
    uint32_t sync = (channels[4].chcr >> 9) & 3u;
    if (sync == 2u) dsm_fail("source SPU DMA: linked-list mode");
    if (!bs) bs = 0x10000u;
    if (sync == 0u) ba = 1u;
    else if (!ba) ba = 0x10000u;
    dsm_begin(DSM_SPU, channels[4].madr, bs * ba);
    dsm[DSM_SPU].blk_words = sync == 1u ? bs : 0u;
    audio_trace_event((channels[4].chcr & 1u) ? AUDIO_EV_DMA_WRITE : AUDIO_EV_DMA_READ,
                      bs * ba, channels[4].madr & 0x1FFFFCu);
    dsm_run_spu();
}

static void dsm_start_mdec(int ch) {
    uint32_t chcr = channels[ch].chcr;
    if ((chcr & 1u) != (ch == 0 ? 1u : 0u))
        dsm_fail("source MDEC DMA: direction outside the profile");
    if (((chcr >> 9) & 3u) != 1u) dsm_fail("source MDEC DMA: only SyncMode 1 is in the profile");
    /* Scope guard: no fixture has measured a decrementing MDEC transfer. */
    if (chcr & 2u) dsm_fail("source MDEC DMA: decrementing address step is not qualified");
    uint32_t bs = channels[ch].bcr & 0xFFFFu, ba = channels[ch].bcr >> 16;
    if (!bs) bs = 0x10000u;
    if (!ba) ba = 0x10000u;
    int k = ch == 0 ? DSM_MDEC_IN : DSM_MDEC_OUT;
    dsm_mdec_feed(psx_cycle_count);
    dsm_begin(k, channels[ch].madr, bs * ba);
    dsm[k].blk_words = bs;
    dsm_run_mdec(k);
}

/* Cycles until the next source machine action (word movement or completion). */
static uint32_t dsm_cycles_to_event(int armed_only) {
    uint32_t best = UINT32_MAX;
    uint64_t now = psx_cycle_count;
    /* The source MDEC advances at every service edge (D17b). */
    if (!armed_only && mdec_source_active()) best = (uint32_t)(DSM_QUANTUM - now % DSM_QUANTUM);
    for (int k = 0; k < DSM_COUNT; k++) {
        const DmaSrcMachine *m = &dsm[k];
        int ch = dsm_channel[k];
        if (!m->running || !((channels[ch].chcr >> 24) & 1u) || !channel_enabled(ch)) continue;
        if (armed_only && !channel_irq_flag_armed(ch)) continue;
        uint32_t d;
        if (k == DSM_LL) {
            if (m->held) continue; /* the GPU side schedules its own readiness */
            d = m->credit > 0 ? 1u : (uint32_t)(1 - m->credit);
        } else {
            d = (uint32_t)(DSM_QUANTUM - now % DSM_QUANTUM);
        }
        if (d < best) best = d;
    }
    return best;
}

/* Register writes in the source profile: finish the work due before the write,
 * so a completion sees the old DICR, then refuse changes the machines cannot
 * follow. Returns 1 when the write was handled here. */
static int dsm_before_write(uint32_t addr, uint32_t *valp, uint32_t mask) {
    if (!dsm_profile_any() && !dsm_spu_model) return 0;
    uint32_t val = *valp;
    /* [ORACLE FIXTURE D14] readback after writing FFFFFFFF:
     *  - DICR 80FF803F. PSX-SPX "DICR" lists bits 0-6 as R/W (7Fh); on the
     *    oracle bit 6 reads 0.
     *  - CHCR 71770703 on ch0-5, the same bits PSX-SPX "D#_CHCR" defines (it
     *    calls the rest "Unused" without a read value).
     *  - BCR and DPCR keep all bits, as PSX-SPX's tables allow. */
    if (addr == 0x1F8010F4u) *valp = val = val & ~0x40u;
    if (addr >= 0x1F801080u && addr <= 0x1F8010DFu && ((addr - 0x1F801080u) & 0xFu) == 8u)
        *valp = val = val & 0x71770703u;
    dsm_write_service = 1;
    dsm_service_all(psx_cycle_count);
    dsm_write_service = 0;
    if (addr == 0x1F8010F0u) {
        uint32_t next = (dpcr & ~mask) | (val & mask);
        for (int k = 0; k < DSM_COUNT; k++) {
            int ch = dsm_channel[k];
            if (dsm[k].running && ((dpcr ^ next) >> (ch * 4)) & 0xFu)
                dsm_fail("source DMA: DPCR change for a channel with a live transfer");
        }
        uint32_t enabling = 0;
        for (int ch = 0; ch < 7; ch++)
            if (!((dpcr >> (ch * 4 + 3)) & 1u) && ((next >> (ch * 4 + 3)) & 1u) &&
                ((channels[ch].chcr >> 24) & 1u))
                enabling |= 1u << ch;
        dpcr = next;
        /* [ORACLE FIXTURE D7]: OTC starts before the GPU in all three DPCR
         * priority orders. Other pairs keep channel order. */
        if (enabling & (1u << 6)) try_execute(6);
        for (int ch = 0; ch < 6; ch++) if (enabling & (1u << ch)) try_execute(ch);
        return 1;
    }
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        int ch = (int)((addr - 0x1F801080u) / 0x10u);
        for (int k = 0; k < DSM_COUNT; k++)
            if (dsm[k].running && dsm_channel[k] == ch)
                dsm_fail("source DMA: register write to a channel with a live transfer");
        uint32_t reg = (addr - 0x1F801080u) & 0xFu;
        /* [DOC] "D#_MADR": bits 24-31 are not used (always zero). */
        if (ch <= 6 && reg == 0u) {
            channels[ch].madr = ((channels[ch].madr & ~mask) | (val & mask)) & 0x00FFFFFFu;
            return 1;
        }
        /* [DOC] "D#_CHCR": D6_CHCR has only bits 24, 28 and 30 writable; bit 1
         * always reads 1 (-4 step) and the other bits 0. PSX-SPX gives no
         * read-as-zero rule for the other channels' unused CHCR bits, or for
         * BCR; D14 checks those on the oracle. */
        if (ch == 6 && reg == 8u) {
            uint32_t next = (channels[6].chcr & ~mask) | (val & mask);
            channels[6].chcr = (next & 0x51000000u) | 0x2u;
            if ((mask & (1u << 24)) && !((channels[6].chcr >> 24) & 1u)) cancel_async_transfer(6);
            if ((mask & (1u << 24)) && ((channels[6].chcr >> 24) & 1u)) try_execute(6);
            return 1;
        }
    }
    return 0;
}

void dma_source_gpu_service_at(uint64_t cycle) {
    g_dma_exec_depth++;
    /* An explicit service point is a caller-driven advance to `cycle`: it
     * clocks the MDEC and grants DMA credit exactly (SPEC-PS1B-186 ruling on
     * the MDEC contract), MDEC channels included. The scheduler path keeps the
 * 128-cycle edges. */
    dsm_mdec_feed(cycle);
    dsm_write_service = 2;
    dsm_service_all(cycle);
    dsm_write_service = 0;
    g_dma_exec_depth--;
}

/* Load wait while an upload runs, sampled once per load by memory.c. */
uint32_t dma_cpu_read_penalty(void) {
    return g_dma_cpu_read_wait;
}

/* The source CPU owner publishes the wait at each instruction boundary. */
void dma_cpu_read_wait_boundary(void) {
    g_dma_cpu_read_wait = dsm_wait_live;
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
        /* Linked-list mode is started by try_execute() and advanced from the
         * guest cycle clock. It must never be drained synchronously here. */
        return 0;
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

static void execute_ch3_cdrom(void) {
    uint32_t chcr = channels[3].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM */

    if (cd_source_model && ((chcr >> 9) & 3u) != 0u)
        dsm_fail("source CD DMA: only SyncMode 0 is in the profile");
    if (direction != 0) {
        channels[3].chcr &= ~((1u << 24) | (1u << 28));
        return;
    }

    start_async_cdrom_transfer();
    if (cd_source_model && cdrom_async.active) {
        dsm_start_cd();
        dsm_kick_hold(DSM_CD);
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

static void execute_ch6_otc(void) {
    if (otc_source_model) {
        dsm_start_otc();
        dsm_kick_hold(DSM_OTC);
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
    return dsm[DSM_OTC].running && dsm[DSM_OTC].halts_cpu;
}

int dma_cpu_source_halted(void) {
    return dma_cpu_otc_halted() || (dsm[DSM_CD].running && dsm[DSM_CD].halts_cpu);
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
    /* [DOC] PSX-SPX "D#_CHCR": bit 28 forces a start without waiting for DREQ,
     * and OTC has no DREQ. [ORACLE FIXTURE D20b] 01000002h on ch6 never starts
     * (bit 24 held 57k cycles, OT unwritten); 11000002h completes. The other
     * channels start on bit 24 alone (D20/D20b: ch0-3 run with bit 28 clear). */
    if (ch == 6 && !((chcr >> 28) & 1)) return;

    /* Bit 28 at the start. Source profile: [ORACLE FIXTURE D20, D20b] it stays
     * set with bit 24 through the transfer on ch0-4 (every sampled channel) and
     * both clear at completion. Default: PSX-SPX "D#_CHCR" clears it when the
     * transfer begins; the oracle retains it [NOT OBSERVED: release policy]. */
    if (!dsm_profile_any() && !dsm_spu_model) channels[ch].chcr &= ~(1u << 28);
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
            if (mdec_source_active()) dsm_start_mdec(0);
            else start_async_mdec_transfer(0);
            break;
        case 1:
            if (mdec_source_active()) dsm_start_mdec(1);
            else start_async_mdec_transfer(1);
            break;
        case 2:
            if (gpu_ll_source_model && ((channels[2].chcr >> 9) & 3u) == 2u)
                dsm_start_ll();
            else if (gpu_upload_source_model && ((channels[2].chcr >> 9) & 3u) != 2u)
                dsm_start_upload();
            else if ((channels[2].chcr & 1u) != 0u &&
                ((channels[2].chcr >> 9) & 3u) == 2u) {
                start_async_gpu_linked_list();
            } else {
                schedule_delayed_complete(2, execute_ch2_gpu(),
                                          DMA_GPU_CYCLES_PER_WORD);
            }
            break;
        case 3:
            execute_ch3_cdrom();
            break;
        case 4:
            if (dsm_spu_model) dsm_start_spu();
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
uint32_t dma_cycles_to_irq(uint32_t i_mask) {
    if (!(i_mask & (1u << 3))) return UINT32_MAX;
    uint32_t best = dsm_cycles_to_event(0);
    const DMAAsyncChannel *async_ch[3] = { &mdec_async[0], &mdec_async[1], &cdrom_async };
    for (int i = 0; i < 3; i++) {
        const DMAAsyncChannel *a = async_ch[i];
        if (!a->active || a->remaining_words == 0) continue;
        /* floor: rw*per_word - accum >= rw - accum (per_word >= 1). Clamp >=0. */
        uint32_t est = a->remaining_words > a->cycles_accum
                         ? (a->remaining_words - a->cycles_accum) : 0u;
        if (est < best) best = est;
    }
    if (gpu_linked_list.active) {
        uint32_t d = dma_gpu_ll_cycles_to_event(&gpu_linked_list);
        if (d < best) best = d;
    }
    for (int ch = 0; ch < 7; ch++) {
        if (delayed_complete[ch].active && delayed_complete[ch].cycles_remaining < best)
            best = delayed_complete[ch].cycles_remaining;
    }
    return best;
}

uint32_t dma_cycles_to_internal_event(void) {
    uint32_t best = dsm_cycles_to_event(0);

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
     * the source CD profile schedules through its own machine. */
    if (!cd_source_model && cdrom_async.active && cdrom_async.remaining_words > 0 &&
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

    if (gpu_linked_list.active && ((channels[2].chcr >> 24) & 1u) &&
        channel_enabled(2)) {
        uint32_t d = dma_gpu_ll_cycles_to_event(&gpu_linked_list);
        if (d < best) best = d;
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
    uint32_t best = dsm_cycles_to_event(1);
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
    if (channel_irq_flag_armed(2) && gpu_linked_list.active) {
        uint32_t d = dma_gpu_ll_cycles_to_event(&gpu_linked_list);
        if (d < best) best = d;
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
    uint64_t now = psx_cycle_count;
    dsm_mdec_feed(now - now % DSM_QUANTUM);
    dsm_service(DSM_OTC, now);
    dsm_service(DSM_CD, now);
    dsm_service(DSM_GPU, now);
    dsm_service(DSM_LL, now);
    advance_mdec_channel(0, cycles);
    dsm_service(DSM_MDEC_IN, now);
    dsm_service(DSM_SPU, now);
    advance_mdec_channel(1, cycles);
    dsm_service(DSM_MDEC_OUT, now);
    if (gpu_linked_list.active && ((channels[2].chcr >> 24) & 1u) &&
        channel_enabled(2)) {
        g_dma_cur_ch = 2;
        g_dma_cur_madr = gpu_linked_list.current_addr;
        g_dma_cur_bcr = channels[2].bcr;
        g_dma_initiator_pc = s_dma_ch_initiator_pc[2];
        dma_gpu_ll_advance(&gpu_linked_list, cycles, &gpu_ll_ops, NULL);
    }
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
    g_dma_cpu_read_wait=0;
    const char *gpu_model=getenv("PSX_GPU_DMA_MODEL");
    gpu_upload_source_model=gpu_model && *gpu_model;
    gpu_ll_source_model=gpu_model && (!strcmp(gpu_model,"octoshock-2.2.2-bounded-linked-list") || !strcmp(gpu_model,"octoshock-2.2.2-bounded-quad"));
    if(gpu_upload_source_model && ((!gpu_ll_source_model && strcmp(gpu_model,"octoshock-2.2.2-vram-upload")) || !getenv("PSX_INPUT_ROUTE_FILE"))) {
        fprintf(stderr,"[dma-model] invalid source GPU upload model or missing route\n");exit(2);
    }
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
    memset(&gpu_linked_list, 0, sizeof(gpu_linked_list));
    memset(delayed_complete, 0, sizeof(delayed_complete));
    memset(dsm, 0, sizeof dsm);
    dsm_wait_live = 0;
    dsm_mdec_clock = psx_cycle_count;
    dsm_spu_model = gpu_upload_source_model;
    /* [ORACLE FIXTURE D8] every DMA register reads 0 at power-on in the source
     * profile (PSX-SPX documents DPCR = 07654321h). */
    if (dsm_profile_any()) dpcr = 0u;
    else dpcr = 0x07654321u;   /* [DOC] PSX-SPX "DPCR": initial value on reset */
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
            case 0x08:
                if (ch == 2) {
                    gpu_ot_stats.chcr_reads_total++;
                    if (gpu_linked_list.active) {
                        gpu_ot_stats.chcr_reads_in_walk++;
                        gpu_ot_polls_this_walk++;
                    }
                }
                return channels[ch].chcr;
            case 0x0C: return 0;
            default: goto bad;
        }
    }

bad:
    /* Unmapped words inside the DMA register block (0x1F8010F8/0xFC, channel
     * reg offset 0x0C variants): real hardware open-buses them and Tomba2's
     * late-attract wild I/O sweep (BIOS bzero/read over a 0xDF80xxxx pointer)
     * reads straight through here. [ORACLE FIXTURE D8] 1F8010F8/FC read 0 at
     * every width, before and after writes; the access does not fault. */
    {
        extern uint64_t g_io_openbus_reads;
        g_io_openbus_reads++;
    }
    return 0;
}

void dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask) {
    if (dsm_before_write(addr, &val, mask)) return;
    source_gpu_runtime_dma_write();
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

    /* Per-channel registers */
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        uint32_t offset = addr - 0x1F801080u;
        int ch = offset / 0x10;
        int reg = offset % 0x10;

        if (ch > 6) goto bad;
        switch (reg) {
            case 0x00:
                channels[ch].madr = (channels[ch].madr & ~mask) | (val & mask);
                return;
            case 0x04:
                channels[ch].bcr = (channels[ch].bcr & ~mask) | (val & mask);
                return;
            case 0x08:
                /* A linked-list stop takes effect between packets. Keep the
                 * already-started packet on the normal word-event clock before
                 * exposing its next header to BreakDraw/DrawOTag callers. */
                if (ch == 2 && gpu_linked_list.active && channel_enabled(2) &&
                    (mask & (1u << 24)) && !(val & (1u << 24)) &&
                    gpu_linked_list.phase == DMA_GPU_LL_PHASE_PAYLOAD) {
                    uint32_t remaining = gpu_linked_list.word_count -
                                         gpu_linked_list.payload_index;
                    psx_advance_cycles(remaining);
                    psx_devices_service_to_now();
                }
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
    /* See the read-side note: writes are ignored [ORACLE FIXTURE D8]. */
    {
        extern uint64_t g_io_openbus_writes;
        g_io_openbus_writes++;
    }
}

void dma_write(uint32_t addr, uint32_t val) {
    dma_write_masked(addr, val, 0xFFFFFFFFu);
}

void dma_debug_get_gpu_ot_stats(DMAGpuOtStats* out) {
    if (!out) return;
    *out = gpu_ot_stats;
    out->initiator_pc = s_dma_ch_initiator_pc[2];
    out->active = gpu_linked_list.active;
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
            ((i == 2) ? gpu_linked_list.active : 0) ||
            ((i == 3) ? cdrom_async.active : 0) ||
            delayed_complete[i].active;
        out->channels[i].remaining_words =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].remaining_words :
            (i == 2 && gpu_linked_list.active)
                ? gpu_linked_list.word_count - gpu_linked_list.payload_index :
            (i == 3 && cdrom_async.active) ? cdrom_async.remaining_words :
            delayed_complete[i].total_words;
        out->channels[i].cycles_accum =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].cycles_accum :
            (i == 2 && gpu_linked_list.active) ? gpu_linked_list.cycles_remaining :
            (i == 3 && cdrom_async.active) ? cdrom_async.cycles_accum :
            delayed_complete[i].cycles_remaining;
    }
}

/* ---- boot snapshot: complete DMA hardware state (LE field wire; see boot_state.h) ---- */
#include "pst_wire.h"

/* DMAChannel = 3×u32 (no pad). Async/delayed structs have host padding — field LE. */
#define DMA_ASYNC_WIRE (1u + 1u + 4u + 4u + 4u + 4u) /* 18 */
#define DMA_DELAY_WIRE (1u + 4u + 4u)                 /* 9 */
#define DMA_GPU_LL_WIRE (4u + (10u * 4u))             /* 44 */
#define DMA_SNAP_WIRE_BYTES ( \
    (7u * 12u) + 4u + 4u + (2u * DMA_ASYNC_WIRE) + DMA_ASYNC_WIRE + \
    DMA_GPU_LL_WIRE + (7u * DMA_DELAY_WIRE))

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
static int dma_w_gpu_ll(PstW *w, const DMAGPULinkedList *s) {
    return pst_w_u8(w, s->active) && pst_w_u8(w, s->phase) &&
           pst_w_u8(w, s->emit_node) && pst_w_u8(w, s->hit_limit) &&
           pst_w_u32(w, s->start_addr) && pst_w_u32(w, s->current_addr) &&
           pst_w_u32(w, s->next_addr) && pst_w_u32(w, s->word_count) &&
           pst_w_u32(w, s->payload_index) &&
           pst_w_u32(w, s->cycles_remaining) &&
           pst_w_u32(w, s->nodes_processed) && pst_w_u32(w, s->max_nodes) &&
           pst_w_u32(w, s->total_words) && pst_w_u32(w, s->empty_rank);
}
static int dma_r_gpu_ll(PstR *r, DMAGPULinkedList *s) {
    int ok = pst_r_u8(r, &s->active) && pst_r_u8(r, &s->phase) &&
           pst_r_u8(r, &s->emit_node) && pst_r_u8(r, &s->hit_limit) &&
           pst_r_u32(r, &s->start_addr) && pst_r_u32(r, &s->current_addr) &&
           pst_r_u32(r, &s->next_addr) && pst_r_u32(r, &s->word_count) &&
           pst_r_u32(r, &s->payload_index) &&
           pst_r_u32(r, &s->cycles_remaining) &&
           pst_r_u32(r, &s->nodes_processed) && pst_r_u32(r, &s->max_nodes) &&
           pst_r_u32(r, &s->total_words) && pst_r_u32(r, &s->empty_rank);
    return ok && s->payload_index <= s->word_count &&
           s->phase <= DMA_GPU_LL_PHASE_PAYLOAD;
}
static int dma_w_delay(PstW *w, const DMADelayedComplete *d) {
    return pst_w_u8(w, d->active) && pst_w_u32(w, d->total_words) &&
           pst_w_u32(w, d->cycles_remaining);
}
static int dma_r_delay(PstR *r, DMADelayedComplete *d) {
    return pst_r_u8(r, &d->active) && pst_r_u32(r, &d->total_words) &&
           pst_r_u32(r, &d->cycles_remaining);
}

/* The BS_SEC_DMA_SRC section is present exactly when a source DMA profile is on. */
int dma_src_active(void) {
    return dsm_profile_any();
}

unsigned dma_source_transfer_active_mask(void) {
    unsigned mask = 0;
    for (int k = 0; k < DSM_COUNT; k++)
        if (dsm[k].running) mask |= 1u << dsm_channel[k];
    return mask;
}

int dma_source_transfer_active(void) {
    return dma_source_transfer_active_mask() != 0;
}
unsigned dma_channels_busy_mask(void) {
    unsigned m = 0;
    for (int i = 0; i < 7; i++) if (channels[i].chcr & 0x01000000u) m |= 1u << i;
    return m;
}

uint32_t dma_snapshot_bytes(void) {
    return DMA_SNAP_WIRE_BYTES;
}

void dma_snapshot_write(uint8_t *p) {
    PstW w;
    if (!p && dsm_live_any())
        dsm_fail("source DMA: capture during a live source transfer is unsupported");
    if (!p) return;
    pst_w_init(&w, p, dma_snapshot_bytes());
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
    dma_w_gpu_ll(&w, &gpu_linked_list);
    for (int i = 0; i < 7; i++)
        dma_w_delay(&w, &delayed_complete[i]);
}

/* ---- BS_SEC_DMA_SRC: the source machines, field by field ----
 * Per machine, in DSM order (otc, upload, ll, cd, spu, mdec_in, mdec_out): running, halts_cpu,
 * stage, held (u8); cursor, words_left, blk_words, blk_pos, node_count, link,
 * credit (u32); served_until (u64). Then the live and published load waits (u32)
 * and the source MDEC clock (u64). */
#define DSM_WIRE_MACHINE 40u
#define DSM_WIRE_BYTES   (DSM_COUNT * DSM_WIRE_MACHINE + 16u)
/* A new DmaSrcMachine field must be added to the wire as well. */
_Static_assert(sizeof(DmaSrcMachine) == DSM_WIRE_MACHINE,
               "DmaSrcMachine changed: update BS_SEC_DMA_SRC field by field");

uint32_t dma_src_wire_bytes(void) {
    return DSM_WIRE_BYTES;
}

void dma_src_wire_write(uint8_t *out) {
    PstW w;
    pst_w_init(&w, out, DSM_WIRE_BYTES);
    for (int k = 0; k < DSM_COUNT; k++) {
        const DmaSrcMachine *m = &dsm[k];
        pst_w_u8(&w, m->running);     pst_w_u8(&w, m->halts_cpu);
        pst_w_u8(&w, m->stage);       pst_w_u8(&w, m->held);
        pst_w_u32(&w, m->cursor);     pst_w_u32(&w, m->words_left);
        pst_w_u32(&w, m->blk_words);  pst_w_u32(&w, m->blk_pos);
        pst_w_u32(&w, m->node_count); pst_w_u32(&w, m->link);
        pst_w_i32(&w, m->credit);     pst_w_u64(&w, m->served_until);
    }
    pst_w_u32(&w, dsm_wait_live);
    pst_w_u32(&w, g_dma_cpu_read_wait);
    pst_w_u64(&w, dsm_mdec_clock);
}

int dma_src_wire_read(const uint8_t *in, uint32_t len) {
    if (!in || len != DSM_WIRE_BYTES) return 0;
    DmaSrcMachine next[DSM_COUNT];
    uint32_t live, published;
    uint64_t mdec_clock;
    PstR r;
    pst_r_init(&r, in, len);
    for (int k = 0; k < DSM_COUNT; k++) {
        DmaSrcMachine *m = &next[k];
        if (!pst_r_u8(&r, &m->running) || !pst_r_u8(&r, &m->halts_cpu) ||
            !pst_r_u8(&r, &m->stage) || !pst_r_u8(&r, &m->held) ||
            !pst_r_u32(&r, &m->cursor) || !pst_r_u32(&r, &m->words_left) ||
            !pst_r_u32(&r, &m->blk_words) || !pst_r_u32(&r, &m->blk_pos) ||
            !pst_r_u32(&r, &m->node_count) || !pst_r_u32(&r, &m->link) ||
            !pst_r_i32(&r, &m->credit) || !pst_r_u64(&r, &m->served_until))
            return 0;
    }
    if (!pst_r_u32(&r, &live) || !pst_r_u32(&r, &published) || !pst_r_u64(&r, &mdec_clock))
        return 0;
    memcpy(dsm, next, sizeof dsm);
    dsm_wait_live = live;
    g_dma_cpu_read_wait = published;
    dsm_mdec_clock = mdec_clock;
    return 1;
}

/* Flips bit 0 of one named field, "<machine>.<field>", for the section-wire test. */
int dma_src_perturb(const char *field) {
    if (!field) return 0;
    for (int k = 0; k < DSM_COUNT; k++) {
        size_t n = strlen(dsm_name[k]);
        if (strncmp(field, dsm_name[k], n) || field[n] != '.') continue;
        const char *f = field + n + 1;
        DmaSrcMachine *m = &dsm[k];
        if (!strcmp(f, "running"))           m->running ^= 1u;
        else if (!strcmp(f, "halts_cpu"))    m->halts_cpu ^= 1u;
        else if (!strcmp(f, "stage"))        m->stage ^= 1u;
        else if (!strcmp(f, "held"))         m->held ^= 1u;
        else if (!strcmp(f, "cursor"))       m->cursor ^= 1u;
        else if (!strcmp(f, "words_left"))   m->words_left ^= 1u;
        else if (!strcmp(f, "blk_words"))    m->blk_words ^= 1u;
        else if (!strcmp(f, "blk_pos"))      m->blk_pos ^= 1u;
        else if (!strcmp(f, "node_count"))   m->node_count ^= 1u;
        else if (!strcmp(f, "link"))         m->link ^= 1u;
        else if (!strcmp(f, "credit"))       m->credit ^= 1;
        else if (!strcmp(f, "served_until")) m->served_until ^= 1u;
        else return 0;
        return 1;
    }
    if (!strcmp(field, "wait.live"))      { dsm_wait_live ^= 1u; return 1; }
    if (!strcmp(field, "wait.published")) { g_dma_cpu_read_wait ^= 1u; return 1; }
    if (!strcmp(field, "mdec.clock"))     { dsm_mdec_clock ^= 1u; return 1; }
    return 0;
}

void dma_source_dma_live(int *upload, int *ll, int *spu) {
    if (upload) *upload = dsm[DSM_GPU].running;
    if (ll)     *ll = dsm[DSM_LL].running;
    if (spu)    *spu = dsm[DSM_SPU].running;
}
int dma_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    int gpu_ll_was_active = gpu_linked_list.active != 0;
    if (!p || len != dma_snapshot_bytes()) return 0;
    pst_r_init(&r, p, len);
    for (int i = 0; i < 7; i++) {
        if (!pst_r_u32(&r, &channels[i].madr) || !pst_r_u32(&r, &channels[i].bcr) ||
            !pst_r_u32(&r, &channels[i].chcr))
            return 0;
    }
    if (!pst_r_u32(&r, &dpcr) || !pst_r_u32(&r, &dicr)) return 0;
    if (!dma_r_async(&r, &mdec_async[0]) || !dma_r_async(&r, &mdec_async[1]) ||
        !dma_r_async(&r, &cdrom_async) || !dma_r_gpu_ll(&r, &gpu_linked_list))
        return 0;
    for (int i = 0; i < 7; i++)
        if (!dma_r_delay(&r, &delayed_complete[i])) return 0;
    if (gpu_ll_was_active) gpu_ws_end_linked_list();
    if (gpu_linked_list.active) {
        gpu_ws_begin_linked_list();
        gpu_ws_prepass_linked_list(gpu_linked_list.start_addr);
        gpu_ws_restore_linked_list_rank(gpu_linked_list.empty_rank);
    }
    return 1;
}
