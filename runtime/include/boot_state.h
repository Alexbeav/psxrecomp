#ifndef PSX_BOOT_STATE_H
#define PSX_BOOT_STATE_H

#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Boot snapshot (a.k.a. fast_boot) — a COMPLETE post-BIOS save-state.
 *
 * Model: first launch (and the first launch after ANY app/recompiler update)
 * runs the real recompiled BIOS normally, logos and all. At the moment the BIOS
 * dispatches into the game's PS-EXE entry_pc, we capture a complete hardware
 * snapshot. Every subsequent launch (same build) restores that snapshot and
 * presents the game's first frame — instant boot, no BIOS, no logos.
 *
 * This is NOT HLE: it persists the REAL hardware state produced by a real BIOS
 * run, then replays it. Nothing about BIOS behaviour is synthesized.
 *
 * Rebuild-proof: the file carries an integrity key (below) that includes the
 * codegen hash + ABI tag + codegen version. A user update changes those, so a
 * stale snapshot can NEVER silently load into a new build — it is rejected and
 * the next boot is a normal boot that recaptures. Graceful, automatic.
 *
 * Completeness is mandatory (v4 no-stub rule): a partial capture that leaves a
 * subsystem at reset while CPU/RAM assume it was configured is a latent stub.
 * Every mutable hardware subsystem gets a section here, and the capture is
 * proven complete by diffing a restored session's frames against a normal-boot
 * session's frames (see the "bootsnap" debug command). Host-side / recompiler-
 * derived state (dirty-RAM bitmap, overlay tables, debug rings) is NOT
 * serialized — it is re-derived from restored guest RAM on load.
 */

#define BOOT_STATE_MAGIC   0x50535842u  /* "PSXB" */
/* v1 = incomplete RAM-only; v2 = full machine but host-struct memcpy (padding);
 * v3 = little-endian field wire (portable Win/Linux/macOS ARM);
 * v4 = v3 + optional zlib on large sections (section pad bit0 = compressed);
 * v5 = v4 + CD-ROM Sub-Q replacement state;
 * v6 = v5 + deterministic scheduler continuation state;
 * v7 = v6 + comparison-profile state: raster clocks (3 instances), source GPU
 *      service, and source timer state;
 * v8 = v7 + CPU timing/instruction continuation, SPU sample clock and exact
 *      MDEC timestamp. */
#define BOOT_STATE_VERSION 8u
/* The version field is the ONLY guard against a blob written by an older
 * RUNTIME: codegen_hash / abi_tag / codegen_ver are keyed to codegen and ABI,
 * so a runtime-only change (new sections, changed snapshot writers) leaves all
 * three unchanged. A pin bump without a code regen would otherwise hand an old
 * runtime's blob to a new loader. v8 therefore rejects every earlier state. */
#define BOOT_STATE_VERSION_MIN_READ 8u
/* Section pad bit0: payload is u32 LE uncompressed_len + zlib deflate bytes. */
#define BOOT_STATE_SEC_ZLIB 1u

/*
 * On-disk header (v3): nine little-endian uint32 fields at offset 0 (36 bytes),
 * followed by the section stream. ALL key fields must match the running build
 * or the snapshot is rejected. Do not fwrite() this struct — use pst_wire.
 */
typedef struct {
    uint32_t magic;          /* BOOT_STATE_MAGIC                                  */
    uint32_t version;        /* BOOT_STATE_VERSION                                */
    /* ---- integrity key (every field must match to accept) ---- */
    uint32_t bios_checksum;  /* sum of all uint32 words in the BIOS ROM           */
    uint32_t entry_pc;       /* game PS-EXE entry PC                              */
    uint32_t codegen_hash;   /* PSX_OVERLAY_CODEGEN_HASH (auto-gen by cmake)      */
    int32_t  abi_tag;        /* PSX_OVERLAY_ABI_TAG (abi version | flavor<<16)    */
    uint32_t codegen_ver;    /* PSX_OVERLAY_CODEGEN_VER                           */
    /* ---- layout ---- */
    uint32_t section_count;  /* number of sections that follow                    */
    uint32_t reserved;       /* 0                                                 */
} BootStateHeader;

