/* Resident control-flow-patch ownership harness.
 *
 * Links the real overlay loader and the real memory.c store path / text guard
 * against a shard that tools/compile_overlays.py built from the SF2 17e9bbaa0
 * fixture: resident boot text at 0x80010000 whose JALR at 0x80010008 is
 * patched to NOP. Device and MMIO paths are not reachable from these stores
 * and abort if touched.
 *
 * Every step prints one line; main() returns non-zero on the first ownership
 * that differs from the expected table.
 */
#define PSX_OVERLAY_DLL_BUILD 1
#include "overlay_loader.h"
#undef PSX_OVERLAY_DLL_BUILD

#include "card_data_writes.h"
#include "cdrom.h"
#include "crash_trace.h"
#include "data_shards.h"
#include "debug_server.h"
#include "dma.h"
#include "fntrace.h"
#include "gpu.h"
#include "kernel_patch_ranges.h"
#include "lockstep.h"
#include "mdec.h"
#include "parity_trace.h"
#include "psx_cycles.h"
#include "dirty_ram_interp.h"
#include "psx_bios_image.h"
#include "psx_icache.h"
#include "sio.h"
#include "source_gpu_runtime.h"
#include "spu.h"
#include "timers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JALR_T1_T0 0x01204009u
#define NOP 0x00000000u
#define RA_SENTINEL 0x80020000u

static void unreachable(const char *what) {
    fprintf(stderr, "unexpected device path: %s\n", what);
    abort();
}

/* ---- memory.c environment ---------------------------------------------- */
uint32_t g_overlay_region_floor = 0x00010800u;
uint32_t g_text_image_lo = 0x00010000u;
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS];
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
int g_dma_exec_depth;
int g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr, g_dma_cur_bcr, g_dma_initiator_pc;
volatile int g_ds_recording;
int g_ls_mode, g_ls_replay_active, g_ls_suppress_record;
int g_ram_read_watch_active;
CPUState *debug_cpu_ptr;
PsxBiosImageInfo psx_bios_image;
const PsxKernelBody *psx_bios_kernel_bodies;
uint32_t psx_bios_kernel_body_count;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges;
uint32_t psx_bios_kernel_patch_range_count;

