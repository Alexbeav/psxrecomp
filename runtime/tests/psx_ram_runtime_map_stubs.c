/* Link doubles for psx_ram_runtime_map_test: everything the REAL memory.c,
 * dma.c and boot_state.c call outside themselves. RAM, the DMA engine and the
 * savestate format are the code under test and are never stubbed. Device
 * snapshot readers count their calls in g_stub_device_restores, which is how
 * the test proves a refused load mutated no device. Section GC (the target is
 * built with -ffunction-sections/--gc-sections like mod_memory_snapshot_test)
 * drops whatever the test never reaches. */
#include "cpu_state.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int g_stub_device_restores;

/* ---- data ---------------------------------------------------------------- */
CPUState *debug_cpu_ptr;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
volatile int g_ds_recording;
int g_event_step_conservative;
int g_ls_mode;
int g_ls_replay_active;
int g_ls_suppress_record;
int g_ram_read_watch_active;
void (*g_overlay_flush_pending_cycles)(void);
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
#ifndef PSX_STUBS_REAL_PGXP
/* memory.c drops the PGXP shadow of a DMA / host store (pgxp.cpp).
 * pgxp_untracked_store_test links the real engine. */
void pgxp_invalidate_word(uint32_t addr) { (void)addr; }
#endif
#ifndef PSX_STUBS_REAL_ICACHE
/* The I-cache model (psx_icache.c). isc_store_test links the real one. */
#include "../src/psx_icache.c"

#endif
uint64_t psx_cycle_count;
uint64_t s_frame_count;
uint64_t psx_next_service_cycle;
int psx_in_device_service;
uint32_t g_dirty_ram_dispatch_pc_bitmap[(0x00800000u / 4u + 31u) / 32u];
uint32_t g_dirty_ram_dispatch_seg_bitmap[3][(0x00800000u / 4u + 31u) / 32u];
uint32_t g_dirty_ram_exec_pc_bitmap[(0x00800000u / 4u + 31u) / 32u];
uint32_t g_dirty_ram_exec_page_bitmap[((0x00800000u / 4096u) + 31u) / 32u];

/* ---- hooks the RAM paths notify (no behaviour under test) ------------------ */
void debug_server_trace_mmio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_ram_read_watch(uint32_t p, uint32_t v) { (void)p; (void)v; }
void debug_server_trace_write_check(uint32_t p, uint32_t o, uint32_t n, int w) { (void)p; (void)o; (void)n; (void)w; }
int  card_data_writes_check(uint32_t p, uint32_t v, uint8_t w) { (void)p; (void)v; (void)w; return 0; }
void ds_note_dma_write(void) {}
void ds_note_read(uint32_t a, uint32_t s) { (void)a; (void)s; }
void ds_note_write(uint32_t a, uint32_t s) { (void)a; (void)s; }
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) { (void)k; (void)d; (void)a; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
void parity_trace_note_write(uint32_t a, uint32_t w, uint32_t pc) { (void)a; (void)w; (void)pc; }
uint32_t ls_read_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; return v; }
void ls_write_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; (void)v; }
void ls_shadow_record_unreplayable(void) {}
int  fntrace_is_game_started(void) { return 0; }
void fntrace_restore_game_started(int started) { (void)started; }
void overlay_loader_note_code_write(void) {}
void overlay_loader_resync_validation_after_restore(void) {}
void gte_canonicalize_cpu_state(CPUState *cpu) { (void)cpu; }

