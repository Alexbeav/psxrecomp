/* A data shard replay marks the RAM pages it writes (PS1B-343).
 *
 * A data shard holds the stores of one call of a hooked function. A replay
 * puts those bytes back without running the function. It is a load path, like
 * a CD DMA transfer, and a load path marks the pages it fills
 * (docs/OVERLAY_MODIFIED_TEXT.md): the next dispatch into such a page checks
 * its bytes again, and every cache keyed on the code generation is derived
 * again.
 *
 * This fixture links the real shard store (data_shards.c) and the real store
 * path and page bitmap (memory.c). It records one call, replays it, and reads
 * the bitmap through the calls that the state file, the netplay digest and
 * the overlay loader use:
 *
 *   1. data shards do nothing until they are turned on;
 *   2. the recorded call itself marks no page: ordinary CPU stores above the
 *      kernel window do not;
 *   3. the replay puts every recorded byte back;
 *   4. after the replay the marked pages are exactly the RAM pages the shard
 *      wrote (a store to the scratchpad marks none), and the code generation
 *      has moved.
 *
 * PS1B-474 also checks byte input verification, byte output replay and the
 * byte feeds' MMIO, exception and DMA rules.
 *
 * test_data_shard_replay_marks.py builds it; link seams that this path does
 * not reach are stubs that abort (source_fixture_link.py). */
#include "cpu_state.h"
#include "data_shards.h"
#include "dirty_ram_interp.h"
#include "kernel_patch_ranges.h"
#include "psx_bios_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_MASK    0x001FFFFFu
#define PAGE_SHIFT  12

#define FUNC_ADDR   0x80034560u   /* the hooked function */
#define CALL_RA     0x80012340u
#define CALL_SP     0x801FFF00u
#define IN_ADDR     0x80110000u   /* the call's input word */
#define TEXT_ADDR   0x80123FF8u   /* 16 bytes over pages 0x123 and 0x124, inside the boot text */
#define OVL_ADDR    0x801A0010u   /* 4 bytes in page 0x1A0, above the boot text */
#define SCR_ADDR    0x1F800020u   /* 4 bytes in the scratchpad */
#define TEXT_WORDS  4u
#define BYTE_IN     (IN_ADDR + 8u)
#define BYTE_OUT    (OVL_ADDR + 16u)
#define OMIT_IN     (IN_ADDR + 12u)
#define OMIT_OUT    (OVL_ADDR + 20u)
#define MMIO_ADDR   0x1F801000u   /* memory-control byte lane in real memory.c */

/* ---- memory.c under test ------------------------------------------------ */
uint8_t *memory_get_ram_ptr(void);
uint8_t *memory_get_scratchpad_ptr(void);
uint32_t psx_read_word(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t val);
uint8_t psx_read_byte(uint32_t addr);
void psx_write_byte(uint32_t addr, uint8_t val);
extern uint32_t g_dirty_ram_code_gen;

/* ---- what the two sources read from the rest of the runtime -------------- */
void (*g_overlay_flush_pending_cycles)(void);
uint32_t g_debug_last_store_pc, g_debug_current_func_addr;
uint32_t g_overlay_region_floor, g_text_image_lo;
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS];
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
int g_dma_exec_depth;
int g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr, g_dma_cur_bcr, g_dma_initiator_pc;
int g_ls_mode, g_ls_replay_active, g_ls_suppress_record;
int g_ram_read_watch_active, g_event_step_conservative, g_insn_log_frozen;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
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
static int in_exception;
int psx_get_in_exception(void) { return in_exception; }
/* Observer and scheduler seams; memory.c's real MMIO byte lane still runs. */
static unsigned device_reads, device_writes;
void psx_devices_mmio_sync(void) { }
void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr; (void)val; (void)width; ++device_reads;
}
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr; (void)val; (void)width; ++device_writes;
}
/* The recorded call costs no guest time here, so the replay credits none. */
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width) {
    (void)phys; (void)value; (void)width; return 0;
}
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width) {
    (void)phys; (void)old_val; (void)new_val; (void)width;
}
void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}

/* ---- the fixture -------------------------------------------------------- */
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static int page_marked(uint32_t page) {
    return (int)((dirty_ram_get_bitmap_word(page >> 5) >> (page & 31u)) & 1u);
}

static unsigned marked_pages(void) {
    unsigned n = 0;
    for (uint32_t w = 0; w < dirty_ram_get_bitmap_word_count(); ++w)
        for (uint32_t bits = dirty_ram_get_bitmap_word(w); bits; bits &= bits - 1u) ++n;
    return n;
}

/* One counter of the store, as the debug server's data_shards command prints it. */
static int stat_is(const char *field, unsigned value) {
    char json[1024], want[96];
    ds_stats_json(json, (int)sizeof json);
    snprintf(want, sizeof want, "\"%s\":%u,", field, value);
    return strstr(json, want) != NULL;
}