int psx_kernel_patch_cmp(const PsxKernelPatchRange *ranges, uint32_t n,
                         const uint8_t *ram, const uint8_t *rom,
                         uint32_t ram_lo, uint32_t rom_off,
                         uint32_t lo, uint32_t hi, uint64_t *skips_out) {
    (void)ranges; (void)n; (void)ram; (void)rom; (void)ram_lo; (void)rom_off;
    (void)lo; (void)hi; (void)skips_out;
    unreachable("kernel patch compare");
    return 0;
}
int psx_kernel_patch_ends_at(const PsxKernelPatchRange *ranges, uint32_t n,
                             uint32_t phys) {
    (void)ranges; (void)n; (void)phys; return 0;
}
int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width) {
    (void)phys; (void)value; (void)width; return 0;
}
void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr; (void)val; (void)width;
}
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr; (void)val; (void)width;
}
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val) {
    (void)phys; (void)val;
}
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width) {
    (void)phys; (void)old_val; (void)new_val; (void)width;
}
void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}
int fntrace_is_game_started(void) { return 1; }
int source_gpu_runtime_active(void) { return 0; }
void ds_note_dma_write(void) {}
void ds_note_read(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
void ds_note_write(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
uint32_t ls_read_hook(uint32_t addr, int size, uint32_t real_val) {
    (void)addr; (void)size; return real_val;
}
void ls_write_hook(uint32_t addr, int size, uint32_t val) {
    (void)addr; (void)size; (void)val;
}
void psx_icache_isolated_store(uint32_t addr, uint32_t cache_control) {
    (void)addr; (void)cache_control; unreachable("isolated cache store");
}
void psx_irq_refresh_cause_ip2(void) {}
void psx_devices_mmio_sync(void) { unreachable("mmio sync"); }
uint32_t cdrom_read(uint32_t a) { (void)a; unreachable("cdrom"); return 0; }
void cdrom_write(uint32_t a, uint32_t v) { (void)a; (void)v; unreachable("cdrom"); }
uint32_t dma_read(uint32_t a) { (void)a; unreachable("dma"); return 0; }
void dma_write(uint32_t a, uint32_t v) { (void)a; (void)v; unreachable("dma"); }
void dma_write_masked(uint32_t a, uint32_t v, uint32_t m) {
    (void)a; (void)v; (void)m; unreachable("dma");
}
uint32_t gpu_read_gpuread(void) { unreachable("gpu"); return 0; }
uint32_t gpu_read_gpustat(void) { unreachable("gpu"); return 0; }
void gpu_set_gp0_source(uint32_t a) { (void)a; }
void gpu_write_gp0(uint32_t v) { (void)v; unreachable("gpu"); }
void gpu_write_gp1(uint32_t v) { (void)v; unreachable("gpu"); }
uint32_t mdec_read(uint32_t a) { (void)a; unreachable("mdec"); return 0; }
void mdec_write(uint32_t a, uint32_t v) { (void)a; (void)v; unreachable("mdec"); }
void sio_card_handoff_on_imask(uint32_t o, uint32_t n) { (void)o; (void)n; }
uint32_t sio_read(uint32_t a) { (void)a; unreachable("sio"); return 0; }
void sio_tick(int c) { (void)c; }
void sio_write(uint32_t a, uint32_t v) { (void)a; (void)v; unreachable("sio"); }
uint32_t spu_read(uint32_t a) { (void)a; unreachable("spu"); return 0; }
void spu_write(uint32_t a, uint32_t v) { (void)a; (void)v; unreachable("spu"); }
uint32_t timers_read(uint32_t a) { (void)a; unreachable("timers"); return 0; }
void timers_write(uint32_t a, uint32_t v) { (void)a; (void)v; unreachable("timers"); }
int timers_source_hblank_counter_read(uint32_t a) { (void)a; return 0; }
int timers_source_raster_enabled(void) { return 0; }

/* ---- overlay_loader.c environment (as overlay_pair_dedup_harness.c) ----- */
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
int g_exec_phase;
int g_idle_note_suppress;
int g_psx_call_bail;
uint64_t g_psx_bail_first, g_psx_bail_resolved;
uint64_t s_frame_count;

void ds_init(const char *cache_dir, const char *game_id) {
    (void)cache_dir; (void)game_id;
}
int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop) {
    (void)cpu; (void)addr; (void)stop; return 0;
}
void dirty_ram_xprobe_call_note(CPUState *cpu, uint32_t target,
                                uint32_t ra, uint8_t phase) {
    (void)cpu; (void)target; (void)ra; (void)phase;
}
void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ra) {
    (void)cpu; (void)addr; (void)ra;
}
void psx_check_interrupts(CPUState *cpu) { (void)cpu; }
void psx_check_interrupts_at(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
int psx_interrupt_delivery_needed(const CPUState *cpu) { (void)cpu; return 0; }
int psx_get_in_exception(void) { return 0; }
uint64_t psx_exception_setjmp_epoch(void) { return 0; }
void psx_restore_state_escape(void) {}
void psx_rfe_mark_escape(void) {}
int psx_syscall(CPUState *cpu, uint32_t code) { (void)cpu; (void)code; return 0; }
void psx_unknown_dispatch(CPUState *cpu, uint32_t addr, uint32_t phys) {
    (void)cpu; (void)addr; (void)phys;
}
void psx_advance_cycles(uint32_t cycles) { (void)cycles; }
uint64_t psx_get_cycle_count(void) { return 0; }
int psx_cycle_replay_begin(uint64_t cycle) { (void)cycle; return 1; }
uint64_t psx_cycle_replay_end(void) { return 0; }
uint32_t psx_cyc_load_word(CPUState *cpu, uint32_t addr, uint32_t rt,
                           uint32_t mask) {
    (void)cpu; (void)addr; (void)rt; (void)mask; return 0;
}
uint16_t psx_cyc_load_half(CPUState *cpu, uint32_t addr, uint32_t rt,
                           uint32_t mask) {
    (void)cpu; (void)addr; (void)rt; (void)mask; return 0;
}
int psx_icache_shadow_record_begin(void) { return 1; }
int psx_icache_shadow_replay_begin(void) { return 1; }
void psx_icache_shadow_replay_end(void) {}
void psx_icache_shadow_abort(void) {}
void psx_cpu_step_boundary_fn(CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
}
int psx_cpu_step_boundary_enabled(int include_replay) {
    (void)include_replay; return 0;
}
void psx_fatal_halt(const char *reason) {
    fprintf(stderr, "psx_fatal_halt: %s\n", reason);
    abort();
}
void psx_icache_fetch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
void psx_icache_fetch_fn(CPUState *cpu, uint32_t addr) { psx_icache_fetch(cpu, addr); }
void psx_muldiv_set(CPUState *cpu, uint32_t latency) { (void)cpu; (void)latency; }
void psx_muldiv_stall(CPUState *cpu) { (void)cpu; }
uint32_t psx_mult_latency_s(uint32_t rs) { (void)rs; return 1; }
uint32_t psx_mult_latency_u(uint32_t rs) { (void)rs; return 1; }
void psx_gte_stall(CPUState *cpu) { (void)cpu; }
void psx_gte_read(CPUState *cpu, uint32_t rt) { (void)cpu; (void)rt; }
int psx_slice_block(CPUState *cpu, uint32_t addr, uint32_t cycles,
                    int side_effects) {
    (void)cpu; (void)addr; (void)cycles; (void)side_effects; return 0;
}
int psx_slice_block_impl(CPUState *cpu, uint32_t addr, uint32_t cycles,
                         int side_effects) {
    return psx_slice_block(cpu, addr, cycles, side_effects);
}
void gte_execute(CPUState *cpu, uint32_t cmd) { (void)cpu; (void)cmd; }
uint32_t gte_read_data(CPUState *cpu, uint8_t reg) { (void)cpu; (void)reg; return 0; }
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t reg) { (void)cpu; (void)reg; return 0; }
void gte_write_data(CPUState *cpu, uint8_t reg, uint32_t val) {
    (void)cpu; (void)reg; (void)val;
}
void gte_write_ctrl(CPUState *cpu, uint8_t reg, uint32_t val) {
    (void)cpu; (void)reg; (void)val;
}
int32_t psx_ws_plane_nx(int32_t nx) { return nx; }
uint32_t psx_ws_xclip_bound(uint32_t vanilla) { return vanilla; }
void gte_precision_store_word(uint32_t addr, uint8_t reg) { (void)addr; (void)reg; }
void gte_precision_speculative_begin(void) {}
void gte_precision_speculative_end(void) {}
int gte_replay_side_effects_begin(void) { return 1; }
void gte_replay_side_effects_end(void) {}
int ls_shadow_record_begin(void) { return 1; }
int ls_shadow_record_end(uint32_t *ops, int *exc) {
    if (ops) *ops = 0;
    if (exc) *exc = 0;
    return 1;
}
int ls_shadow_replay_begin(void) { return 1; }
int ls_shadow_replay_end(uint32_t *ops, int *kind, uint32_t *pc,
                         uint32_t *addr, uint32_t *expected,
                         uint32_t *actual) {
    (void)ops; (void)kind; (void)pc; (void)addr; (void)expected; (void)actual;
    return 1;
}
void ls_shadow_abort(void) {}
int psx_ws_backdrop_x(int x) { return x; }
int psx_ws_x_margin(void) { return 0; }
void psx_ws_sprite_tag(CPUState *cpu) { (void)cpu; }
uint32_t psx_ws_backdrop_value(uint32_t orig, int end, int cols) {
    (void)end; (void)cols; return orig;
}
int32_t psx_ws_depth_bound(int32_t imm) { return imm; }
int32_t psx_ws_player_x_bound(int32_t vanilla) { return vanilla; }
int32_t psx_ws_screen_x_bound(int32_t vanilla) { return vanilla; }
void psx_mod_function_entry(CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
}
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_ws_angle_widen(uint32_t vanilla) { return vanilla; }
uint32_t psx_ws_cull_keep_result(uint32_t vanilla, uint32_t forced) {
    (void)forced; return vanilla;
}
uint32_t psx_ws_aspect_cone_result(uint32_t site, uint32_t vanilla,
                                  uint32_t object, int32_t x, int32_t z, int32_t y) {
    (void)site; (void)object; (void)x; (void)z; (void)y; return vanilla;
}
void psx_pgxp_load(CPUState *cpu, uint32_t i, uint32_t a, uint32_t v) {
    (void)cpu; (void)i; (void)a; (void)v;
}
void psx_pgxp_store(CPUState *cpu, uint32_t i, uint32_t a, uint32_t v) {
    (void)cpu; (void)i; (void)a; (void)v;
}
void psx_pgxp_alu(CPUState *cpu, uint32_t i, uint32_t r, uint32_t s, uint32_t t) {
    (void)cpu; (void)i; (void)r; (void)s; (void)t;
}
void psx_pgxp_muldiv(CPUState *cpu, uint32_t i, uint32_t h, uint32_t l,
                     uint32_t s, uint32_t t) {
    (void)cpu; (void)i; (void)h; (void)l; (void)s; (void)t;
}
void psx_pgxp_cop2(CPUState *cpu, uint32_t i, uint32_t v, uint32_t a) {
    (void)cpu; (void)i; (void)v; (void)a;
}