/* ---- scheduler / interrupts ------------------------------------------------ */
void psx_advance_cycles_slow(uint32_t c) { psx_cycle_count += c; }
/* [timing] guest_cycle_scale at 1 (faithful) for the real memory.c/icache. */
uint32_t g_psx_gcs_recip_q16 = 65536u;
uint32_t g_psx_gcs_frac = 0u;
uint32_t psx_guest_cycle_scale_config(void) { return 1u; }
void psx_guest_cycle_scale_snapshot(uint32_t out[3]) { out[0] = out[1] = out[2] = 0u; }
void psx_guest_cycle_scale_restore(const uint32_t in[3]) { (void)in; }
void psx_devices_mmio_sync(void) {}
void psx_devices_service_to_now(void) {}
/* Source devices are disabled in the ordinary geometry/cache fixtures. */
int source_gpu_runtime_active(void){return 0;}
int gpu_dma_source_ll_ready(void){fputs("unexpected source GPU\n",stderr);exit(2);}
uint32_t cdrom_dma_read_padded(void){fputs("unexpected source CD\n",stderr);exit(2);}
int cdrom_get_setloc_lba(void){return 0;}
void overlay_capture_on_dma(uint32_t a,uint32_t n,const uint8_t *p){(void)a;(void)n;(void)p;}
void spu_catch_up(void) {}
int mod_runtime_snapshot_package_active(const char *id) { (void)id; return 0; }
int  psx_get_in_exception(void) { return 0; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void psx_irq_refresh_cause_ip2(void) {}
void psx_fatal_halt(const char *reason) { (void)reason; __builtin_trap(); }
void sio_card_handoff_on_imask(uint32_t o, uint32_t n) { (void)o; (void)n; }
static uint32_t s_csv;
uint32_t interrupts_get_cycles_since_vblank(void) { return s_csv; }
void interrupts_set_cycles_since_vblank(uint32_t v) { s_csv = v; g_stub_device_restores++; }
void timers_get_snapshot(uint16_t c[3], uint32_t m[3], uint16_t t[3], int32_t l[3], uint32_t f[3]) {
    for (int i = 0; i < 3; i++) { c[i] = (uint16_t)i; m[i] = 0; t[i] = 0; l[i] = 0; f[i] = 0; }
}
void timers_set_snapshot(const uint16_t c[3], const uint32_t m[3], const uint16_t t[3],
                         const int32_t l[3], const uint32_t f[3]) {
    (void)c; (void)m; (void)t; (void)l; (void)f; g_stub_device_restores++;
}

/* ---- MMIO devices the routing reaches ------------------------------------- */
void cdrom_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void timers_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void spu_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void sio_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void mdec_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void gpu_write_gp0(uint32_t v) { (void)v; }
void gpu_write_gp1(uint32_t v) { (void)v; }
void gpu_set_gp0_source(uint32_t a) { (void)a; }
uint32_t gpu_read_gpuread(void) { return 0; }
uint32_t spu_dma_read(void) { return 0; }
void spu_dma_write(uint32_t w) { (void)w; }
void mdec_debug_dma_in_start(uint32_t a, uint32_t w) { (void)a; (void)w; }
void mdec_debug_dma_out_start(uint32_t a, uint32_t w) { (void)a; (void)w; }
void cdrom_debug_snapshot(void *out) { (void)out; }
uint32_t cdrom_dma_sector_word_count(void) { return 0; }
void dma_gpu_ll_start(void *s, uint32_t a, uint32_t m) { (void)s; (void)a; (void)m; }
void dma_gpu_ll_cancel(void *s) { (void)s; }
/* The kick's CPU hold asks for the next walk event; the stubbed start leaves
 * the walk inactive, so the hold never loops. */
uint32_t dma_gpu_ll_cycles_to_event(const void *s) { (void)s; return 0xFFFFFFFFu; }
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) { (void)a; }
void gpu_ws_restore_linked_list_rank(uint32_t r) { (void)r; }

/* ---- render passes (render_pass.c; never active in this test) ------------- */
/* memory.c routes stores through the pass store policy and dma.c walks a GPU
 * linked list synchronously while a pass runs; with the flag at 0 neither
 * path is taken, so the doubles trap if they ever are. */
int g_psx_render_pass_active;
int render_pass_mmio_read_allowed(uint32_t a, uint32_t w) { (void)a; (void)w; return 1; }
int g_psx_guest_time_frozen;
int render_pass_store_to(const void *t, uint32_t a, uint32_t v, uint32_t w) {
    (void)t; (void)a; (void)v; (void)w; __builtin_trap();
}
void dma_gpu_ll_advance(void *s, uint32_t c, const void *ops, void *o) {
    (void)s; (void)c; (void)ops; (void)o; __builtin_trap();
}
void gpu_set_gp0_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; }
void gpu_ws_validate_linked_list_header(uint32_t a, uint32_t h) { (void)a; (void)h; }
void gpu_ws_validate_linked_list_node(uint32_t a, uint32_t n) { (void)a; (void)n; }
/* MMIO readers: the render-pass store path in memory.c keeps them in this
 * link (mingw-w64 GCC 15 + LTO fails without them). No test access reaches
 * MMIO, so these trap too. */
