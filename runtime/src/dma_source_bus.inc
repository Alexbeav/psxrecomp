/* Accepted authored ac56b624 DSM contract (PS1B-186), beside modern default
 * controllers. Source channels share one owner; MDEC is never advanced twice.
 * Constants/step provenance are retained from the accepted implementation. */
#include "source_gpu_runtime.h"
#include "pst_wire.h"
static int otc_source_model,cd_source_model,gpu_upload_source_model,gpu_ll_source_model;
uint32_t g_dma_cpu_read_wait;
static void try_execute(int ch);
#define DSM_QUANTUM          128u /* [EMU] grid edge period (spec 1.1); D9a, D9e     */
#define DSM_KICK_CREDIT       64  /* [EMU] kick allowance (spec 6.3); D9a, D10       */
#define DSM_OTC_WORD           1  /* [DOC] "DMA Transfer Rates"; D9a exact            */
#define DSM_GPU_BLOCK          7  /* [EMU] SyncMode 1 block start on channel 2 (spec
                                   * 2.3); D10, D10b. Charged in the same step as the
                                   * block's first word, which moves even when the
                                   * charge alone exhausts the allowance: amendment 6
                                   * called the order unobservable, but the Bio return
                                   * 19,576 readback frontier samples it (PS1B-186)   */
#define DSM_CD_WORD            9  /* [EMU] spec 2.3; D9e (PSX-SPX: 24 or 40)          */
#define DSM_SPU_WORD          48  /* [EMU] spec 2.3; D3 (PSX-SPX: 4, uncertain)       */
#define DSM_CHOP_WORD          8  /* [EMU] spec 2.3: a word with CHCR bit 8 on 0-2    */
#define DSM_LL_NODE           10  /* [EMU] empty list node header (spec 2.3); D23     */
#define DSM_LL_NODE_PAYLOAD   15  /* [EMU] header of a node with payload (spec 2.3);
                                   * D23 b agrees                                    */
#define DSM_STALL_CAP        200u /* [EMU] upload read stall BS-1, capped (spec 5.2)  */
#define DSM_STOP_BUDGET     2048  /* [EMU] cycles of work a stop may move (spec 7.5)  */
#define DSM_MDEC_FEED_STEP   128u /* the MDEC credit cap: feeding in steps no longer
                                   * than this equals a per-cycle clock               */

enum { DSM_OTC, DSM_GPU, DSM_LL, DSM_CD, DSM_SPU, DSM_MDEC_IN, DSM_MDEC_OUT, DSM_COUNT };
static const uint8_t dsm_channel[DSM_COUNT] = { 6, 2, 2, 3, 4, 0, 1 };
static const char *const dsm_name[DSM_COUNT] = { "otc", "upload", "ll", "cd", "spu",
                                                 "mdec_in", "mdec_out" };
/* Channel order at a service point (spec 1.3): 0, 1, 2, 3, 4, 6. */
static const uint8_t dsm_order[DSM_COUNT] = { DSM_MDEC_IN, DSM_MDEC_OUT, DSM_GPU, DSM_LL,
                                              DSM_CD, DSM_SPU, DSM_OTC };

typedef struct {
    uint8_t  running;      /* the machine owns this channel's busy transfer       */
    uint8_t  halts_cpu;    /* the transfer holds the CPU now (spec 5.1)           */
    uint8_t  stage;        /* a block (SyncMode 0: the transfer; list: a node) is
                            * in progress                                         */
    uint8_t  held;         /* waiting for the device at a block start (spec 2.4)  */
    uint32_t cursor;       /* next word address, 24 bits (spec 3.1)               */
    uint32_t words_left;   /* words left in the block, node or SyncMode 0 transfer */
    uint32_t blk_words;    /* length of the SyncMode 1 block in progress          */
    uint32_t blk_pos;      /* words moved in that block                           */
    uint32_t node_count;   /* blocks started / list headers fetched               */
    uint32_t link;         /* SyncMode 1: start address of the block in progress  */
    int32_t  credit;       /* allowance; below zero is a debt (spec 2.1)          */
    uint64_t served_until; /* the machine's latest service point                  */
} DmaSrcMachine;

static DmaSrcMachine dsm[DSM_COUNT];
static int dsm_spu_model;      /* SPU follows the GPU source profile (spu.c uses the same switch) */
static uint32_t dsm_wait_live; /* upload read stall at the latest evaluation (spec 5.2) */
static uint64_t dsm_mdec_clock; /* guest cycle the source MDEC has been advanced to */
static int dsm_write_serviced; /* the current register write has had its service point */

static void dsm_fail(const char *what) {
    fprintf(stderr, "[dma-model] %s\n", what);
    exit(2);
}

static int dsm_profile_any(void) {
    return gpu_upload_source_model || gpu_ll_source_model || cd_source_model ||
           otc_source_model || mdec_source_active();
}

/* Advance the source MDEC to `now` (mdec.h: its service is owned by source
 * DMA). Steps of at most the MDEC credit cap equal a per-cycle clock; after
 * many steps the decoder can only be waiting, so the rest is one step. */