/* ---- The static resident owner, as main_psx.cpp emits it --------------- */
/* Resident function 0x80010008 (JALR t1 / addiu v0 / jr ra / delay) and its
 * post-call continuation 0x80010010 share one exact CFG range. */
static const uint32_t k_resident_ranges[] = { 0x80010008u, 16u };

int psx_game_text_native_ok(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys != 0x00010008u && phys != 0x00010010u) return 0;
    return dirty_ram_text_native_ok_ranges_from(k_resident_ranges, 1u, addr);
}

/* ---- Scenario ------------------------------------------------------------ */
extern int g_psx_cps_mode;
extern uint8_t *memory_get_ram_ptr(void);
extern void overlay_loader_bad_entry_stats(uint32_t *owner, uint32_t *pc,
                                           uint64_t *count);
extern void dirty_ram_register_text_image(uint32_t phys_lo, const uint8_t *bytes,
                                          uint32_t len);
extern void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);
extern int overlay_loader_dispatch(CPUState *cpu, uint32_t addr);
extern void psx_write_word(uint32_t addr, uint32_t val);

static uint8_t s_ref[0x800];
static int s_failures;

typedef struct {
    int static_ok;
    int window;
    int native;
    int bad_entry;
} Ownership;

static Ownership observe(uint32_t pc) {
    Ownership o;
    uint64_t bad_before = 0, bad_after = 0;
    uint32_t owner = 0, bad_pc = 0;
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[2] = 0xDEADu;
    cpu.gpr[9] = 0x80030000u;
    cpu.gpr[31] = RA_SENTINEL;
    o.static_ok = psx_game_text_native_ok(pc);
    o.window = overlay_cache_window_contains(pc & 0x1FFFFFFFu);
    overlay_loader_bad_entry_stats(&owner, &bad_pc, &bad_before);
    o.native = overlay_loader_dispatch(&cpu, pc);
    overlay_loader_bad_entry_stats(&owner, &bad_pc, &bad_after);
    o.bad_entry = bad_after != bad_before;
    if (o.native && (cpu.pc != RA_SENTINEL || cpu.gpr[2] != 1u)) {
        fprintf(stderr, "native run at 0x%08X returned pc=0x%08X v0=0x%X\n",
                pc, cpu.pc, cpu.gpr[2]);
        s_failures++;
    }
    return o;
}