#define BOOT_STATE_HEADER_WIRE_BYTES 36u

/*
 * Section stream (v3/v4): section_count records, each laid out as
 *     uint32_t tag;        LE (one of BS_SEC_*)
 *     uint32_t pad;        LE flags (v3: 0; v4: BOOT_STATE_SEC_ZLIB optional)
 *     uint64_t len;        LE payload byte count
 *     uint8_t  payload[len];   (module payloads are LE field wires too)
 * When BOOT_STATE_SEC_ZLIB is set, payload = u32 LE raw_len + zlib(raw).
 * Unknown tags are skipped for forward compatibility. A malformed known section
 * or a missing required section on load is a hard reject (incomplete restore is
 * never allowed) -> normal boot + recapture.
 */
enum {
    BS_SEC_CPU_EXEC = 0x17, /* interpreter instruction/branch/load continuation */
    BS_SEC_CPU    = 0x01,  /* CPU registers, completion deadlines and load timing */
    BS_SEC_RAM    = 0x02,  /* 2 MB main RAM                                       */
    BS_SEC_SPAD   = 0x03,  /* 1 KB scratchpad                                     */
    BS_SEC_IRQ    = 0x04,  /* i_stat / i_mask / cycles_since_vblank (12B; 8B ok)  */
    BS_SEC_TIMER  = 0x05,  /* 3 root counters (counter/mode/target/irq/frac)      */
    BS_SEC_CLOCK  = 0x06,  /* psx_cycle_count                                     */
    BS_SEC_GPU    = 0x07,  /* GPU regs: display/draw-area/offset/mask/texpage/xfer*/
    BS_SEC_VRAM   = 0x08,  /* 1 MB VRAM (1024x512x16)                             */
    BS_SEC_SPU    = 0x09,  /* SPU regs + 24 voice decode/ADSR state + latches     */
    BS_SEC_SPURAM = 0x0A,  /* 512 KB SPU RAM                                      */
    BS_SEC_CDROM  = 0x0B,  /* CD-ROM controller FSM (regs/FIFOs/seek/read/pending)*/
    BS_SEC_DMA    = 0x0C,  /* DMA channels[7] + dpcr/dicr + async-transfer state  */
    BS_SEC_SIO    = 0x0D,  /* SIO regs + pad-config FSM + memcard FSM             */
    BS_SEC_DIRTY  = 0x0E,  /* dirty-RAM page bitmap (guest-written code pages)    */
    BS_SEC_MDEC   = 0x0F,  /* MDEC command/FIFOs/quant/scale (FMV decode resume)  */
    BS_SEC_ICACHE = 0x10,  /* R3000A I-cache tag/valid words (1024 u32) — fetch
                              cost model. Host-persistent otherwise: a warm load
                              without it replays with the pre-load timeline's
                              cache, so fetch-miss cycles differ per peer/retry
                              and IRQ delivery lands a few wait-loop iterations
                              apart (MotK abort@940: fin cyc Δ8, v0 5c83/5c86
                              from identical baselines). Optional on load for
                              old blobs (left untouched when absent).          */
    BS_SEC_SCHED  = 0x11,  /* deterministic scheduler return continuation       */
    BS_SEC_RASTER = 0x12,  /* comparison raster clocks, 3 instances x 80 B:
                              [0]   input_route_raster (interrupts.c)
                              [80]  clock_state.raster  (source_gpu_runtime.c)
                              [160] draw_raster         (source_gpu_runtime.c)
                              These `cycle`/`last_rise` values are INTERNAL
                              counters, not psx_cycle_count's time base — never
                              rebase them; the fraction cross-check depends on
                              that time base.                                   */
    BS_SEC_GPU_SERVICE = 0x13, /* bounded-quad source GPU service: the service
                              clock scalars + the command-projection scalar
                              tail. queue[32] is NOT serialized: save AND load
                              both require count==0, since a non-zero count
                              with no queue would restore as a stub.           */
    BS_SEC_TIMER_SRC = 0x14, /* source timer state machines (the IRQ-disabled
                              comparison timers).                               */
    BS_SEC_DMA_SRC   = 0x15, /* source-DMA state machines: bounded-quad GPU
                              upload, GPU linked list, SPU request, OTC (140 B).
                              Measured live at 98.1%/98.4%/57.7% of frame
                              boundaries, so they cannot be quiescence-guarded;
                              every member is a flat scalar, and the address
                              fields are GUEST physical addresses, never host
                              pointers. Required exactly when a source DMA model
                              is active (profile-exact, like RASTER).           */
    BS_SEC_IRQ_TIMING = 0x16, /* field clock + VBlank-edge/IRQ-deferral state
                              (64 B). Found by the step-8 forward sweep: live in
                              every profile and previously covered by NO section.
                              Always required. input_route_raster_deadline is
                              derived and recomputed at load, not serialized.    */
};