void debug_server_trace_mmio_read(uint32_t a, uint32_t v, uint8_t w) {
    (void)a; (void)v; (void)w; __builtin_trap();
}
uint32_t cdrom_read(uint32_t a) { (void)a; __builtin_trap(); }
uint32_t mdec_read(uint32_t a) { (void)a; __builtin_trap(); }
uint32_t gpu_read_gpustat(void) { __builtin_trap(); }
uint32_t spu_read(uint32_t a) { (void)a; __builtin_trap(); }
uint32_t timers_read(uint32_t a) { (void)a; __builtin_trap(); }
uint32_t sio_read(uint32_t a) { (void)a; __builtin_trap(); }
void sio_tick(int c) { (void)c; __builtin_trap(); }

/* ---- device snapshot sections (fixed-size doubles; readers count) --------- */
#define STUB_SECTION(name, size)                                             \
    uint32_t name##_snapshot_bytes(void) { return (size); }                  \
    void name##_snapshot_write(uint8_t *p) { memset(p, 0xA5, (size)); }      \
    int name##_snapshot_read(const uint8_t *p, uint32_t len) {               \
        (void)p; g_stub_device_restores++; return len == (size);             \
    }
STUB_SECTION(gpu, 96u)
STUB_SECTION(spu, 80u)
STUB_SECTION(cdrom, 64u)
STUB_SECTION(sio, 48u)
STUB_SECTION(mdec, 400u)
uint32_t spu_cd_snapshot_bytes(void) {return 4;}
void spu_cd_snapshot_write(uint8_t *p) {memset(p,0,4);}
int spu_cd_snapshot_read(const uint8_t *p,uint32_t n) {if(!p||n!=4)return 0;g_stub_device_restores++;return 1;}
int spu_snapshot_len_ok(uint32_t len) { return len == 80u; }
int gpu_snapshot_validate(const uint8_t *p,uint32_t len){return p && len==96u;}
/* This fixture's CD writer emits exactly 64 bytes of 0xA5. Validate that
 * wire without counting a restore or mutating any device. */
int cdrom_snapshot_validate(const uint8_t *p, uint32_t len) {
    if (!p || len != 64u) return 0;
    for (uint32_t i = 0; i < len; ++i) if (p[i] != 0xA5) return 0;
    return 1;
}
int sio_snapshot_validate(const uint8_t *p, uint32_t len) { return p && len == 48u; }
int mdec_snapshot_validate(const uint8_t *p, uint32_t len) { return p && len == 400u; }

static uint8_t s_spu_ram[512u * 1024u];
uint8_t *spu_get_ram_ptr(void) { return s_spu_ram; }
uint32_t spu_get_ram_bytes(void) { return (uint32_t)sizeof s_spu_ram; }

static uint16_t s_vram[1024u * 512u];
const uint16_t *gpu_get_vram(void) { return s_vram; }
int gpu_vram_dirty_tracking(void) { return 0; }
int gpu_vram_dirty_verify_enabled(void) { return 0; }
uint32_t gpu_vram_dirty_row_count(void) { return 512u; }
const uint64_t *gpu_vram_dirty_mask(void) { static uint64_t m[8]; return m; }
void gpu_vram_dirty_clear(void) {}
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *d) {
    (void)x; (void)y; memcpy(d, s_vram, (size_t)w * (size_t)h * 2u);
}
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *d) {
    (void)x; (void)y; memcpy(s_vram, d, (size_t)w * (size_t)h * 2u);
    g_stub_device_restores++;
}
void gr_vram_upload_begin(int x,int y,int w,int h) { (void)x; (void)y; (void)w; (void)h; }
void gr_vram_upload_commit(int x,int y,int w,int h,const uint16_t *d) { gr_vram_transfer_in(x,y,w,h,d); }
void gr_vram_upload_set_open(int open) { (void)open; }

