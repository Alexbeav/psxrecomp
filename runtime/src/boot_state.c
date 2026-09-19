#include "boot_state.h"
#include "mod_memory.h"
#include "fntrace.h"
#include "overlay_api.h"   /* PSX_OVERLAY_CODEGEN_HASH / _ABI_TAG / _CODEGEN_VER */
#include "dirty_ram_interp.h"
#include "gpu.h"           /* gpu_get_vram — CPU-auth mirror under dual-raster   */
#include "gpu_render.h"    /* gr_vram_transfer_in / gr_vram_transfer_out          */
#include "gpu_vram_dirty.h"
#include "cpu_state.h"     /* gte_canonicalize_cpu_state after CPU wire restore   */
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_icache.h"    /* g_psx_icache_tv — fetch-cost tags in BS_SEC_ICACHE */
#include "psx_scheduler.h"
#include "source_gpu_runtime.h" /* source GPU service + raster sections */
#include "timers.h"             /* BS_SEC_TIMER_SRC */
#include "dma.h"                /* BS_SEC_DMA_SRC */
#include "input_route_raster_clock_wire.h"
#include "pst_wire.h"
#include "cpu_state_wire.h"
#include "dirty_ram_interp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

static double boot_state_mono_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

/* Compress payloads at/above this size (RAM/VRAM/SPU/dirty dominate I/O). */
#define BOOT_STATE_ZLIB_MIN 256u

#define RAM_SIZE   (2u * 1024u * 1024u)
#define SPAD_SIZE  (1024u)
#define VRAM_W     1024
#define VRAM_H     512
#define VRAM_SIZE  ((uint32_t)(VRAM_W * VRAM_H * 2))  /* 1 MB, 16bpp */

/* ---- core accessors (existing runtime modules) ---- */
extern uint8_t*  memory_get_ram_ptr(void);
extern uint8_t*  memory_get_scratchpad_ptr(void);
extern uint32_t  i_stat;
extern uint32_t  i_mask;
extern uint64_t  psx_cycle_count;
extern void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                                uint16_t target[3], int32_t irq_line[3],
                                uint32_t frac[3]);
extern void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                                const uint16_t target[3], const int32_t irq_line[3],
                                const uint32_t frac[3]);

/* ---- per-subsystem complete-state accessors (defined in each module) ---- */
extern uint32_t gpu_snapshot_bytes(void);
extern void     gpu_snapshot_write(uint8_t* p);
extern int      gpu_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t spu_snapshot_bytes(void);
extern void     spu_snapshot_write(uint8_t* p);
extern int      spu_snapshot_read(const uint8_t* p, uint32_t len);
extern int      spu_snapshot_validate(const uint8_t* p, uint32_t len);
extern uint8_t* spu_get_ram_ptr(void);
extern uint32_t spu_get_ram_bytes(void);
extern uint32_t cdrom_snapshot_bytes(void);
extern void     cdrom_snapshot_write(uint8_t* p);
extern int      cdrom_snapshot_read(const uint8_t* p, uint32_t len);
extern int      cdrom_snapshot_validate(const uint8_t* p, uint32_t len);
extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t* p);
extern int      dma_snapshot_read(const uint8_t* p, uint32_t len);
extern uint32_t sio_snapshot_bytes(void);
extern void     sio_snapshot_write(uint8_t* p);
extern int      sio_snapshot_read(const uint8_t* p, uint32_t len);
extern int      sio_snapshot_validate(const uint8_t* p, uint32_t len);
extern int      mdec_snapshot_prepare(const uint8_t* p, uint32_t len);
extern int      sio_snapshot_shape_ok(uint32_t len);
extern uint32_t mdec_snapshot_bytes(void);
extern void     mdec_snapshot_write(uint8_t* p);
extern int      mdec_snapshot_read(const uint8_t* p, uint32_t len);

/* CPU wire: 524 register bytes + 56 timing bytes, without padding/pointers. */
#define CPU_REGS_WIRE_BYTES CPU_STATE_WIRE_BYTES
/* Timer wire: 3*u16 + 3*u32 + 3*u16 + 3*i32 + 3*u32 = 48 bytes (no pad holes). */
#define TIMER_REGS_WIRE_BYTES (48u)

/* ---- deferred capture state (armed before first boot, fired at handoff) ---- */
static char     s_capture_path[512];
static uint32_t s_capture_checksum;
static uint32_t s_capture_entry_pc;

/* §96: persistent VRAM mirror for raw ring snaps — patch dirty scanlines only. */
static uint16_t s_vram_mirror[VRAM_W * VRAM_H];
static int      s_vram_mirror_valid;
static uint32_t s_last_vram_dirty_rows;
static int      s_last_vram_incremental;

uint32_t boot_state_last_vram_dirty_rows(void)
{
    return s_last_vram_dirty_rows;
}

int boot_state_last_vram_incremental(void)
{
    return s_last_vram_incremental;
}

void boot_state_vram_mirror_reset(void)
{
    s_vram_mirror_valid = 0;
    s_last_vram_dirty_rows = VRAM_H;
    s_last_vram_incremental = 0;
}

/* Build s_vram_mirror from live CPU VRAM using dirty rows when possible.
 * Always emits a full 1 MiB BS_SEC_VRAM (loads stay independent).
 * Only used while gpu_vram_dirty_tracking() (rollback netplay). */
static int sync_vram_mirror_for_save(void)
{
    const uint16_t *live = gpu_get_vram();
    uint32_t dirty_n;
    uint32_t y;

    if (!gpu_vram_dirty_tracking()) {
        /* Should not be called offline — full refresh fallback. */
        if (live)
            memcpy(s_vram_mirror, live, VRAM_SIZE);
        else
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, s_vram_mirror);
        s_vram_mirror_valid = 1;
        s_last_vram_dirty_rows = VRAM_H;
        s_last_vram_incremental = 0;
        return 1;
    }

    dirty_n = gpu_vram_dirty_row_count();
    s_last_vram_dirty_rows = dirty_n;

    if (!live) {
        gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, s_vram_mirror);
        s_vram_mirror_valid = 1;
        s_last_vram_dirty_rows = VRAM_H;
        s_last_vram_incremental = 0;
        gpu_vram_dirty_clear();
        return 1;
    }

    if (!s_vram_mirror_valid || dirty_n >= VRAM_H) {
        memcpy(s_vram_mirror, live, VRAM_SIZE);
        s_vram_mirror_valid = 1;
        s_last_vram_incremental = 0;
    } else if (dirty_n == 0u) {
        /* Mirror already matches live. */
        s_last_vram_incremental = 1;
    } else {
        const uint64_t *mask = gpu_vram_dirty_mask();
        for (y = 0; y < VRAM_H; y++) {
            if (mask[y >> 6] & ((uint64_t)1u << (y & 63u))) {
                memcpy(s_vram_mirror + (size_t)y * VRAM_W,
                       live + (size_t)y * VRAM_W,
                       (size_t)VRAM_W * sizeof(uint16_t));
            }
        }
        s_last_vram_incremental = 1;
    }

    if (gpu_vram_dirty_verify_enabled()) {
        uint16_t *full = (uint16_t *)malloc(VRAM_SIZE);
        if (full) {
            gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, full);
            if (memcmp(full, s_vram_mirror, VRAM_SIZE) != 0) {
                fprintf(stderr,
                        "psxrecomp: VRAM dirty VERIFY FAIL dirty_rows=%u "
                        "incr=%d — forcing full mirror\n",
                        (unsigned)dirty_n, s_last_vram_incremental);
                fflush(stderr);
                memcpy(s_vram_mirror, full, VRAM_SIZE);
                s_last_vram_incremental = 0;
                s_last_vram_dirty_rows = VRAM_H;
            }
            free(full);
        }
    }

    gpu_vram_dirty_clear();
    return 1;
}