/* Save a COMPLETE snapshot at game handoff. Returns 1 on success. */
int  boot_state_save(const CPUState* cpu, uint32_t bios_checksum,
                     uint32_t entry_pc, const char* path);

/* Atomic replace of `to` by `from` (Windows: MoveFileExW REPLACE_EXISTING).
 * Exposed so the overwrite/refusal behaviour can be regression-tested. */
int  boot_state_replace_file(const char* from, const char* to);

/* Same as boot_state_save, but into a malloc'd buffer (caller frees *out_data).
 * Compresses large sections (disk-oriented). */
int  boot_state_save_buffer(const CPUState* cpu, uint32_t bios_checksum,
                            uint32_t entry_pc, uint8_t** out_data,
                            size_t* out_len);

/* In-memory ring snaps: same sections, no zlib. Load accepts either form.
 * Avoids compress2 on ~3.5 MiB RAM+VRAM+SPU every live/resim snap (FPS). */
int  boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios_checksum,
                                uint32_t entry_pc, uint8_t** out_data,
                                size_t* out_len);

/* §96 telemetry: after the latest save, how many VRAM scanlines were dirty
 * and whether the incremental mirror path patched (vs full memcpy). */
uint32_t boot_state_last_vram_dirty_rows(void);
int      boot_state_last_vram_incremental(void);

/* Drop the §96 VRAM mirror (RB shutdown / before re-enable). */
void boot_state_vram_mirror_reset(void);

/* Load + validate (integrity key) + restore the full machine. On any mismatch
 * or incompleteness returns 0 (caller then boots normally and recaptures). */
int  boot_state_load(const char* path, uint32_t bios_checksum,
                     uint32_t entry_pc, CPUState* cpu);

/* Read only the serialized CPU resume PC and stack pointer. No machine state
 * is applied. Disk-state admission uses both to reject host-only BIOS frames. */
int  boot_state_peek_cpu_context(const char* path, uint32_t* out_pc,
                                 uint32_t* out_sp, uint32_t* out_ra);
int  boot_state_peek_cpu_context_buffer(const uint8_t* file, size_t file_len,
                                        uint32_t* out_pc, uint32_t* out_sp,
                                        uint32_t* out_ra);

/* Same as boot_state_load, but from an already-buffered .pst image (netplay). */
int  boot_state_load_buffer(const uint8_t* file, size_t file_len,
                            uint32_t bios_checksum, uint32_t entry_pc,
                            CPUState* cpu);

/* Header-only integrity check (no section inflate/apply). Returns 1 if this
 * build can load the image; 0 and fills reason (when non-NULL) on reject. */
int  boot_state_check_buffer(const uint8_t* file, size_t file_len,
                             uint32_t bios_checksum, uint32_t entry_pc,
                             char* reason, size_t reason_cap);

/* Register a deferred capture: when boot_state_trigger_capture() fires (from
 * fntrace at game-start), serialize to path. One-shot. */
void boot_state_set_capture(const char* path, uint32_t bios_checksum,
                             uint32_t entry_pc);

/* Called from fntrace when the game entry PC first dispatches. */
void boot_state_trigger_capture(const CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BOOT_STATE_H */