static void step(const char *name, uint32_t pc, Ownership want) {
    uint32_t word = memory_get_ram_ptr()[0x10008] |
                    (uint32_t)memory_get_ram_ptr()[0x10009] << 8 |
                    (uint32_t)memory_get_ram_ptr()[0x1000A] << 16 |
                    (uint32_t)memory_get_ram_ptr()[0x1000B] << 24;
    Ownership got = observe(pc);
    int ok = got.static_ok == want.static_ok && got.window == want.window &&
             got.native == want.native && got.bad_entry == want.bad_entry;
    printf("%-34s pc=%08X word@10008=%08X static_native_ok=%d window=%d "
           "shard_native=%d foreign_interior=%d registered=%d %s\n",
           name, pc, word, got.static_ok, got.window, got.native,
           got.bad_entry, overlay_loader_registered_count(),
           ok ? "ok" : "UNEXPECTED");
    if (!ok) s_failures++;
}

static void load_resident_image(void) {
    static const uint32_t words[] = {
        0x27BDFFF0u, 0x11111111u, JALR_T1_T0, 0x24020001u, 0x03E00008u, NOP,
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        s_ref[i * 4] = (uint8_t)words[i];
        s_ref[i * 4 + 1] = (uint8_t)(words[i] >> 8);
        s_ref[i * 4 + 2] = (uint8_t)(words[i] >> 16);
        s_ref[i * 4 + 3] = (uint8_t)(words[i] >> 24);
    }
    memcpy(memory_get_ram_ptr() + 0x10000u, s_ref, sizeof(s_ref));
    dirty_ram_register_text_image(0x10000u, s_ref, sizeof(s_ref));
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <cache-root> <game-id> <config-hash>\n", argv[0]);
        return 2;
    }
    uint32_t config_hash = (uint32_t)strtoul(argv[3], NULL, 16);
    load_resident_image();
    overlay_loader_init(argv[1], argv[2], config_hash);
    printf("loader: lazy_manifests=%d msg=%s\n",
           overlay_loader_lazy_manifest_count(), overlay_loader_last_msg());
    if (overlay_loader_lazy_manifest_count() < 1) {
        fprintf(stderr, "the patched shard was not indexed\n");
        return 1;
    }

    /* 1. Pristine resident text: static code owns; the patched shard cannot. */
    step("pristine", 0x80010008u, (Ownership){1, 0, 0, 0});

    /* 2. Guest CPU store JALR->NOP (the game patching its own text). */
    psx_write_word(0x80010008u, NOP);
    step("cpu-store patch: entry", 0x80010008u, (Ownership){0, 0, 0, 0});
    step("cpu-store patch: old call return", 0x80010010u, (Ownership){0, 0, 0, 0});

    /* 3. The same bytes delivered through a load path that marks the page
     *    executable (CD DMA / data-shard publication). */
    dirty_ram_mark_executable_range(0x10000u, 0x18u);
    step("loaded patch: entry", 0x80010008u, (Ownership){0, 1, 1, 0});
    g_psx_cps_mode = 1;
    step("loaded patch: old call return", 0x80010010u, (Ownership){0, 1, 0, 1});
    g_psx_cps_mode = 0;

    /* 4. Guest restores the JALR: static owns again, the shard must not run. */
    psx_write_word(0x80010008u, JALR_T1_T0);
    step("restored: entry", 0x80010008u, (Ownership){1, 1, 0, 0});

    /* 5. Patch again after restore: the shard revalidates on exact bytes. */
    psx_write_word(0x80010008u, NOP);
    step("re-patched: entry", 0x80010008u, (Ownership){0, 1, 1, 0});

    if (s_failures) {
        fprintf(stderr, "FAIL: %d ownership mismatch(es); loader: %s\n",
                s_failures, overlay_loader_last_msg());
        return 1;
    }
    printf("PASS: resident control-flow patch ownership\n");
    return 0;
}