static void dsm_mdec_feed(uint64_t now) {
    if (!mdec_source_active()) { if (now > dsm_mdec_clock) dsm_mdec_clock = now; return; }
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

/* The source CPU owner (source_gpu_runtime) halts the CPU and latches the read
 * stall at instruction boundaries. Without it, a halting kick advances the
 * guest clock itself and the stall is published at once. */
static int dsm_cpu_owner(void) {
    return source_gpu_runtime_active();
}

static void dsm_credit_add(DmaSrcMachine *m, uint64_t cycles) {
    int64_t c = (int64_t)m->credit + (int64_t)(cycles > 0x40000000u ? 0x40000000u : cycles);
    m->credit = c > 0x40000000 ? 0x40000000 : (int32_t)c;
}

static void dsm_idle(int k) {
    DmaSrcMachine *m = &dsm[k];
    m->running = 0;
    m->halts_cpu = 0;
    m->stage = 0;
    m->held = 0;
    m->words_left = 0;
    m->credit = 0;
}

/* Completion (spec 4.1): CHCR bits 24 and 28 clear and the flag latches in
 * complete_transfer(). CD keeps its capture bookkeeping; its MADR stays as the
 * transfer left it (spec 3.3, 3.4). */
static void dsm_end(int k) {
    uint32_t cursor = dsm[k].cursor;
    dsm_idle(k);
    if (k == DSM_CD && !(channels[3].chcr & 1u)) {
        uint32_t madr = channels[3].madr;
        finish_async_cdrom_transfer(DMA_RAM_ADDR(cursor));
        channels[3].madr = madr;
        return;
    }
    complete_transfer(dsm_channel[k]);
}

/* [EMU] bus error (spec 3.2): a word or header due at an address with bit 23
 * set stops the channel with no word moved and no channel flag; DICR bit 15
 * sets and the IRQ line is re-evaluated. */
static void dsm_bus_error(int k) {
    int ch = dsm_channel[k];
    uint32_t before = dicr;
    dsm_idle(k);
    if (ch == 3) cancel_async_transfer(3);
    channels[ch].chcr &= ~((1u << 24) | (1u << 28));
    dicr |= 1u << 15;
    raise_dma_irq_on_master_edge(before);
}

/* Device readiness at a block start (spec 2.4). */
static int dsm_ready(int k) {
    switch (dsm_channel[k]) {
        case 0: return mdec_dma_write_ready();
        case 1: return mdec_dma_read_ready();
        case 2: {
            if (!(channels[2].chcr & 1u)) return 1;          /* GPU to RAM: always */
            int r = gpu_dma_source_ll_ready();
            if (k == DSM_LL && r < 0)
                dsm_fail("source GPU linked list: GPU state outside the profile");
            /* An upload into a GPU state the source projection does not describe
             * (a VRAM transfer without the projection) keeps the pre-spec
             * ungated start [NOT OBSERVED]. */
            return r != 0;
        }
        case 6: return (channels[6].chcr >> 28) & 1u;        /* D20b */
        default: return 1;                                    /* CD, SPU */
    }
}

static int32_t dsm_word_cost(int k) {
    uint32_t chcr = channels[dsm_channel[k]].chcr;
    switch (dsm_channel[k]) {
        case 3: return (chcr & 1u) ? 1 : DSM_CD_WORD;        /* spec 9.6: RAM to CD, 1 */
        case 4: return DSM_SPU_WORD;
        case 6: return DSM_OTC_WORD;
        default: return ((chcr >> 8) & 1u) ? DSM_CHOP_WORD : 1;
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

/* One data word between RAM and the channel's device. RAM uses the low 21
 * address bits (spec 3.1). Direction quirks follow spec 9.6. */
static void dsm_device_word(int k, uint32_t addr) {
    uint32_t ra = DMA_RAM_ADDR(addr);
    int to_dev = (int)(channels[dsm_channel[k]].chcr & 1u);
    switch (dsm_channel[k]) {
        case 0:
            if (to_dev) mdec_dma_write_word(psx_read_word(ra));
            else psx_write_word(ra, 0);                       /* spec 9.6 */
            break;
        case 1:
            if (!to_dev) {
                uint32_t offset;
                uint32_t word = mdec_source_dma_read(&offset);
                uint32_t a = DMA_RAM_ADDR(addr + 4u * offset); /* spec 9.1 */
                g_dma_cur_madr = a;
                psx_write_word(a, word);
            } else {
                (void)psx_read_word(ra);                      /* spec 9.6 */
            }
            break;
        case 2:
            if (to_dev) {
                uint32_t word = psx_read_word(ra);
                gpu_set_gp0_source(ra);
                gpu_write_gp0(word);
            } else {
                psx_write_word(ra, gpu_read_gpuread());
            }
            break;
        case 3:
            if (!to_dev) {
                uint32_t word = cdrom_dma_read_padded();
                psx_write_word(ra, word);
                record_cdrom_dma_word(word);
                dirty_ram_mark_executable_range(ra, 4);
            } else {
                (void)psx_read_word(ra);                      /* spec 9.6 */
            }
            break;
        case 4:
            if (to_dev) spu_dma_write(psx_read_word(ra));
            else psx_write_word(ra, spu_dma_read());
            break;
    }
}

/* One word of a block, a SyncMode 0 transfer or a stop. With chopping in
 * SyncMode 0 the word reloads its address and count from MADR and BCR and
 * writes them back after it moves (spec 3.4). Returns 0 on a bus error. */
static int dsm_block_word(int k, int32_t *budget) {
    DmaSrcMachine *m = &dsm[k];
    int ch = dsm_channel[k];
    uint32_t chcr = channels[ch].chcr;
    int chop0 = ((chcr >> 9) & 3u) == 0u && ((chcr >> 8) & 1u);
    if (chop0) {
        uint32_t n = channels[ch].bcr & 0xFFFFu;
        m->cursor = channels[ch].madr & 0x00FFFFFFu;
        if (n != (m->words_left & 0xFFFFu)) m->words_left = n;
    }
    if (m->cursor & 0x00800000u) { dsm_bus_error(k); return 0; }
    dsm_device_word(k, m->cursor);
    m->cursor = (m->cursor + ((chcr & 2u) ? (uint32_t)-4 : 4u)) & 0x00FFFFFFu;
    m->words_left--;
    m->blk_pos++;
    *budget -= dsm_word_cost(k);
    if (ch == 3) cdrom_async.remaining_words = m->words_left;
    if (chop0) {
        channels[ch].madr = m->cursor;
        channels[ch].bcr = (channels[ch].bcr & 0xFFFF0000u) | (m->words_left & 0xFFFFu);
    }
    return 1;
}

/* Channels 0-4 in SyncMode 0 or 1 (spec 3.3-3.5, 9). MADR and BCR are read
 * when a block starts, so writes between blocks steer the next one (spec 7.2,
 * 7.3); CHCR is read each step (spec 7.4). */
static void dsm_run_block(int k) {
    DmaSrcMachine *m = &dsm[k];
    int ch = dsm_channel[k];
    int block_word_due = 0;  /* a ch2 block start owes its first word in the same step */
    for (;;) {
        uint32_t chcr = channels[ch].chcr;
        uint32_t sync = (chcr >> 9) & 3u;
        if (m->credit <= 0 && !block_word_due) return;
        block_word_due = 0;
        if (!m->stage) {
            if (!dsm_ready(k)) { m->held = 1; return; }
            m->held = 0;
            m->stage = 1;
            m->blk_pos = 0;
            m->node_count++;
            if (sync == 1u) {
                /* [EMU] the block count decreases at the block start (spec 3.5). */
                uint32_t bcr = channels[ch].bcr, bs = bcr & 0xFFFFu;
                channels[ch].bcr = (bcr & 0xFFFFu) | ((((bcr >> 16) - 1u) & 0xFFFFu) << 16);
                m->blk_words = m->words_left = bs ? bs : 0x10000u;
                m->cursor = m->link = channels[ch].madr & 0x00FFFFFFu;
                if (ch == 2) {
                    /* [EMU] spec 2.3: the GPU block-start charge and the block's
                     * first word are one step, so the word moves even when the
                     * charge alone would exhaust the allowance. Amendment 6 called
                     * the order unobservable; Bio return 19,576 samples the readback
                     * frontier at exactly such a block start (PS1B-186). */
                    m->credit -= DSM_GPU_BLOCK;
                    block_word_due = 1;
                    continue;
                }
            } else {
                uint32_t n = channels[ch].bcr & 0xFFFFu;
                m->blk_words = m->words_left = n ? n : 0x10000u;
                m->cursor = channels[ch].madr & 0x00FFFFFFu;
            }
            continue;
        }
        if (!dsm_block_word(k, &m->credit)) return;
        if (m->words_left) continue;
        if (sync == 1u) {
            channels[ch].madr = m->cursor;
            m->stage = 0;
            if (channels[ch].bcr >> 16) continue;
        }
        dsm_end(k);
        return;
    }
}

/* GPU linked list (channel 2, SyncMode 2; spec 3.6). Before each header fetch
 * the GPU must be ready and MADR must not have bit 23 set. The fetch sets MADR
 * to the next pointer at once [EMU]; the payload follows without a readiness
 * check. The list ends when MADR reads 00FFFFFFh after a node. */
static void dsm_run_ll(void) {
    DmaSrcMachine *m = &dsm[DSM_LL];
    for (;;) {
        if (m->credit <= 0) return;
        if (!m->stage) {
            if (!dsm_ready(DSM_LL)) { m->held = 1; return; }
            m->held = 0;
            uint32_t at = channels[2].madr & 0x00FFFFFFu;
            if (at & 0x00800000u) { dsm_bus_error(DSM_LL); return; }
            uint32_t header = psx_read_word(at & 0x1FFFFCu);
            uint32_t n = header >> 24;
            channels[2].madr = header & 0x00FFFFFFu;
            gpu_set_gp0_linked_list_node(at & 0x1FFFFCu, n);
            m->cursor = (at + 4u) & 0x00FFFFFFu;
            m->words_left = n;
            m->node_count++;
            m->credit -= n ? DSM_LL_NODE_PAYLOAD : DSM_LL_NODE;
            m->stage = 1;
        } else if (m->words_left) {
            if (m->cursor & 0x00800000u) { dsm_bus_error(DSM_LL); return; }
            uint32_t word = psx_read_word(DMA_RAM_ADDR(m->cursor));
            dsm_ll_check_word(word);
            gpu_set_gp0_source(DMA_RAM_ADDR(m->cursor));
            gpu_write_gp0(word);
            m->cursor = (m->cursor + 4u) & 0x00FFFFFFu;
            m->words_left--;
            m->credit -= 1;
        }
        if (m->stage && !m->words_left) {
            m->stage = 0;
            if (channels[2].madr == 0x00FFFFFFu) { dsm_end(DSM_LL); return; }
        }
    }
}

/* OTC (channel 6): a backward SyncMode 0 burst (spec 3.7). It starts only
 * with CHCR bit 28 set (spec 2.4, D20b). Each entry is (address - 4) masked
 * to 21 bits [EMU]; the last is 00FFFFFFh. MADR and BCR stay unchanged. */
static void dsm_run_otc(void) {
    DmaSrcMachine *m = &dsm[DSM_OTC];
    for (;;) {
        if (m->credit <= 0) return;
        if (!m->stage) {
            if (!dsm_ready(DSM_OTC)) { m->held = 1; return; }
            uint32_t n = channels[6].bcr & 0xFFFFu;
            m->held = 0;
            m->stage = 1;
            m->node_count++;
            m->words_left = m->blk_words = n ? n : 0x10000u;
            m->cursor = channels[6].madr & 0x00FFFFFFu;
            continue;
        }
        if (m->cursor & 0x00800000u) { dsm_bus_error(DSM_OTC); return; }
        uint32_t v = m->words_left == 1u ? 0x00FFFFFFu : ((m->cursor - 4u) & 0x001FFFFFu);
        psx_write_word(DMA_RAM_ADDR(m->cursor), v);
        m->cursor = (m->cursor - 4u) & 0x00FFFFFFu;
        m->words_left--;
        m->credit -= DSM_OTC_WORD;
        if (!m->words_left) { dsm_end(DSM_OTC); return; }
    }
}

static void dsm_take_steps(int k) {
    int ch = dsm_channel[k];
    g_dma_cur_ch = ch;
    g_dma_initiator_pc = s_dma_ch_initiator_pc[ch];
    if (k == DSM_OTC) dsm_run_otc();
    else if (k == DSM_LL) dsm_run_ll();
    else dsm_run_block(k);
    g_dma_cur_ch = -1;
}

/* Serve one machine at a service point: the allowance grows by the cycles
 * since its last one, steps follow, and a positive remainder is dropped
 * (spec 1.3, 2.1, 2.2). The kick resets progress and the allowance (spec 6.3). */
static void dsm_service(int k, uint64_t now) {
    DmaSrcMachine *m = &dsm[k];
    if (now > m->served_until) {
        if (m->running) dsm_credit_add(m, now - m->served_until);
        m->served_until = now;
    }
    if (!m->running) return;
    dsm_take_steps(k);
    if (m->running && m->credit > 0) m->credit = 0;
}

/* Halt and read stall (spec 5), re-evaluated at every service point and every
 * DMA register write. */
static void dsm_eval_halt_stall(void) {
    int halted = 0;
    for (int k = 0; k < DSM_COUNT; k++) {
        DmaSrcMachine *m = &dsm[k];
        uint32_t chcr = channels[dsm_channel[k]].chcr;
        m->halts_cpu = (uint8_t)(m->running && k != DSM_LL && m->stage && m->words_left &&
                                 ((chcr >> 8) & 7u) == 0u);
        halted |= m->halts_cpu;
    }
    uint32_t wait = 0;
    const DmaSrcMachine *u = &dsm[DSM_GPU];
    uint32_t chcr = channels[2].chcr;
    if (u->running && !halted && ((chcr >> 9) & 3u) == 1u && !((chcr >> 8) & 1u) &&
        dsm_ready(DSM_GPU)) {
        uint32_t bs = channels[2].bcr & 0xFFFFu;
        wait = bs ? (bs - 1u < DSM_STALL_CAP ? bs - 1u : DSM_STALL_CAP) : 0u;
    }
    dsm_wait_live = wait;
    if (!dsm_cpu_owner()) g_dma_cpu_read_wait = wait;
}

/* The latest service point: every machine is served at each one. */
static uint64_t dsm_service_clock(void) {
    uint64_t c = 0;
    for (int k = 0; k < DSM_COUNT; k++) if (dsm[k].served_until > c) c = dsm[k].served_until;
    return c;
}

/* One service point at `now` (spec 1.3; the GPU is already up to `now`).
 * [EMU: the oracle advances the MDEC decoder at every DMA register write (spec
 * v4 1.9); D18d-consistent from the next grid edge] the decoder advances at
 * every service point: grid edges, frame returns and every DMA register write,
 * MDEC channel kicks included. In D18 group d the timelines with and without
 * the access differ only until the next grid edge, before D18d's sampling. */
static void dsm_serve_at(uint64_t now) {
    dsm_mdec_feed(now);
    for (int i = 0; i < DSM_COUNT; i++) dsm_service(dsm_order[i], now);
    dsm_eval_halt_stall();
}

/* Serve the grid edges after the latest service point, up to `now`, one by one
 * (spec 1.4). With nothing running and no source MDEC there is no state to
 * move, so the clock jumps to the last edge. */
static void dsm_service_edges(uint64_t now) {
    uint64_t clock = dsm_service_clock();
    uint64_t e = clock - clock % DSM_QUANTUM + DSM_QUANTUM;
    if (e > now) return;
    if (!dsm_live_any() && !mdec_source_active()) {
        uint64_t last = now - now % DSM_QUANTUM;
        for (int k = 0; k < DSM_COUNT; k++) dsm[k].served_until = last;
        if (last > dsm_mdec_clock) dsm_mdec_clock = last;
        dsm_eval_halt_stall();
        return;
    }
    for (; e <= now; e += DSM_QUANTUM) dsm_serve_at(e);
}

/* A service point at `now`: missed edges first (spec 1.5), then `now` itself. */
static void dsm_service_point(uint64_t now) {
    dsm_service_edges(now);
    if (now > dsm_service_clock()) dsm_serve_at(now);
    else dsm_eval_halt_stall();
}

/* A halting kick without the source CPU owner: advance the guest clock edge by
 * edge until no channel holds the CPU (spec 5.1). */
static void dsm_hold_here(void) {
    for (;;) {
        int halted = 0;
        for (int k = 0; k < DSM_COUNT; k++) halted |= dsm[k].halts_cpu;
        if (!halted) return;
        uint64_t now = psx_cycle_count;
        psx_advance_cycles((uint32_t)(DSM_QUANTUM - now % DSM_QUANTUM));
        dsm_service_edges(psx_cycle_count);
    }
}

/* The machine a channel's kick starts in the source profile, or -1. */
static int dsm_kick_machine(int ch) {
    uint32_t sync = (channels[ch].chcr >> 9) & 3u;
    switch (ch) {
        case 0: return mdec_source_active() ? DSM_MDEC_IN : -1;
        case 1: return mdec_source_active() ? DSM_MDEC_OUT : -1;
        case 2: return sync == 2u ? (gpu_ll_source_model ? DSM_LL : -1)
                                  : (gpu_upload_source_model ? DSM_GPU : -1);
        case 3: return cd_source_model ? DSM_CD : -1;
        case 4: return dsm_spu_model ? DSM_SPU : -1;
        case 6: return otc_source_model ? DSM_OTC : -1;
        default: return -1;
    }
}

/* The source machine running on a channel, or -1. */
static int dsm_running_machine(int ch) {
    for (int k = 0; k < DSM_COUNT; k++)
        if (dsm[k].running && dsm_channel[k] == ch) return k;
    return -1;
}

/* A kick (spec 6.3): the write's service point has run and CHCR is stored.
 * Progress resets, the channel runs at once on a 64-cycle allowance, and the
 * halt and read stall are re-evaluated (spec 6.4). */
static void dsm_kick(int k) {
    DmaSrcMachine *m = &dsm[k];
    int ch = dsm_channel[k];
    uint32_t chcr = channels[ch].chcr;
    uint32_t sync = (chcr >> 9) & 3u;
    if (k == DSM_LL && !(chcr & 1u)) dsm_fail("source GPU linked list: direction to RAM");
    if (k != DSM_LL && k != DSM_OTC && sync > 1u)
        dsm_fail("source DMA: SyncMode 2/3 outside channel 2");
    if (k == DSM_GPU && sync == 1u && (chcr & 1u)) {
        uint32_t bs = channels[2].bcr & 0xFFFFu, ba = channels[2].bcr >> 16;
        if ((uint64_t)(bs ? bs : 0x10000u) * (ba ? ba : 0x10000u) > gpu_dma_vram_upload_words())
            dsm_fail("source GPU upload: the GPU is not expecting this many VRAM words");
    }
    if (k == DSM_CD && !(chcr & 1u)) start_async_cdrom_transfer();
    if (k == DSM_SPU) {
        uint32_t bs = channels[4].bcr & 0xFFFFu, ba = sync ? channels[4].bcr >> 16 : 1u;
        audio_trace_event((chcr & 1u) ? AUDIO_EV_DMA_WRITE : AUDIO_EV_DMA_READ,
                          (bs ? bs : 0x10000u) * (ba ? ba : 0x10000u), channels[4].madr & 0x1FFFFCu);
    }
    memset(m, 0, sizeof *m);
    m->running = 1;
    m->credit = DSM_KICK_CREDIT;
    m->served_until = psx_cycle_count;
    dsm_take_steps(k);
    if (m->running && m->credit > 0) m->credit = 0;
    dsm_eval_halt_stall();
}

/* [EMU] a stop (spec 7.5): CHCR bit 24 cleared on a busy channel. After the
 * write's service point, only the rest of the block or node in progress moves,
 * at most DSM_STOP_BUDGET cycles of work, with no readiness check and no CPU
 * time. No new block starts, MADR is not advanced for a stopped block, no flag
 * latches and no IRQ is raised. */
static void dsm_stop(int k) {
    DmaSrcMachine *m = &dsm[k];
    int32_t budget = DSM_STOP_BUDGET;
    if (m->stage && k == DSM_LL) {
        while (m->words_left && budget > 0 && !(m->cursor & 0x00800000u)) {
            uint32_t word = psx_read_word(DMA_RAM_ADDR(m->cursor));
            gpu_set_gp0_source(DMA_RAM_ADDR(m->cursor));
            gpu_write_gp0(word);
            m->cursor = (m->cursor + 4u) & 0x00FFFFFFu;
            m->words_left--;
            budget -= 1;
        }
    } else if (m->stage && k != DSM_OTC) {
        g_dma_cur_ch = dsm_channel[k];
        while (m->words_left && budget > 0 && !(m->cursor & 0x00800000u)) {
            if (!dsm_block_word(k, &budget)) { g_dma_cur_ch = -1; return; }
        }
        g_dma_cur_ch = -1;
    }
    dsm_idle(k);
    if (k == DSM_CD) cancel_async_transfer(3);
}

/* Register writes in the source profile (spec 1.1 b, 7). The service point
 * comes first: with the source GPU runtime its WRITE event brings the GPU up to
 * the write and serves the edges and the write; without it the write serves
 * itself. Then the stored bits follow spec 7.1 and a write to a busy channel
 * follows spec 7.2-7.5; none is refused. Returns 1 when handled here; 0 hands
 * the (masked) value to the default path. */
static int dsm_before_write(uint32_t *addrp, uint32_t *valp, uint32_t mask) {
    if (!dsm_profile_any() && !dsm_spu_model) return 0;
    uint32_t addr = *addrp, val = *valp;
    if (!dsm_write_serviced) {
        dsm_write_serviced = 1;
        if (source_gpu_runtime_active()) source_gpu_runtime_dma_write();
        else dsm_service_point(psx_cycle_count);
    }
    if (addr == 0x1F8010F4u) {
        /* [ORACLE FIXTURE D14] DICR reads back 80FF803Fh after FFFFFFFFh. */
        *valp = val & ~0x40u;
        return 0;
    }
    if (addr < 0x1F801080u || addr > 0x1F8010EFu) return 0;  /* DPCR, 1F8010F8h/FCh */
    int ch = (int)((addr - 0x1F801080u) / 0x10u);
    uint32_t reg = (addr - 0x1F801080u) & 0xFu;
    /* [EMU] offset +Ch is a second CHCR address (spec 7.11). */
    if (reg == 0xCu) { reg = 8u; *addrp = addr = addr - 4u; }
    if (reg == 0u) {
        /* [DOC] "D#_MADR": bits 24-31 are not used (always zero). */
        channels[ch].madr = ((channels[ch].madr & ~mask) | (val & mask)) & 0x00FFFFFFu;
        return 1;
    }
    if (reg != 8u) return 0;                                  /* BCR keeps all bits */
    /* [ORACLE FIXTURE D14] CHCR keeps 71770703h on channels 0-5; [DOC] D6_CHCR
     * keeps bits 24, 28 and 30, and bit 1 reads 1. */
    uint32_t old = channels[ch].chcr;
    uint32_t next = (old & ~mask) | (val & mask);
    next = ch == 6 ? ((next & 0x51000000u) | 0x2u) : (next & 0x71770703u);
    int was_busy = (old >> 24) & 1u, busy = (next >> 24) & 1u;
    int k = dsm_running_machine(ch);
    if (k >= 0) {
        channels[ch].chcr = next;                             /* spec 7.4 */
        if (!busy) dsm_stop(k);                               /* spec 7.5 */
        return 1;
    }
    if (ch == 5) {                                            /* [EMU] spec 9.5 */
        channels[5].chcr = next;
        return 1;
    }
    if (!was_busy && busy && dsm_kick_machine(ch) >= 0) {     /* spec 6.1, 7.6 */
        channels[ch].chcr = next;
        try_execute(ch);
        return 1;
    }
    if (ch == 6) {
        channels[6].chcr = next;
        if (was_busy && !busy) cancel_async_transfer(6);
        if (!was_busy && busy) try_execute(6);
        return 1;
    }
    *valp = next;
    return 0;
}

/* Service point entry for the source GPU runtime (grid-edge DMA events, the
 * WRITE event of a DMA register write, the frame-return DMA event). */
void dma_source_gpu_service_at(uint64_t cycle) {
    g_dma_exec_depth++;
    dsm_service_point(cycle);
    g_dma_exec_depth--;
}

/* The MDEC contract harness's authored service point. The harness's own calls
 * are its service points: its time base has no known grid phase against the
 * oracle that produced the transcripts (spec 11: do not assume an offset of 0),
 * so no grid edges are inserted between them. */
void dma_source_gpu_service_at_exact(uint64_t cycle) {
    g_dma_exec_depth++;
    if (cycle > dsm_service_clock()) dsm_serve_at(cycle);
    else dsm_eval_halt_stall();
    g_dma_exec_depth--;
}

/* Cycles until the next source service point (the next grid edge) while a
 * machine runs or the source MDEC is active. */
static uint32_t dsm_cycles_to_event(int armed_only) {
    uint32_t edge = (uint32_t)(DSM_QUANTUM - psx_cycle_count % DSM_QUANTUM);
    if (!armed_only && mdec_source_active()) return edge;
    for (int k = 0; k < DSM_COUNT; k++) {
        if (!dsm[k].running) continue;
        if (armed_only && !channel_irq_flag_armed(dsm_channel[k])) continue;
        return edge;
    }
    return UINT32_MAX;
}

/* Load wait while an upload runs, sampled once per load by memory.c. */
uint32_t dma_cpu_read_penalty(void) {
    return g_dma_cpu_read_wait;
}

/* The source CPU owner publishes the wait at each instruction boundary. */
void dma_cpu_read_wait_boundary(void) {
    g_dma_cpu_read_wait = dsm_wait_live;
}


void dma_source_mdec_service_at_exact(uint64_t cycle){dma_source_gpu_service_at_exact(cycle);}
static void dsm_reset(void){
 memset(dsm,0,sizeof dsm);dsm_wait_live=g_dma_cpu_read_wait=0;dsm_mdec_clock=psx_cycle_count;dsm_write_serviced=0;
 for(int k=0;k<DSM_COUNT;++k)dsm[k].served_until=psx_cycle_count;
 const char *gpu=getenv("PSX_GPU_DMA_MODEL"),*cd=getenv("PSX_CD_DMA_MODEL"),*otc=getenv("PSX_INPUT_ROUTE_DMA_MODEL");
 gpu_upload_source_model=gpu&&*gpu;
 gpu_ll_source_model=gpu&&(!strcmp(gpu,"octoshock-2.2.2-bounded-linked-list")||!strcmp(gpu,"octoshock-2.2.2-bounded-quad"));
 cd_source_model=cd&&*cd;otc_source_model=otc&&*otc;dsm_spu_model=gpu_upload_source_model;
 if(gpu_upload_source_model && ((!gpu_ll_source_model&&strcmp(gpu,"octoshock-2.2.2-vram-upload"))||!getenv("PSX_INPUT_ROUTE_FILE")))dsm_fail("invalid source GPU model or missing route");
 if(cd_source_model&&(strcmp(cd,"octoshock-2.2.2")||!getenv("PSX_INPUT_ROUTE_FILE")))dsm_fail("invalid source CD model or missing route");
 if(otc_source_model&&(strcmp(otc,"octoshock-2.2.2-otc")||!getenv("PSX_INPUT_ROUTE_FILE")))dsm_fail("invalid source OTC model or missing route");
}

/* Keep the maintained SMD1 MDEC tail; full bus adds an explicit configured SDB1
 * tail rather than reinterpreting a legacy stage DMA section. */
#define SMD_TAIL_BYTES 56u
#define SDB_TAIL_BYTES 308u
static uint32_t dsm_flags(void){
 unsigned gpu=gpu_upload_source_model?(gpu_ll_source_model?(!strcmp(getenv("PSX_GPU_DMA_MODEL"),"octoshock-2.2.2-bounded-quad")?3u:2u):1u):0u;
 return gpu|(cd_source_model?4u:0u)|(otc_source_model?8u:0u)|(dsm_spu_model?16u:0u)|(source_gpu_runtime_active()?32u:0u);
}
static uint32_t smd_tail_bytes(void){return(mdec_source_active()?SMD_TAIL_BYTES:0u)+(dsm_flags()?SDB_TAIL_BYTES:0u);}
static void dsm_w_machine(PstW *w,const DmaSrcMachine *m){
 pst_w_u8(w,m->running);pst_w_u8(w,m->halts_cpu);pst_w_u8(w,m->stage);pst_w_u8(w,m->held);
 pst_w_u32(w,m->cursor);pst_w_u32(w,m->words_left);pst_w_u32(w,m->blk_words);pst_w_u32(w,m->blk_pos);pst_w_u32(w,m->node_count);pst_w_u32(w,m->link);pst_w_i32(w,m->credit);pst_w_u64(w,m->served_until);
}
static int dsm_r_machine(PstR *r,DmaSrcMachine *m){
 return pst_r_u8(r,&m->running)&&pst_r_u8(r,&m->halts_cpu)&&pst_r_u8(r,&m->stage)&&pst_r_u8(r,&m->held)&&pst_r_u32(r,&m->cursor)&&pst_r_u32(r,&m->words_left)&&pst_r_u32(r,&m->blk_words)&&pst_r_u32(r,&m->blk_pos)&&pst_r_u32(r,&m->node_count)&&pst_r_u32(r,&m->link)&&pst_r_i32(r,&m->credit)&&pst_r_u64(r,&m->served_until);
}
static int dsm_machine_valid(const DmaSrcMachine *m,uint64_t clock){
 return m->running<=1 && m->halts_cpu<=1 && m->stage<=1 && m->held<=1 && (m->running||(!m->halts_cpu&&!m->stage)) && m->cursor<=0xffffffu && m->link<=0xffffffu && m->words_left<=65536u && m->blk_words<=65536u && m->credit>=-2048 && m->credit<=0x40000000 && m->served_until<=clock;
}
static void smd_tail_write(PstW *w){
 if(mdec_source_active()){
  pst_w_u32(w,0x31444d53u);pst_w_u64(w,dsm_mdec_clock);
  for(int ch=0;ch<2;++ch){DmaSrcMachine *m=&dsm[ch?DSM_MDEC_OUT:DSM_MDEC_IN];pst_w_u8(w,m->running);pst_w_u8(w,m->stage);pst_w_u32(w,m->cursor);pst_w_u32(w,m->words_left);pst_w_i32(w,m->credit);pst_w_u64(w,m->served_until);}
 }
 if(dsm_flags()){
  pst_w_u32(w,0x31424453u);pst_w_u32(w,dsm_flags());pst_w_u32(w,g_psx_ram_size);
  for(int k=0;k<DSM_COUNT;++k)dsm_w_machine(w,&dsm[k]);
  pst_w_u32(w,dsm_wait_live);pst_w_u32(w,g_dma_cpu_read_wait);pst_w_u64(w,dsm_mdec_clock);
 }
}
static int smd_tail_read(const uint8_t *p,uint32_t n,int apply){
 if(n!=smd_tail_bytes() || (n&&!p))return 0;
 if(!n)return 1;
 PstR r;pst_r_init(&r,p,n);DmaSrcMachine next[DSM_COUNT]={0},oldm[2]={{0}};uint64_t clock=psx_cycle_count,oldclock=0;uint32_t live=0,published=0,magic;
 if(mdec_source_active()){
  if(!pst_r_u32(&r,&magic)||magic!=0x31444d53u||!pst_r_u64(&r,&oldclock))return 0;clock=oldclock;
  for(int ch=0;ch<2;++ch){DmaSrcMachine *m=&oldm[ch];
   if(!pst_r_u8(&r,&m->running)||!pst_r_u8(&r,&m->stage)||!pst_r_u32(&r,&m->cursor)||!pst_r_u32(&r,&m->words_left)||!pst_r_i32(&r,&m->credit)||!pst_r_u64(&r,&m->served_until)||!dsm_machine_valid(m,clock))return 0;
   next[ch?DSM_MDEC_OUT:DSM_MDEC_IN]=*m;
  }
 }
 if(dsm_flags()){
  uint32_t flags,ram;
  if(!pst_r_u32(&r,&magic)||magic!=0x31424453u||!pst_r_u32(&r,&flags)||flags!=dsm_flags()||!pst_r_u32(&r,&ram)||ram!=g_psx_ram_size)return 0;
  for(int k=0;k<DSM_COUNT;++k)if(!dsm_r_machine(&r,&next[k]))return 0;
  if(!pst_r_u32(&r,&live)||!pst_r_u32(&r,&published)||!pst_r_u64(&r,&clock)||live>DSM_STALL_CAP||published>DSM_STALL_CAP)return 0;
  for(int k=0;k<DSM_COUNT;++k)if(!dsm_machine_valid(&next[k],clock))return 0;
  if(mdec_source_active())for(int ch=0;ch<2;++ch){DmaSrcMachine *m=&next[ch?DSM_MDEC_OUT:DSM_MDEC_IN],*o=&oldm[ch];if(clock!=oldclock||m->running!=o->running||m->stage!=o->stage||m->cursor!=o->cursor||m->words_left!=o->words_left||m->credit!=o->credit||m->served_until!=o->served_until)return 0;}
 }else{
  /* Old SMD1 omitted counters unused by MDEC addressing; reconstruct these
   * diagnostic/block fields from the restored public channel registers. */
  for(int ch=0;ch<2;++ch){DmaSrcMachine *m=&next[ch?DSM_MDEC_OUT:DSM_MDEC_IN];m->blk_words=channels[ch].bcr&0xffffu;if(!m->blk_words)m->blk_words=65536;m->blk_pos=m->blk_words>=m->words_left?m->blk_words-m->words_left:0;m->link=channels[ch].madr&0xffffffu;m->halts_cpu=m->running&&m->stage&&m->words_left&&((channels[ch].chcr>>8)&7u)==0u;}
 }
 if(r.p!=r.end)return 0;
 if(apply){memcpy(dsm,next,sizeof dsm);dsm_wait_live=live;g_dma_cpu_read_wait=published;dsm_mdec_clock=clock;}
 return 1;
}
unsigned dma_source_transfer_active_mask(void){unsigned bits=0;for(int k=0;k<DSM_COUNT;++k)if(dsm[k].running)bits|=1u<<dsm_channel[k];return bits;}
int dma_source_transfer_active(void){return dma_source_transfer_active_mask()!=0;}

int dma_cpu_source_halted(void){for(int k=0;k<DSM_COUNT;++k)if(dsm[k].halts_cpu)return 1;return 0;}

/* Accepted e841 public source-DMA state boundary, same dsm owner. */
#define DSM_WIRE_MACHINE 40u
#define DSM_WIRE_BYTES (DSM_COUNT * DSM_WIRE_MACHINE + 16u)
int dma_src_active(void) {
    return dsm_profile_any();
}

unsigned dma_channels_busy_mask(void) {
    unsigned m = 0;
    for (int i = 0; i < 7; i++) if (channels[i].chcr & 0x01000000u) m |= 1u << i;
    return m;
}

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
