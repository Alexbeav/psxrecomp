/* The KSEG2 ignored-access counters reach the run report (PS1B-516).
 *
 * memory.c drops reads and writes at KSEG2 (0xC0000000 and up; only the cache
 * control register at 0xFFFE0130 exists) and counts them in
 * g_kseg2_ignored_reads and g_kseg2_ignored_writes. Nothing printed them.
 * crash_trace.c now writes memory_kseg2_ignored_json() into
 * psx_last_run_report.json as "kseg2_ignored": {"reads": N, "writes": N}.
 *
 * This fixture links the real memory.c and checks the text that function
 * returns, read after each access:
 *
 *   1. a KSEG2 word read raises "reads" by one and leaves "writes" alone;
 *   2. a KSEG2 word store raises "writes" by one and leaves "reads" alone;
 *   3. KSEG0 and KSEG1 RAM reads and the cache control register raise
 *      neither;
 *   4. a buffer that is too small gives 0, not a cut-off string.
 *
 * test_kseg2_report_counters.py builds it and checks that crash_trace.c prints
 * the field; link seams that this path does not reach are stubs that abort
 * (source_fixture_link.py). */
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "kernel_patch_ranges.h"
#include "psx_bios_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- memory.c under test ------------------------------------------------ */
uint32_t psx_read_word(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t val);
int memory_kseg2_ignored_json(char *out, int cap);

/* ---- what memory.c and the inline loads read from the rest of the runtime */
void (*g_overlay_flush_pending_cycles)(void);
uint32_t g_debug_last_store_pc, g_debug_current_func_addr;
uint32_t g_overlay_region_floor, g_text_image_lo;
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS];
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
int g_dma_exec_depth;
int g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr, g_dma_cur_bcr, g_dma_initiator_pc;
uint32_t g_dma_cpu_read_wait;
volatile int g_ds_recording;
int g_ls_mode, g_ls_replay_active, g_ls_suppress_record;
int g_ram_read_watch_active, g_event_step_conservative, g_insn_log_frozen;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
int g_psx_cyc_bb_defer;
uint32_t *g_psx_cyc_local_acc;
uint64_t g_psx_bail_flattened, g_psx_device_gen, psx_cycle_count, s_frame_count;
uint64_t psx_next_service_cycle;
int psx_in_device_service;
uint32_t g_psx_icache_tv[1024];
CPUState *debug_cpu_ptr;
PsxBiosImageInfo psx_bios_image;
const PsxKernelBody *psx_bios_kernel_bodies;
uint32_t psx_bios_kernel_body_count;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges;
uint32_t psx_bios_kernel_patch_range_count;

int source_gpu_runtime_active(void) { return 0; }
int fntrace_is_game_started(void) { return 1; }
int psx_get_in_exception(void) { return 0; }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val) {
    (void)phys; (void)val;
}

static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static void expect(unsigned reads, unsigned writes, const char *what) {
    char got[96], want[96];
    snprintf(want, sizeof want, "{\"reads\": %u, \"writes\": %u}", reads, writes);
    check(memory_kseg2_ignored_json(got, sizeof got) > 0, "the report text fits its buffer");
    if (strcmp(got, want)) { fprintf(stderr, "FAIL: %s: got %s, want %s\n", what, got, want); exit(1); }
}

int main(void) {
    char got[96], tiny[8];
    unsigned r = 0, w = 0;
    uint32_t value = 0x12345678u;

    memcpy(g_psx_ram + 0x1000u, &value, sizeof value);
    /* The counters belong to the process: start from what they hold. */
    check(memory_kseg2_ignored_json(got, sizeof got) > 0, "report text before any access");
    sscanf(got, "{\"reads\": %u, \"writes\": %u}", &r, &w);
    expect(r, w, "start");

    check(psx_read_word(0x80001000u) == value, "KSEG0 word reads RAM");
    check(psx_read_word(0xA0001000u) == value, "KSEG1 word reads RAM");
    expect(r, w, "RAM reads are not counted");

    check(psx_read_word(0xC0001000u) == 0u, "KSEG2 word reads 0");
    expect(r + 1u, w, "one KSEG2 word read");
    psx_write_word(0xC0001000u, 0xdeadbeefu);
    expect(r + 1u, w + 1u, "one KSEG2 word store");
    check(psx_read_word(0xE0001000u) == 0u, "KSEG2 mirror word reads 0");
    expect(r + 2u, w + 1u, "a second KSEG2 word read");

    (void)psx_read_word(0xFFFE0130u);
    expect(r + 2u, w + 1u, "cache control is read, not ignored");

    check(memory_kseg2_ignored_json(tiny, sizeof tiny) == 0, "a small buffer gives 0");

    puts("PASS: KSEG2 word read and store each raise the run-report counters by one");
    return 0;
}