/* File or growable memory sink — both save paths share one serializer. */
typedef struct BsOut {
    FILE*    f;       /* non-NULL => write to file */
    uint8_t* data;    /* memory sink (owned by caller / save_buffer) */
    size_t   len;
    size_t   cap;
    int      no_zlib; /* 1 => always raw sections (netplay snap ring) */
} BsOut;

static int bs_write(BsOut* o, const void* p, size_t n) {
    if (!n) return 1;
    if (o->f)
        return fwrite(p, 1, n, o->f) == n;
    if (o->len + n > o->cap) {
        size_t nc = o->cap ? o->cap * 2u : (256u * 1024u);
        uint8_t* nd;
        while (nc < o->len + n) {
            if (nc > (SIZE_MAX / 2u)) return 0;
            nc *= 2u;
        }
        nd = (uint8_t*)realloc(o->data, nc);
        if (!nd) return 0;
        o->data = nd;
        o->cap = nc;
    }
    memcpy(o->data + o->len, p, n);
    o->len += n;
    return 1;
}

static int write_header_le(BsOut* o, const BootStateHeader* h) {
    uint8_t buf[BOOT_STATE_HEADER_WIRE_BYTES];
    PstW w;
    pst_w_init(&w, buf, sizeof buf);
    if (!pst_w_u32(&w, h->magic) ||
        !pst_w_u32(&w, h->version) ||
        !pst_w_u32(&w, h->bios_checksum) ||
        !pst_w_u32(&w, h->entry_pc) ||
        !pst_w_u32(&w, h->codegen_hash) ||
        !pst_w_i32(&w, h->abi_tag) ||
        !pst_w_u32(&w, h->codegen_ver) ||
        !pst_w_u32(&w, h->section_count) ||
        !pst_w_u32(&w, h->reserved) ||
        w.written != BOOT_STATE_HEADER_WIRE_BYTES)
        return 0;
    return bs_write(o, buf, sizeof buf);
}

static int write_section_raw(BsOut* o, uint32_t tag, uint32_t flags,
                             const void* data, uint64_t len) {
    uint8_t hdr[16];
    PstW w;
    pst_w_init(&w, hdr, sizeof hdr);
    if (!pst_w_u32(&w, tag) || !pst_w_u32(&w, flags) || !pst_w_u64(&w, len))
        return 0;
    if (!bs_write(o, hdr, sizeof hdr)) return 0;
    if (len && !bs_write(o, data, (size_t)len)) return 0;
    return 1;
}

/* Prefer zlib for large blobs (smaller disk + faster load on slow storage).
 * Falls back to raw if compressBound/compress fails.
 * o->no_zlib skips compress entirely (in-memory netplay ring). */
static int write_section(BsOut* o, uint32_t tag, const void* data, uint64_t len) {
    if (!data && len) return 0;
    if (!o->no_zlib && len >= BOOT_STATE_ZLIB_MIN && len <= 0xffffffffu) {
        uLong bound = compressBound((uLong)len);
        uint8_t* packed = (uint8_t*)malloc(4u + (size_t)bound);
        if (packed) {
            PstW lw;
            uLong dest_len = bound;
            pst_w_init(&lw, packed, 4);
            if (pst_w_u32(&lw, (uint32_t)len) &&
                compress2(packed + 4, &dest_len, (const Bytef*)data, (uLong)len,
                          Z_BEST_SPEED) == Z_OK) {
                uint64_t payload = 4u + (uint64_t)dest_len;
                int ok = write_section_raw(o, tag, BOOT_STATE_SEC_ZLIB,
                                           packed, payload);
                free(packed);
                return ok;
            }
            free(packed);
        }
    }
    return write_section_raw(o, tag, 0u, data, len);
}

static int write_module_section(BsOut* o, uint32_t tag,
                                uint32_t (*bytes)(void),
                                void (*write)(uint8_t*)) {
    uint32_t n = bytes();
    if (!n) {
        fprintf(stderr, "[stateio] refusing snapshot: required section %u has no serializer\n", tag);
        return 0;
    }
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1);
    if (!buf) return 0;
    write(buf);
    int ok = write_section(o, tag, buf, n);
    free(buf);
    return ok;
}

static int write_cpu_section(BsOut* o, const CPUState* cpu) {
    uint8_t buf[CPU_REGS_WIRE_BYTES];
    CPUState saved = *cpu;
    saved.pc = dirty_ram_checkpoint_pc(cpu->pc);
    return cpu_state_wire_write(buf, &saved) &&
           write_section(o, BS_SEC_CPU, buf, sizeof buf);
}