/* These default-controller fixtures deliberately leave the optional MDEC off. */
int mdec_source_active(void){return 0;}
void mdec_source_advance(uint32_t clocks){(void)clocks;__builtin_trap();}
uint32_t mdec_source_dma_read(uint32_t *offset){(void)offset;__builtin_trap();}
void mdec_dma_write_word(uint32_t v){(void)v;__builtin_trap();} int mdec_dma_write_ready(void){__builtin_trap();} int mdec_dma_read_ready(void){__builtin_trap();}

#include "gpu_queue_test_stubs.h"
#include "dirty_ram_interp.h"
#include "psx_scheduler.h"
uint64_t g_psx_device_gen;
uint64_t psx_get_cycle_count(void) {return psx_cycle_count;}
static uint8_t map_exec[DIRTY_RAM_CHECKPOINT_BYTES],map_sched[PSX_SCHEDULER_SNAPSHOT_BYTES],map_irq_timing[64];
static int map_wire_ok(const uint8_t *p,uint32_t n,const uint8_t *expected,uint32_t size) {return p&&n==size&&!memcmp(p,expected,n);}
uint32_t dirty_ram_checkpoint_pc(uint32_t fallback) {return fallback;}
void dirty_ram_checkpoint_write(uint8_t *p) {memcpy(p,map_exec,sizeof map_exec);}
int dirty_ram_checkpoint_validate(const uint8_t *p,uint32_t n) {return map_wire_ok(p,n,map_exec,sizeof map_exec);}
int dirty_ram_checkpoint_read(const uint8_t *p,uint32_t n) {if(!dirty_ram_checkpoint_validate(p,n))return 0;g_stub_device_restores++;return 1;}
void psx_scheduler_snapshot_write(uint8_t *p,uint32_t n) {if(n!=sizeof map_sched)abort();memcpy(p,map_sched,n);}
int psx_scheduler_snapshot_validate(const uint8_t *p,uint32_t n,const uint8_t *ram) {return ram&&map_wire_ok(p,n,map_sched,sizeof map_sched);}
int psx_scheduler_snapshot_read(const uint8_t *p,uint32_t n,CPUState *cpu) {if(!cpu||!map_wire_ok(p,n,map_sched,sizeof map_sched))return 0;g_stub_device_restores++;return 1;}
void interrupts_timing_wire_write(uint8_t *p) {memcpy(p,map_irq_timing,sizeof map_irq_timing);}
int interrupts_timing_wire_read(const uint8_t *p,uint32_t n) {if(!p||n!=sizeof map_irq_timing)return 0;memcpy(map_irq_timing,p,n);g_stub_device_restores++;return 1;}
int interrupts_raster_comparison_active(void) {return 0;}
int timers_source_active(void) {return 0;}
#define MAP_INACTIVE(name) void name##_write(uint8_t *p){(void)p;abort();}     int name##_read(const uint8_t *p,uint32_t n){(void)p;(void)n;abort();}
MAP_INACTIVE(interrupts_raster_wire)
MAP_INACTIVE(source_gpu_raster_wire)
MAP_INACTIVE(source_gpu_service_wire)
MAP_INACTIVE(timers_source_wire)
#undef MAP_INACTIVE
void source_gpu_runtime_rederive_returns(void) {}
void interrupts_note_state_load(int present) {(void)present;}
void gte_precision_timeline_invalidate(void) {}
int mdec_snapshot_prepare(const uint8_t *p,uint32_t n) {return mdec_snapshot_validate(p,n);}
int sio_snapshot_shape_ok(uint32_t n) {return n==48;}
int spu_snapshot_validate(const uint8_t *p,uint32_t n) {return p&&n==80;}
int spu_cd_snapshot_validate(const uint8_t *p,uint32_t n) {return p&&n==4&&!p[0]&&!p[1]&&!p[2]&&!p[3];}

/* Report-only accounting is outside this fixture's hardware seams. */
void dirty_ram_pc_table_game_load(void) {}