/* What the hooked function does: one input word read, three places written. */
static const uint32_t text_words[TEXT_WORDS] = {
    0x3C028012u, 0x8C423FF8u, 0x03E00008u, 0x00000000u
};
static void hooked_function_body(void) {
    uint32_t in = psx_read_word(IN_ADDR);
    for (uint32_t i = 0; i < TEXT_WORDS; ++i)
        psx_write_word(TEXT_ADDR + 4u * i, text_words[i] ^ in);
    psx_write_word(OVL_ADDR, 0x0BADC0DEu ^ in);
    psx_write_word(SCR_ADDR, 0x5CA7C4EDu ^ in);
}
static int outputs_present(uint32_t in) {
    for (uint32_t i = 0; i < TEXT_WORDS; ++i)
        if (psx_read_word(TEXT_ADDR + 4u * i) != (text_words[i] ^ in)) return 0;
    return psx_read_word(OVL_ADDR) == (0x0BADC0DEu ^ in) &&
           psx_read_word(SCR_ADDR) == (0x5CA7C4EDu ^ in);
}

static void byte_function_body(void) {
    psx_write_byte(BYTE_OUT, (uint8_t)(psx_read_byte(BYTE_IN) ^ 0xA5u));
}

static void check_byte_feeds(CPUState *cpu, uint8_t *ram) {
    const uint32_t input = BYTE_IN & RAM_MASK, output = BYTE_OUT & RAM_MASK;
    ram[input] = 0x36u;
    ram[output - 1u] = 0x11u; ram[output + 1u] = 0x22u;
    check(!psx_datashard_enter(cpu, FUNC_ADDR + 4u) && g_ds_recording,
          "the byte call arms a capture");
    byte_function_body();
    psx_datashard_ret(cpu);
    check(stat_is("captures_ok", 2u), "the byte call is captured");
    ram[output] = 0;
    check(psx_datashard_enter(cpu, FUNC_ADDR + 4u) && stat_is("replays", 2u),
          "the byte call replays");
    check(ram[output] == (0x36u ^ 0xA5u) && ram[output - 1u] == 0x11u &&
          ram[output + 1u] == 0x22u, "byte replay restores only the stored byte");
    ram[input] ^= 1u;
    check(!psx_datashard_enter(cpu, FUNC_ADDR + 4u) && g_ds_recording &&
          stat_is("verify_fail", 1u) && stat_is("replays", 2u),
          "changed byte input rejects the shard with exactly one verify failure");
    byte_function_body();
    psx_datashard_ret(cpu);
    check(stat_is("captures_ok", 3u), "the changed input records a new variant");

    psx_write_byte(MMIO_ADDR, 0x7Au);
    device_reads = device_writes = 0;
    for (unsigned write = 0; write < 2u; ++write) {
        check(!psx_datashard_enter(cpu, FUNC_ADDR + 8u + 4u * write) && g_ds_recording,
              "the device byte call arms a capture");
        if (write) psx_write_byte(MMIO_ADDR, 0x42u);
        else check(psx_read_byte(MMIO_ADDR) == 0x7Au, "the device read returns its byte");
        psx_datashard_ret(cpu);
        check(stat_is("poison_mmio", write + 1u) && stat_is("captures_poisoned", write + 1u) &&
              stat_is("captures_ok", 3u), "byte MMIO poisons the capture instead of storing it");
    }
    check(device_reads == 1u && device_writes == 1u, "both byte device paths execute");

    check(!psx_datashard_enter(cpu, FUNC_ADDR + 16u) && g_ds_recording,
          "the exception exclusion call arms a capture");
    in_exception = 1;
    (void)psx_read_byte(OMIT_IN);
    psx_write_byte(OMIT_OUT, 0x55u);
    (void)psx_read_byte(MMIO_ADDR);
    psx_write_byte(MMIO_ADDR, 0x42u);
    in_exception = 0;
    byte_function_body();
    psx_datashard_ret(cpu);
    ram[OMIT_IN & RAM_MASK] ^= 1u; ram[OMIT_OUT & RAM_MASK] = 0;
    ram[output] = 0;
    check(psx_datashard_enter(cpu, FUNC_ADDR + 16u) && ram[OMIT_OUT & RAM_MASK] == 0 &&
          ram[output] == (ram[input] ^ 0xA5u) && stat_is("captures_ok", 4u) &&
          stat_is("verify_fail", 1u) && stat_is("poison_mmio", 2u),
          "exception byte reads, stores and MMIO are excluded");

    check(!psx_datashard_enter(cpu, FUNC_ADDR + 20u) && g_ds_recording,
          "the DMA read exclusion call arms a capture");
    g_dma_exec_depth = 1;
    (void)psx_read_byte(OMIT_IN); (void)psx_read_byte(MMIO_ADDR);
    g_dma_exec_depth = 0;
    byte_function_body();
    psx_datashard_ret(cpu);
    ram[OMIT_IN & RAM_MASK] ^= 1u;
    check(psx_datashard_enter(cpu, FUNC_ADDR + 20u) && stat_is("captures_ok", 5u) &&
          stat_is("verify_fail", 1u) && stat_is("poison_mmio", 2u),
          "DMA byte reads are excluded");
    check(!psx_datashard_enter(cpu, FUNC_ADDR + 24u) && g_ds_recording,
          "the DMA byte store call arms a capture");
    g_dma_exec_depth = 1;
    psx_write_byte(BYTE_OUT, 0x66u);
    g_dma_exec_depth = 0;
    psx_datashard_ret(cpu);
    check(stat_is("poison_dma", 1u) && stat_is("captures_poisoned", 3u) &&
          stat_is("captures_ok", 5u), "a DMA byte store poisons the capture");
}