static int write_timer_section(BsOut* o) {
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    uint8_t buf[TIMER_REGS_WIRE_BYTES];
    PstW w;
    timers_get_snapshot(counter, mode, target, irq_line, frac);
    pst_w_init(&w, buf, sizeof buf);
    for (int i = 0; i < 3; i++)
        if (!pst_w_u16(&w, counter[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u32(&w, mode[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u16(&w, target[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_i32(&w, irq_line[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_u32(&w, frac[i])) return 0;
    if (w.written != TIMER_REGS_WIRE_BYTES) return 0;
    return write_section(o, BS_SEC_TIMER, buf, TIMER_REGS_WIRE_BYTES);
}

/* ============================ SAVE ============================ */

/* Classic full VRAM section (offline / zlib / tracking off). */
static int write_vram_section_full(BsOut *o)
{
    uint16_t *vbuf = (uint16_t *)malloc(VRAM_SIZE);
    int ok;
    if (!vbuf)
        return 0;
    gr_vram_transfer_out(0, 0, VRAM_W, VRAM_H, vbuf);
    s_last_vram_dirty_rows = VRAM_H;
    s_last_vram_incremental = 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    ok = write_section(o, BS_SEC_VRAM, vbuf, VRAM_SIZE);
#else
    {
        uint8_t *wire = (uint8_t *)malloc(VRAM_SIZE);
        if (!wire) {
            free(vbuf);
            return 0;
        }
        {
            PstW w;
            pst_w_init(&w, wire, VRAM_SIZE);
            ok = pst_w_pod(&w, vbuf, VRAM_SIZE, 2) &&
                 write_section(o, BS_SEC_VRAM, wire, VRAM_SIZE);
        }
        free(wire);
    }
#endif
    free(vbuf);
    return ok;
}

/* v7 profile-aware section presence (amendment A): a section must exist
 * exactly when its subsystem is active. boot_state.c is shared code, so the
 * three comparison-profile sections must NOT be always-required — under a
 * normal profile there is no raster clock, no source GPU service and no source
 * timers, and always-requiring them would break every normal boot state. */
/* Deferred VBlank phase (reshaped #5). BS_SEC_IRQ is written before
 * BS_SEC_RASTER, so at IRQ-apply time we cannot yet know whether the raster
 * section is coming. Stage the phase here and commit it after the whole stream
 * has been read, where the section's presence is known. */
static int      s_pending_vblank_phase_valid;
static uint32_t s_pending_vblank_phase;
static int      s_pending_game_started;

static int boot_state_raster_section_active(void) {
    return interrupts_raster_comparison_active() || source_gpu_runtime_active();
}
static uint32_t boot_state_extra_sections(void) {
    uint32_t n = 0;
    if (boot_state_raster_section_active()) n++;
    if (source_gpu_runtime_active()) n++;
    if (timers_source_active()) n++;
    if (dma_src_active()) n++;
    return n;
}

static int boot_state_save_to(BsOut* o, const CPUState* cpu,
                              uint32_t bios_checksum, uint32_t entry_pc) {
    BootStateHeader h;
    int ok;
    memset(&h, 0, sizeof h);
    h.magic         = BOOT_STATE_MAGIC;
    h.version       = BOOT_STATE_VERSION;
    h.reserved      = psx_mod_memory_layout_cookie();
    h.bios_checksum = bios_checksum;
    h.entry_pc      = entry_pc;
    h.codegen_hash  = (uint32_t)PSX_OVERLAY_CODEGEN_HASH;
    h.abi_tag       = (int32_t)PSX_OVERLAY_ABI_TAG;
    h.codegen_ver   = (uint32_t)PSX_OVERLAY_CODEGEN_VER;
    h.section_count = 20u + boot_state_extra_sections() +
                      (psx_mod_memory_snapshot_bytes() ? 1u : 0u);

    ok = write_header_le(o, &h);

    if (ok) ok = write_cpu_section(o, cpu);
    if (ok) {
        uint8_t exec[DIRTY_RAM_CHECKPOINT_BYTES];
        dirty_ram_checkpoint_write(exec);
        ok = write_section(o, BS_SEC_CPU_EXEC, exec, sizeof exec);
    }
    if (ok) ok = write_section(o, BS_SEC_RAM,  memory_get_ram_ptr(),        RAM_SIZE);
    if (ok) {
        uint8_t sched[PSX_SCHEDULER_SNAPSHOT_BYTES];
        psx_scheduler_snapshot_write(sched, sizeof sched);
        ok = write_section(o, BS_SEC_SCHED, sched, sizeof sched);
    }
    if (ok) {
        uint8_t flow[BOOT_STATE_BOOTFLOW_BYTES];
        PstW w;
        pst_w_init(&w, flow, sizeof flow);
        ok = pst_w_u32(&w, fntrace_is_game_started() ? BOOT_STATE_BOOTFLOW_GAME_STARTED : 0u) &&
             write_section(o, BS_SEC_BOOTFLOW, flow, sizeof flow);
    }
    if (ok) ok = write_section(o, BS_SEC_SPAD, memory_get_scratchpad_ptr(), SPAD_SIZE);
    if (ok && psx_mod_memory_snapshot_bytes())
        ok = write_module_section(o, BS_SEC_MODMEM, psx_mod_memory_snapshot_bytes,
                                  psx_mod_memory_snapshot_write);
    if (ok) {
        /* 12B: i_stat, i_mask, cycles_since_vblank. Zeroing csv on warm load
         * rebased every tip to phase 0 and forked MotK wait-loop resim
         * (IRQ at CD54 vs CDA0). Selfcheck already restored csv out-of-band. */
        uint8_t irq[12];
        PstW w;
        pst_w_init(&w, irq, sizeof irq);
        ok = pst_w_u32(&w, i_stat) && pst_w_u32(&w, i_mask) &&
             pst_w_u32(&w, interrupts_get_cycles_since_vblank()) &&
             write_section(o, BS_SEC_IRQ, irq, 12);
    }
    if (ok) ok = write_timer_section(o);
    if (ok) {
        uint8_t cyc[8];
        PstW w;
        pst_w_init(&w, cyc, sizeof cyc);
        /* Publish deferred load-charge batch before snapshotting the clock. */
        psx_cyc_batch_flush();
        ok = pst_w_u64(&w, psx_cycle_count) &&
             write_section(o, BS_SEC_CLOCK, cyc, 8);
    }
    if (ok) ok = write_module_section(o, BS_SEC_GPU, gpu_snapshot_bytes, gpu_snapshot_write);
    if (ok) {
        /* §96 incremental mirror only while RB dirty-tracking is on.
         * Offline / delay-sync / zlib disk: classic full transfer_out. */
        if (o->no_zlib && gpu_vram_dirty_tracking()) {
            ok = sync_vram_mirror_for_save();
            if (ok) {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
                ok = write_section(o, BS_SEC_VRAM, s_vram_mirror, VRAM_SIZE);
#else
                {
                    uint8_t* wire = (uint8_t*)malloc(VRAM_SIZE);
                    if (!wire) ok = 0;
                    else {
                        PstW w;
                        pst_w_init(&w, wire, VRAM_SIZE);
                        ok = pst_w_pod(&w, s_vram_mirror, VRAM_SIZE, 2) &&
                             write_section(o, BS_SEC_VRAM, wire, VRAM_SIZE);
                        free(wire);
                    }
                }
#endif
            }
        } else {
            ok = write_vram_section_full(o);
        }
    }
    if (ok) ok = write_module_section(o, BS_SEC_SPU, spu_snapshot_bytes, spu_snapshot_write);
    if (ok) ok = write_section(o, BS_SEC_SPURAM, spu_get_ram_ptr(), spu_get_ram_bytes());
    if (ok) ok = write_module_section(o, BS_SEC_CDROM, cdrom_snapshot_bytes, cdrom_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_DMA,   dma_snapshot_bytes,   dma_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_SIO,   sio_snapshot_bytes,   sio_snapshot_write);
    if (ok) ok = write_module_section(o, BS_SEC_MDEC,  mdec_snapshot_bytes,  mdec_snapshot_write);
    if (ok) {
        /* I-cache tags: warm loads must replay with the fetch-cost state the
         * live timeline had, or miss cycles differ per peer/retry and IRQ
         * delivery forks a few wait-loop iterations (MotK abort@940). */
        uint8_t ib[1024u * 4u];
        PstW w;
        pst_w_init(&w, ib, sizeof ib);
        ok = 1;
        for (uint32_t i = 0; ok && i < 1024u; i++)
            ok = pst_w_u32(&w, g_psx_icache_tv[i]);
        if (ok) ok = write_section(o, BS_SEC_ICACHE, ib, sizeof ib);
    }
    if (ok) {
        uint32_t wc = dirty_ram_get_bitmap_word_count();
        uint64_t nbytes = (uint64_t)wc * 4u;
        uint8_t* db = (uint8_t*)malloc(nbytes ? (size_t)nbytes : 1);
        if (!db) ok = 0;
        else {
            PstW w;
            pst_w_init(&w, db, (size_t)nbytes);
            ok = 1;
            for (uint32_t i = 0; ok && i < wc; i++)
                ok = pst_w_u32(&w, dirty_ram_get_bitmap_word(i));
            if (ok) ok = write_section(o, BS_SEC_DIRTY, db, nbytes);
            free(db);
        }
    }
    /* v7 comparison-profile sections, written ONLY when their subsystem is
     * active, so a normal-profile boot state keeps the v6 shape. */
    if (ok && boot_state_raster_section_active()) {
        uint8_t buf[INPUT_ROUTE_RASTER_WIRE_BYTES * 3u];
        interrupts_raster_wire_write(buf);                                   /* [0]   */
        source_gpu_raster_wire_write(buf + INPUT_ROUTE_RASTER_WIRE_BYTES);   /* [1][2] */
        ok = write_section(o, BS_SEC_RASTER, buf, sizeof buf);
    }
    if (ok && source_gpu_runtime_active()) {
        uint8_t buf[SOURCE_GPU_SERVICE_WIRE_BYTES];
        source_gpu_service_wire_write(buf);
        ok = write_section(o, BS_SEC_GPU_SERVICE, buf, sizeof buf);
    }
    if (ok && timers_source_active()) {
        uint8_t buf[60u];                     /* source timer state machines */
        timers_source_wire_write(buf);
        ok = write_section(o, BS_SEC_TIMER_SRC, buf, sizeof buf);
    }
    if (ok && dma_src_active()) {
        uint8_t buf[140u];                    /* four source-DMA state machines */
        dma_src_wire_write(buf);
        ok = write_section(o, BS_SEC_DMA_SRC, buf, sizeof buf);
    }
    if (ok) {
        /* Always present: none of these fields is model-gated. */
        uint8_t buf[64u];
        interrupts_timing_wire_write(buf);
        ok = write_section(o, BS_SEC_IRQ_TIMING, buf, sizeof buf);
    }
    return ok;
}

/* Atomic replace of `to` by `from`. See boot_state_replace.c (its own TU so the
 * overwrite/refusal behaviour can be regression-tested in isolation). */

int boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                    uint32_t entry_pc, const char* path) {
    BsOut o;
    FILE* f;
    char tmp[1024];
    int ok;
    /* Validate BEFORE creating anything, and write to a temporary that is only
     * renamed into place on success. This kills the trap class where a refusal
     * (or any write failure) inside the serializer leaves a zero-byte or short
     * artifact at `path` — a truncated file that still parses is the same
     * silent-stub failure, one layer down. */
    if (!path || snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return 0;
    f = fopen(tmp, "wb");
    if (!f) return 0;
    memset(&o, 0, sizeof o);
    o.f = f;
    ok = boot_state_save_to(&o, cpu, bios_checksum, entry_pc);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return 0;
    }
    if (!boot_state_replace_file(tmp, path)) {
        remove(tmp);
        return 0;
    }
    return 1;
}

static int boot_state_save_buffer_ex(const CPUState* cpu, uint32_t bios_checksum,
                                     uint32_t entry_pc, uint8_t** out_data,
                                     size_t* out_len, int no_zlib) {
    BsOut o;
    if (!out_data || !out_len) return 0;
    *out_data = NULL;
    *out_len = 0;
    memset(&o, 0, sizeof o);
    o.no_zlib = no_zlib ? 1 : 0;
    /* Compressed MotK ~1.3–1.5 MiB; raw ~3.5–4 MiB (RAM+VRAM+SPU). */
    o.cap = no_zlib ? (5u * 1024u * 1024u) : (2u * 1024u * 1024u);
    o.data = (uint8_t*)malloc(o.cap);
    if (!o.data) return 0;
    if (!boot_state_save_to(&o, cpu, bios_checksum, entry_pc)) {
        free(o.data);
        return 0;
    }
    *out_data = o.data;
    *out_len = o.len;
    return 1;
}

int boot_state_save_buffer(const CPUState* cpu, uint32_t bios_checksum,
                           uint32_t entry_pc, uint8_t** out_data,
                           size_t* out_len) {
    return boot_state_save_buffer_ex(cpu, bios_checksum, entry_pc, out_data,
                                     out_len, 0);
}

int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios_checksum,
                               uint32_t entry_pc, uint8_t** out_data,
                               size_t* out_len) {
    return boot_state_save_buffer_ex(cpu, bios_checksum, entry_pc, out_data,
                                     out_len, 1);
}

/* ============================ LOAD ============================ */

static int apply_section(uint32_t tag, const uint8_t* p, uint32_t len,
                         CPUState* cpu, uint32_t entry_pc) {
    switch (tag) {
    case BS_SEC_CPU:
        if (!cpu_state_wire_read(p, len, cpu)) return 0;
        /* The snapshot is an exact backing-state image, not a guest register
         * write. Normalization would change untouched cold LZCR from 0 to 32. */
        gte_precision_timeline_invalidate();
        return 1;
    case BS_SEC_CPU_EXEC:
        return dirty_ram_checkpoint_read(p, len);
    case BS_SEC_RAM:
        if (len != RAM_SIZE) return 0;
        memcpy(memory_get_ram_ptr(), p, RAM_SIZE);
        {
            extern void psx_kernel_bless_note_range(uint32_t phys, uint32_t l);
            psx_kernel_bless_note_range(0, RAM_SIZE);
        }
        return 1;
    case BS_SEC_RASTER: {
        /* 3 x 80 B in fixed order. Each instance's read refuses on its own
         * fraction/cycle cross-check, so a cross-instance swap is caught. */
        if (len != INPUT_ROUTE_RASTER_WIRE_BYTES * 3u) return 0;
        if (!interrupts_raster_wire_read(p, INPUT_ROUTE_RASTER_WIRE_BYTES)) return 0;
        if (!source_gpu_raster_wire_read(p + INPUT_ROUTE_RASTER_WIRE_BYTES,
                                         INPUT_ROUTE_RASTER_WIRE_BYTES * 2u)) return 0;
        return 1;
    }
    case BS_SEC_GPU_SERVICE:
        return source_gpu_service_wire_read(p, len);
    case BS_SEC_TIMER_SRC:
        return timers_source_wire_read(p, len);
    case BS_SEC_DMA_SRC:
        return dma_src_wire_read(p, len);
    case BS_SEC_IRQ_TIMING:
        return interrupts_timing_wire_read(p, len);
    case BS_SEC_SCHED:
        /* RAM precedes this section in every stream, so guest TCB pointers
         * can be validated against the restored kernel state. */
        return psx_scheduler_snapshot_read(p, len, cpu);
    case BS_SEC_BOOTFLOW: {
        /* Staged, not applied: the latch is committed only after every section
         * has loaded, so a refused load leaves the live boot flow untouched. */
        PstR r;
        uint32_t flags;
        if (len != BOOT_STATE_BOOTFLOW_BYTES) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u32(&r, &flags) || (flags & ~BOOT_STATE_BOOTFLOW_GAME_STARTED)) return 0;
        s_pending_game_started = (flags & BOOT_STATE_BOOTFLOW_GAME_STARTED) ? 1 : 0;
        return 1;
    }
    case BS_SEC_SPAD:
        if (len != SPAD_SIZE) return 0;
        memcpy(memory_get_scratchpad_ptr(), p, SPAD_SIZE);
        return 1;
    case BS_SEC_IRQ: {
        PstR r;
        uint32_t st, mk, csv;
        if (len != 8 && len != 12) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u32(&r, &st) || !pst_r_u32(&r, &mk)) return 0;
        i_stat = st;
        i_mask = mk;
        if (len == 12) {
            if (!pst_r_u32(&r, &csv)) return 0;
            s_pending_vblank_phase = csv;
            s_pending_vblank_phase_valid = 1;
        } else {
            /* Legacy UI/disk snaps: no phase — rebase like pre-csv saves. */
            s_pending_vblank_phase = 0;
            s_pending_vblank_phase_valid = 1;
        }
        return 1;
    }
    case BS_SEC_TIMER: {
        uint16_t counter[3], target[3];
        uint32_t mode[3], frac[3];
        int32_t irq_line[3];
        PstR r;
        if (len != TIMER_REGS_WIRE_BYTES) return 0;
        pst_r_init(&r, p, len);
        for (int i = 0; i < 3; i++)
            if (!pst_r_u16(&r, &counter[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u32(&r, &mode[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u16(&r, &target[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_i32(&r, &irq_line[i])) return 0;
        for (int i = 0; i < 3; i++)
            if (!pst_r_u32(&r, &frac[i])) return 0;
        timers_set_snapshot(counter, mode, target, irq_line, frac);
        return 1;
    }
    case BS_SEC_CLOCK: {
        PstR r;
        uint64_t cyc;
        if (len != 8) return 0;
        pst_r_init(&r, p, len);
        if (!pst_r_u64(&r, &cyc)) return 0;
        psx_cycle_count = cyc;
        return 1;
    }
    case BS_SEC_GPU:
        return gpu_snapshot_read(p, len);
    case BS_SEC_VRAM: {
        if (len != VRAM_SIZE) return 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        /* Wire == host layout: upload straight from the section buffer. */
        gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, (const uint16_t*)p);
        if (gpu_vram_dirty_tracking()) {
            memcpy(s_vram_mirror, p, VRAM_SIZE);
            s_vram_mirror_valid = 1;
            gpu_vram_dirty_clear();
        } else {
            s_vram_mirror_valid = 0;
        }
        return 1;
#else
        {
            uint16_t* vbuf;
            PstR r;
            vbuf = (uint16_t*)malloc(VRAM_SIZE);
            if (!vbuf) return 0;
            pst_r_init(&r, p, len);
            if (!pst_r_pod(&r, vbuf, VRAM_SIZE, 2)) {
                free(vbuf);
                return 0;
            }
            gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, vbuf);
            if (gpu_vram_dirty_tracking()) {
                memcpy(s_vram_mirror, vbuf, VRAM_SIZE);
                s_vram_mirror_valid = 1;
                gpu_vram_dirty_clear();
            } else {
                s_vram_mirror_valid = 0;
            }
            free(vbuf);
            return 1;
        }
#endif
    }
    case BS_SEC_SPU:
        return spu_snapshot_read(p, len);
    case BS_SEC_SPURAM:
        if (len != spu_get_ram_bytes()) return 0;
        memcpy(spu_get_ram_ptr(), p, len);
        return 1;
    case BS_SEC_CDROM:
        return cdrom_snapshot_read(p, len);
    case BS_SEC_DMA:
        return dma_snapshot_read(p, len);
    case BS_SEC_SIO:
        return sio_snapshot_read(p, len);
    case BS_SEC_MDEC:
        return mdec_snapshot_read(p, len);
    case BS_SEC_DIRTY: {
        uint32_t wc;
        uint32_t* words;
        PstR r;
        if (len % 4u) return 0;
        wc = len / 4u;
        words = (uint32_t*)malloc(len ? len : 1);
        if (!words) return 0;
        pst_r_init(&r, p, len);
        for (uint32_t i = 0; i < wc; i++) {
            if (!pst_r_u32(&r, &words[i])) {
                free(words);
                return 0;
            }
        }
        dirty_ram_set_bitmap_words(words, wc);
        free(words);
        return 1;
    }
    case BS_SEC_MODMEM:
        return psx_mod_memory_snapshot_read(p, len);
    case BS_SEC_ICACHE: {
        PstR r;
        if (len != 1024u * 4u) return 0;
        pst_r_init(&r, p, len);
        for (uint32_t i = 0; i < 1024u; i++)
            if (!pst_r_u32(&r, &g_psx_icache_tv[i])) return 0;
        return 1;
    }
    default:
        /* Unknown section: SKIP, never fail. This was `return 0`, which made
         * every state written by a build with one extra section a poison pill
         * for every build without it -- and worse than unloadable: the apply
         * loop had already restored RAM/VRAM/CPU by the time it hit the
         * unknown tag, so the refusal left a HALF-RESTORED machine that
         * wedged or died at PC=0. A sectioned state format exists precisely
         * so readers can step over what they do not know. */
        return 1;
    }
}

static int boot_state_parse_header(const uint8_t* file, size_t file_len,
                                   BootStateHeader* h_out) {
    PstR hr;
    if (!file || !h_out || file_len < BOOT_STATE_HEADER_WIRE_BYTES ||
        file_len > 64u * 1024u * 1024u) {
        return 0;
    }
    pst_r_init(&hr, file, BOOT_STATE_HEADER_WIRE_BYTES);
    memset(h_out, 0, sizeof(*h_out));
    if (!pst_r_u32(&hr, &h_out->magic) ||
        !pst_r_u32(&hr, &h_out->version) ||
        !pst_r_u32(&hr, &h_out->bios_checksum) ||
        !pst_r_u32(&hr, &h_out->entry_pc) ||
        !pst_r_u32(&hr, &h_out->codegen_hash) ||
        !pst_r_i32(&hr, &h_out->abi_tag) ||
        !pst_r_u32(&hr, &h_out->codegen_ver) ||
        !pst_r_u32(&hr, &h_out->section_count) ||
        !pst_r_u32(&hr, &h_out->reserved)) {
        return 0;
    }
    return 1;
}

static void boot_state_append_reason(char* reason, size_t reason_cap,
                                     const char* part) {
    size_t n;
    if (!reason || reason_cap == 0 || !part || !part[0]) return;
    n = strlen(reason);
    if (n + 1 >= reason_cap) return;
    if (n > 0) {
        reason[n++] = ',';
        reason[n] = '\0';
        if (n + 1 >= reason_cap) return;
    }
    snprintf(reason + n, reason_cap - n, "%s", part);
}

int boot_state_check_buffer(const uint8_t* file, size_t file_len,
                            uint32_t bios_checksum, uint32_t entry_pc,
                            char* reason, size_t reason_cap) {
    BootStateHeader h;
    char part[96];

    if (reason && reason_cap)
        reason[0] = '\0';

    if (!file || file_len < BOOT_STATE_HEADER_WIRE_BYTES) {
        boot_state_append_reason(reason, reason_cap, "missing_or_truncated");
        return 0;
    }
    if (file_len > 64u * 1024u * 1024u) {
        boot_state_append_reason(reason, reason_cap, "too_large");
        return 0;
    }
    if (!boot_state_parse_header(file, file_len, &h)) {
        boot_state_append_reason(reason, reason_cap, "header_parse");
        return 0;
    }

    if (h.magic != BOOT_STATE_MAGIC) {
        snprintf(part, sizeof(part), "magic=%08X(want %08X)",
                 (unsigned)h.magic, (unsigned)BOOT_STATE_MAGIC);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.reserved != psx_mod_memory_layout_cookie()) {
        boot_state_append_reason(reason, reason_cap, "enhancement_memory_layout");
        return 0;
    }
    if (h.version < BOOT_STATE_VERSION_MIN_READ ||
        h.version > BOOT_STATE_VERSION) {
        snprintf(part, sizeof(part), "version=%u(want %u..%u)",
                 (unsigned)h.version, (unsigned)BOOT_STATE_VERSION_MIN_READ,
                 (unsigned)BOOT_STATE_VERSION);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.bios_checksum != bios_checksum) {
        snprintf(part, sizeof(part), "bios=%08X(want %08X)",
                 (unsigned)h.bios_checksum, (unsigned)bios_checksum);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.entry_pc != entry_pc) {
        snprintf(part, sizeof(part), "entry=%08X(want %08X)",
                 (unsigned)h.entry_pc, (unsigned)entry_pc);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.codegen_hash != (uint32_t)PSX_OVERLAY_CODEGEN_HASH) {
        snprintf(part, sizeof(part), "codegen_hash=%08X(want %08X)",
                 (unsigned)h.codegen_hash,
                 (unsigned)PSX_OVERLAY_CODEGEN_HASH);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.abi_tag != (int32_t)PSX_OVERLAY_ABI_TAG) {
        snprintf(part, sizeof(part), "abi_tag=%d(want %d)",
                 (int)h.abi_tag, (int)PSX_OVERLAY_ABI_TAG);
        boot_state_append_reason(reason, reason_cap, part);
    }
    if (h.codegen_ver != (uint32_t)PSX_OVERLAY_CODEGEN_VER) {
        snprintf(part, sizeof(part), "codegen_ver=%u(want %u)",
                 (unsigned)h.codegen_ver, (unsigned)PSX_OVERLAY_CODEGEN_VER);
        boot_state_append_reason(reason, reason_cap, part);
    }

    if (reason && reason_cap && reason[0])
        return 0;
    return 1;
}

/* Pass-1 shape validation. Mirrors the length rule each reader enforces, but
 * reads no machine state and mutates nothing -- so a malformed stream is refused
 * BEFORE any section is applied. Without this, a failure part-way through left a
 * HALF-APPLIED machine (live state changed, replay then runs on a mix), and the
 * first failing section masked every later one. */
static int section_shape_ok(uint32_t tag, uint32_t len) {
    switch (tag) {
    case BS_SEC_CPU:        return len == CPU_REGS_WIRE_BYTES;
    case BS_SEC_CPU_EXEC:   return len == DIRTY_RAM_CHECKPOINT_BYTES;
    case BS_SEC_RAM:        return len == RAM_SIZE;
    case BS_SEC_SCHED:      return len == PSX_SCHEDULER_SNAPSHOT_BYTES;
    case BS_SEC_BOOTFLOW:   return len == BOOT_STATE_BOOTFLOW_BYTES;
    case BS_SEC_SPAD:       return len == SPAD_SIZE;
    case BS_SEC_MODMEM:     return len == psx_mod_memory_snapshot_bytes();
    case BS_SEC_IRQ:        return len == 8u || len == 12u;
    case BS_SEC_TIMER:      return len == TIMER_REGS_WIRE_BYTES;
    case BS_SEC_CLOCK:      return len == 8u;
    case BS_SEC_GPU:        return len == gpu_snapshot_bytes();
    case BS_SEC_VRAM:       return len == VRAM_SIZE;
    case BS_SEC_SPU:        return len == spu_snapshot_bytes();
    case BS_SEC_SPURAM:     return len == spu_get_ram_bytes();
    case BS_SEC_CDROM:      return len == cdrom_snapshot_bytes();
    case BS_SEC_DMA:        return len == dma_snapshot_bytes();
    case BS_SEC_SIO:        return sio_snapshot_shape_ok(len);
    case BS_SEC_MDEC:       return 1; /* variable FIFO lengths: mdec_snapshot_prepare */
    case BS_SEC_ICACHE:     return len == 1024u * 4u;
    case BS_SEC_DIRTY:      return (len % 4u) == 0u;
    case BS_SEC_RASTER:     return len == INPUT_ROUTE_RASTER_WIRE_BYTES * 3u;
    case BS_SEC_GPU_SERVICE:return len == SOURCE_GPU_SERVICE_WIRE_BYTES;
    case BS_SEC_TIMER_SRC:  return len == 60u;
    case BS_SEC_DMA_SRC:    return len == dma_src_wire_bytes();
    case BS_SEC_IRQ_TIMING: return len == 64u;
    default:                return 0;   /* unknown tag: refuse */
    }
}


typedef struct BsSection {
    const uint8_t *data;
    uint8_t *owned;
    uint32_t len;
} BsSection;

/* These checks must not change device state. Allocation admission follows them. */
static int section_content_ok(uint32_t tag, const BsSection *sec,
                              const BsSection *sections) {
    const uint8_t *p = sec->data;
    const uint32_t len = sec->len;
    switch (tag) {
    case BS_SEC_CPU: {
        CPUState temporary = {0};
        return cpu_state_wire_read(p, len, &temporary);
    }
    case BS_SEC_CPU_EXEC: return dirty_ram_checkpoint_validate(p, len);
    case BS_SEC_SCHED:
        return psx_scheduler_snapshot_validate(p, len, sections[BS_SEC_RAM].data);
    case BS_SEC_BOOTFLOW: {
        PstR r; uint32_t flags;
        pst_r_init(&r, p, len);
        return pst_r_u32(&r, &flags) && !(flags & ~BOOT_STATE_BOOTFLOW_GAME_STARTED);
    }
    case BS_SEC_RASTER:
        for (unsigned i = 0; i < 3; i++) {
            InputRouteRasterClock temporary;
            if (!input_route_raster_wire_read(&temporary,
                    p + i * INPUT_ROUTE_RASTER_WIRE_BYTES,
                    INPUT_ROUTE_RASTER_WIRE_BYTES)) return 0;
        }
        return 1;
    case BS_SEC_GPU_SERVICE: {
        PstR r; uint32_t count;
        pst_r_init(&r, p + 48, 4);
        return pst_r_u32(&r, &count) && count <= 32u;
    }
    case BS_SEC_MODMEM: return psx_mod_memory_snapshot_validate(p, len);
    case BS_SEC_SPU: return spu_snapshot_validate(p, len);
    case BS_SEC_CDROM: return cdrom_snapshot_validate(p, len);
    case BS_SEC_SIO: return sio_snapshot_validate(p, len);
    default: return 1; /* Fixed-width readers; MDEC is prepared separately. */
    }
}

int boot_state_peek_cpu_pc_buffer(const uint8_t* file, size_t file_len,
                                  uint32_t* out_pc) {
    BootStateHeader h;
    const uint8_t *cur, *end;

    if (!out_pc || !boot_state_parse_header(file, file_len, &h)) return 0;
    cur = file + BOOT_STATE_HEADER_WIRE_BYTES;
    end = file + file_len;
    for (uint32_t i = 0; i < h.section_count; i++) {
        PstR sh, cr;
        uint32_t tag, flags, raw_len, discard;
        uint64_t len;
        const uint8_t *payload, *data;
        uint8_t *owned = NULL;
        int ok;
        if ((size_t)(end - cur) < 16u) return 0;
        pst_r_init(&sh, cur, 16u);
        if (!pst_r_u32(&sh, &tag) || !pst_r_u32(&sh, &flags) ||
            !pst_r_u64(&sh, &len)) return 0;
        cur += 16;
        if (len > 64u * 1024u * 1024u || (uint64_t)(end - cur) < len) return 0;
        payload = cur;
        cur += (size_t)len;
        if (tag != BS_SEC_CPU) continue;
        raw_len = (uint32_t)len;
        data = payload;
        if (flags == BOOT_STATE_SEC_ZLIB) {
            PstR lr;
            uLong dest_len;
            if (len < 4u) return 0;
            pst_r_init(&lr, payload, 4u);
            if (!pst_r_u32(&lr, &raw_len) || raw_len != CPU_REGS_WIRE_BYTES) return 0;
            owned = (uint8_t*)malloc(raw_len);
            if (!owned) return 0;
            dest_len = raw_len;
            if (uncompress(owned, &dest_len, payload + 4, (uLong)(len - 4u)) != Z_OK ||
                dest_len != raw_len) {
                free(owned);
                return 0;
            }
            data = owned;
        } else if (flags != 0u) {
            return 0;
        }
        ok = raw_len == CPU_REGS_WIRE_BYTES;
        pst_r_init(&cr, data, raw_len);
        for (int reg = 0; ok && reg < 32; reg++)   /* gpr[0..31] precede pc */
            ok = pst_r_u32(&cr, &discard);
        ok = ok && pst_r_u32(&cr, out_pc);
        free(owned);
        return ok;
    }
    return 0;
}

int boot_state_peek_cpu_pc(const char* path, uint32_t* out_pc) {
    FILE* f;
    long sz;
    uint8_t* file;
    int ok;

    if (!path || !out_pc || !(f = fopen(path, "rb"))) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < (long)BOOT_STATE_HEADER_WIRE_BYTES ||
        sz > 64L * 1024L * 1024L || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    file = (uint8_t*)malloc((size_t)sz);
    if (!file) { fclose(f); return 0; }
    ok = fread(file, 1, (size_t)sz, f) == (size_t)sz &&
         boot_state_peek_cpu_pc_buffer(file, (size_t)sz, out_pc);
    free(file);
    fclose(f);
    return ok;
}

int boot_state_load_buffer(const uint8_t* file, size_t file_len,
                           uint32_t bios_checksum, uint32_t entry_pc,
                           CPUState* cpu) {
    const uint8_t *cur, *end;
    BootStateHeader h;
    char reject[256];
    BsSection sections[BS_SEC_BOOTFLOW + 1] = {{0}};
    /* The writer's dependency order: clock before devices;
     * dirty bitmap after RAM's write/invalidation bookkeeping.
     * MODMEM follows SPAD, matching boot_state_save_to. */
    static const uint8_t order[] = {
        BS_SEC_CPU, BS_SEC_CPU_EXEC, BS_SEC_RAM, BS_SEC_SCHED, BS_SEC_BOOTFLOW, BS_SEC_SPAD, BS_SEC_MODMEM, BS_SEC_IRQ,
        BS_SEC_TIMER, BS_SEC_CLOCK, BS_SEC_GPU, BS_SEC_VRAM, BS_SEC_SPU,
        BS_SEC_SPURAM, BS_SEC_CDROM, BS_SEC_DMA, BS_SEC_SIO, BS_SEC_MDEC,
        BS_SEC_ICACHE, BS_SEC_DIRTY, BS_SEC_RASTER, BS_SEC_GPU_SERVICE,
        BS_SEC_TIMER_SRC, BS_SEC_DMA_SRC, BS_SEC_IRQ_TIMING
    };
    const uint32_t required =
        (1u<<BS_SEC_CPU)|(1u<<BS_SEC_RAM)|(1u<<BS_SEC_SPAD)|(1u<<BS_SEC_IRQ)|
        (1u<<BS_SEC_TIMER)|(1u<<BS_SEC_CLOCK)|(1u<<BS_SEC_GPU)|(1u<<BS_SEC_VRAM)|
        (1u<<BS_SEC_SPU)|(1u<<BS_SEC_SPURAM)|(1u<<BS_SEC_CDROM)|(1u<<BS_SEC_DMA)|
        (1u<<BS_SEC_SIO)|(1u<<BS_SEC_MDEC)|(1u<<BS_SEC_DIRTY)|
        (1u<<BS_SEC_SCHED)|(1u<<BS_SEC_ICACHE)|(1u<<BS_SEC_IRQ_TIMING)|(1u<<BS_SEC_CPU_EXEC)|
        (1u<<BS_SEC_BOOTFLOW)|
        (psx_mod_memory_snapshot_bytes() ? (1u<<BS_SEC_MODMEM) : 0u);
    uint32_t seen = 0, dirty_count = 0;
    uint32_t *dirty_words = NULL;
    uint16_t *native_vram = NULL;
    size_t prepared_bytes = 0;
    int ok = 0;
    const double t0 = boot_state_mono_ms();
    double inflate_ms = 0.0, apply_ram_ms = 0.0, apply_vram_ms = 0.0;
    double apply_spuram_ms = 0.0, apply_other_ms = 0.0;

    if (!cpu) return 0;
    if (!boot_state_check_buffer(file, file_len, bios_checksum, entry_pc,
                                 reject, sizeof(reject))) {
        fprintf(stderr, "boot_state: reject — %s\n", reject);
        return 0;
    }
    if (!boot_state_parse_header(file, file_len, &h)) return 0;
    cur = file + BOOT_STATE_HEADER_WIRE_BYTES;
    end = file + file_len;

    /* No guest or precision state changes in this pass. Retain inflated data
     * until commit so neither a late malformed section nor allocation failure
     * can strand the caller in a partially restored machine. */
    for (uint32_t i = 0; i < h.section_count; i++) {
        PstR sh;
        uint32_t tag, flags, raw_len;
        uint64_t len;
        const uint8_t *payload, *data;
        uint8_t *owned = NULL;
        if ((size_t)(end - cur) < 16u) goto cleanup;
        pst_r_init(&sh, cur, 16u);
        if (!pst_r_u32(&sh, &tag) || !pst_r_u32(&sh, &flags) ||
            !pst_r_u64(&sh, &len)) goto cleanup;
        cur += 16;
        if (len > 64u * 1024u * 1024u || (uint64_t)(end-cur) < len)
            goto cleanup;
        payload = cur;
        cur += (size_t)len;
        raw_len = (uint32_t)len;
        data = payload;
        if (flags == BOOT_STATE_SEC_ZLIB) {
            PstR lr;
            uLong dest_len;
            double t_inf;
            if (len < 4u) goto cleanup;
            pst_r_init(&lr, payload, 4u);
            if (!pst_r_u32(&lr, &raw_len) || !raw_len ||
                raw_len > 64u * 1024u * 1024u) goto cleanup;
            /* Bound simultaneous inflated storage, including unknown records. */
            if (prepared_bytes + raw_len > 64u * 1024u * 1024u) goto cleanup;
            owned = (uint8_t*)malloc(raw_len);
            if (!owned) goto cleanup;
            dest_len = raw_len;
            t_inf = boot_state_mono_ms();
            if (uncompress(owned, &dest_len, payload+4, (uLong)(len-4u)) != Z_OK ||
                dest_len != raw_len) {
                free(owned);
                goto cleanup;
            }
            inflate_ms += boot_state_mono_ms() - t_inf;
            data = owned;
        } else if (flags != 0u) goto cleanup;

        if (tag == 0u || tag > BS_SEC_BOOTFLOW) {
            free(owned); /* unknown sections remain forward-compatible */
            continue;
        }
        if ((seen & (1u << tag)) || !section_shape_ok(tag, raw_len)) {
            free(owned);
            goto cleanup;
        }
        sections[tag].data = data;
        sections[tag].owned = owned;
        sections[tag].len = raw_len;
        if (owned) prepared_bytes += raw_len;
        seen |= 1u << tag;
    }
    if ((seen & required) != required) goto cleanup;
    /* Amendment A: the three comparison-profile sections must be present
     * EXACTLY when their subsystem is active. Missing -> refuse (the blob could
     * not have been written by this profile). Present while inactive -> refuse
     * (the blob came from a different profile; restoring it is a mismatch).
     * Both directions fail loudly rather than restoring a half-configured
     * machine. */
    if (boot_state_raster_section_active() != ((seen >> BS_SEC_RASTER) & 1u) ||
        source_gpu_runtime_active()       != ((seen >> BS_SEC_GPU_SERVICE) & 1u) ||
        timers_source_active()            != ((seen >> BS_SEC_TIMER_SRC) & 1u) ||
        dma_src_active()                  != ((seen >> BS_SEC_DMA_SRC) & 1u)) {
        fprintf(stderr, "boot_state: reject — comparison-profile section "
                        "presence does not match the active profile "
                        "(raster=%u service=%u timers=%u dma_src=%u)\n",
                (unsigned)((seen >> BS_SEC_RASTER) & 1u),
                (unsigned)((seen >> BS_SEC_GPU_SERVICE) & 1u),
                (unsigned)((seen >> BS_SEC_TIMER_SRC) & 1u),
                (unsigned)((seen >> BS_SEC_DMA_SRC) & 1u));
        goto cleanup;
    }

    for (uint32_t tag = 1; tag <= BS_SEC_BOOTFLOW; tag++)
        if ((seen & (1u << tag)) && !section_content_ok(tag, &sections[tag], sections))
            goto cleanup;
    /* Convert the dirty bitmap before commit. The setter historically accepts
     * short/long bitmaps and consumes only its own word count; retain that. */
    dirty_count = sections[BS_SEC_DIRTY].len / 4u;
    if (dirty_count > dirty_ram_get_bitmap_word_count())
        dirty_count = dirty_ram_get_bitmap_word_count();
    dirty_words = (uint32_t*)malloc(dirty_count ? (size_t)dirty_count * 4u : 1u);
    if (!dirty_words) goto cleanup;
    {
        PstR r;
        pst_r_init(&r, sections[BS_SEC_DIRTY].data, sections[BS_SEC_DIRTY].len);
        for (uint32_t i = 0; i < dirty_count; i++)
            (void)pst_r_u32(&r, &dirty_words[i]);
    }
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
    native_vram = (uint16_t*)malloc(VRAM_SIZE);
    if (!native_vram) goto cleanup;
    {
        PstR r;
        pst_r_init(&r, sections[BS_SEC_VRAM].data, VRAM_SIZE);
        if (!pst_r_pod(&r, native_vram, VRAM_SIZE, 2)) goto cleanup;
    }
#endif
    if (!mdec_snapshot_prepare(sections[BS_SEC_MDEC].data,
                               sections[BS_SEC_MDEC].len)) goto cleanup;

    /* All rejection and fallible allocation paths are above this point. */
    for (size_t i = 0; i < sizeof(order); i++) {
        const uint32_t tag = order[i];
        const BsSection *sec = &sections[tag];
        double t_sec, dt;
        if (!(seen & (1u << tag))) continue;
        t_sec = boot_state_mono_ms();
        if (tag == BS_SEC_DIRTY) {
            dirty_ram_set_bitmap_words(dirty_words, dirty_count);
        } else if (tag == BS_SEC_VRAM && native_vram) {
            gr_vram_transfer_in(0, 0, VRAM_W, VRAM_H, native_vram);
            if (gpu_vram_dirty_tracking()) {
                memcpy(s_vram_mirror, native_vram, VRAM_SIZE);
                s_vram_mirror_valid = 1;
                gpu_vram_dirty_clear();
            } else s_vram_mirror_valid = 0;
        } else {
            /* Readers cannot fail after shape, content and allocation admission. */
            (void)apply_section(tag, sec->data, sec->len, cpu, entry_pc);
        }
        dt = boot_state_mono_ms() - t_sec;
        if (tag == BS_SEC_RAM) apply_ram_ms += dt;
        else if (tag == BS_SEC_VRAM) apply_vram_ms += dt;
        else if (tag == BS_SEC_SPURAM) apply_spuram_ms += dt;
        else apply_other_ms += dt;
    }
    /* Amendment C: return_clock/return_command are assigned once per frame
     * boundary on the normal path, so right after a restore they would be one
     * frame stale. A bit-exact replay cannot afford a frame of stale reads.
     * Re-derive both once, here, after every section has loaded. */
    source_gpu_runtime_rederive_returns();

    /* Reshaped #5: the VBlank phase was staged while BS_SEC_IRQ applied, so
     * commit it now that BS_SEC_RASTER's presence is known. The guard inside
     * interrupts_set_cycles_since_vblank refuses a comparison-profile restore
     * that arrived without the raster section. */
    interrupts_note_state_load((int)((seen >> BS_SEC_RASTER) & 1u));
    if (s_pending_vblank_phase_valid) {
        interrupts_set_cycles_since_vblank(s_pending_vblank_phase);
        s_pending_vblank_phase_valid = 0;
    }

    /* RAM was memcpy'd; force overlay revalidation before resume. */
    overlay_watch_invalidate_after_ram_restore();
    fntrace_restore_game_started(s_pending_game_started);
    /* Every device changed at once: the deadline caches keyed on this
     * generation must not reuse a countdown computed before the load. */
    { extern uint64_t g_psx_device_gen; g_psx_device_gen++; }

    ok = 1;
    fprintf(stderr,
            "savestate: load_timing read=0.0 inflate=%.1f "
            "apply_ram=%.1f apply_vram=%.1f apply_spuram=%.1f "
            "apply_other=%.1f total=%.1f ms (file=%zu)\n",
            inflate_ms, apply_ram_ms, apply_vram_ms, apply_spuram_ms,
            apply_other_ms, boot_state_mono_ms() - t0, file_len);
cleanup:
    if (!ok) fprintf(stderr, "boot_state: reject - incomplete, duplicate, malformed or unallocatable state (nothing applied)\n");
    for (uint32_t tag = 1; tag <= BS_SEC_BOOTFLOW; tag++) free(sections[tag].owned);
    free(dirty_words);
    free(native_vram);
    return ok;
}

int boot_state_load(const char* path, uint32_t bios_checksum,
                    uint32_t entry_pc, CPUState* cpu) {
    FILE* f = fopen(path, "rb");
    long sz;
    uint8_t* file = NULL;
    size_t file_len = 0;
    int ok;
    const double t0 = boot_state_mono_ms();
    double t_after_read;

    if (!f) {
        fprintf(stderr, "boot_state: reject — missing %s\n",
                path ? path : "(null)");
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz < (long)BOOT_STATE_HEADER_WIRE_BYTES || sz > 64L * 1024L * 1024L) {
        fprintf(stderr, "boot_state: reject — bad size %ld for %s\n",
                sz, path ? path : "(null)");
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    file_len = (size_t)sz;
    file = (uint8_t*)malloc(file_len);
    if (!file) { fclose(f); return 0; }
    if (fread(file, 1, file_len, f) != file_len) {
        free(file);
        fclose(f);
        return 0;
    }
    fclose(f);
    t_after_read = boot_state_mono_ms();
    (void)t0;
    (void)t_after_read;

    ok = boot_state_load_buffer(file, file_len, bios_checksum, entry_pc, cpu);
    free(file);
    return ok;
}

void boot_state_set_capture(const char* path, uint32_t bios_checksum,
                            uint32_t entry_pc) {
    strncpy(s_capture_path, path, sizeof(s_capture_path) - 1);
    s_capture_path[sizeof(s_capture_path) - 1] = '\0';
    s_capture_checksum = bios_checksum;
    s_capture_entry_pc = entry_pc;
}

void boot_state_trigger_capture(const CPUState* cpu) {
    if (!s_capture_path[0]) return;
    boot_state_save(cpu, s_capture_checksum, s_capture_entry_pc, s_capture_path);
    s_capture_path[0] = '\0';
}