int main(void) {
    static CPUState cpu;
    static const uint8_t input[4] = {0x44u, 0x33u, 0x22u, 0x11u};
    const uint32_t text_phys = TEXT_ADDR & RAM_MASK, ovl_phys = OVL_ADDR & RAM_MASK;
    uint8_t *ram = memory_get_ram_ptr();
    uint8_t *scratchpad = memory_get_scratchpad_ptr();
    uint32_t in, gen;

    /* Boot text at 0x10000..0x17FFFF: there, only the bitmap says whether a
     * page is dirty. Above it every page counts as dirty already. */
    g_text_image_lo = 0x00010000u;
    g_overlay_region_floor = 0x00180000u;
    g_debug_last_store_pc = FUNC_ADDR + 0x40u;
    memcpy(ram + (IN_ADDR & RAM_MASK), input, sizeof input);
    in = psx_read_word(IN_ADDR);

    cpu.gpr[4] = IN_ADDR; cpu.gpr[5] = TEXT_ADDR; cpu.gpr[6] = OVL_ADDR; cpu.gpr[7] = SCR_ADDR;
    cpu.gpr[29] = CALL_SP; cpu.gpr[31] = CALL_RA;

    /* 1. Off until turned on: the hook neither replays nor records. */
    check(!psx_datashard_enter(&cpu, FUNC_ADDR) && !g_ds_recording && stat_is("enters", 0u),
          "data shards do nothing until they are turned on");
    ds_set_enabled(1);

    /* The first call: no shard yet, so the function runs and is recorded. */
    check(!psx_datashard_enter(&cpu, FUNC_ADDR) && g_ds_recording,
          "the first call arms a capture");
    hooked_function_body();
    psx_datashard_ret(&cpu);
    check(!g_ds_recording && stat_is("captures_ok", 1u) && stat_is("captures_poisoned", 0u),
          "the first call is stored as one shard");
    check(outputs_present(in), "the first call wrote its outputs");

    /* 2. The function's own stores mark nothing. */
    check(marked_pages() == 0u && !dirty_ram_is_dirty(text_phys),
          "the recorded call marks no page");

    /* The same call again with its outputs gone: the shard is replayed. */
    memset(ram + text_phys, 0, 4u * TEXT_WORDS);
    memset(ram + ovl_phys, 0, 4u);
    memset(scratchpad + (SCR_ADDR - 0x1F800000u), 0, 4u);
    check(!outputs_present(in), "the outputs are gone before the second call");
    gen = g_dirty_ram_code_gen;
    cpu.pc = 0;
    check(psx_datashard_enter(&cpu, FUNC_ADDR) == 1 && stat_is("replays", 1u),
          "the second call is replayed from the shard");
    check(cpu.pc == CALL_RA && !g_ds_recording, "the replay returns to the caller");

    /* 3. */
    check(outputs_present(in), "the replay puts every recorded byte back");

    /* 4. Every RAM page the replay wrote is marked, and no other page. */
    check(page_marked(text_phys >> PAGE_SHIFT) &&
          page_marked((text_phys + 4u * TEXT_WORDS - 1u) >> PAGE_SHIFT) &&
          page_marked(ovl_phys >> PAGE_SHIFT),
          "the replay marks every RAM page it wrote");
    check(marked_pages() == 3u, "the replay marks no other page");
    check(dirty_ram_is_dirty(text_phys) && dirty_ram_is_dirty(text_phys + 8u),
          "a dispatch into replayed boot text finds its page dirty");
    check(g_dirty_ram_code_gen != gen, "the replay moves the code generation");

    check_byte_feeds(&cpu, ram);
    puts("data_shard_replay_marks: PASS (word marks, byte replay/verification/MMIO/exclusions)");
    return 0;
}
