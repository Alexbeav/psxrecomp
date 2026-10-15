/* Guest RAM 0..15 belongs to the guest: game entry leaves it as the BIOS left
 * it, and every guest store to it lands. A replay of a recording from an
 * older build still compares those bytes as that build held them.
 *
 * Two older kinds of build changed RAM 0..15 from the host, and their player
 * replays hold state digests of RAM as they held it:
 *
 *   - builds before PS1G-39 zeroed the 16 bytes at game entry, and dropped
 *     some guest word stores to them after game entry, by the program counter
 *     of the store;
 *   - builds from PS1G-39 up to PS1B-456 kept the bytes at game entry and
 *     dropped the same stores.
 *
 * This fixture links the real store path (memory.c), the real game-start
 * latch (fntrace.c) and the real core digest (netplay_state_digest.c), and
 * checks four things:
 *
 *   1. words stored at RAM 0..15 before game entry are still in RAM after it;
 *   2. a guest store to RAM 0..15 lands whatever instruction made it, also
 *      the stores that the older builds dropped, and the run report's count
 *      takes exactly those;
 *   3. with the switch for one of the older kinds on, the RAM part of the
 *      core digest equals the digest of RAM as a build of that kind holds it
 *      (`old_low` and `kept_low` below, kept by hand by that build's rule),
 *      and with both switches off it is the digest of RAM itself;
 *   4. the 16 bytes at game entry are kept for the run report.
 *
 * The end of main() keeps the known limit of point 3 as a negative case: an
 * unaligned word store (SWL, SWR) into RAM 0..15.
 *
 * test_low_ram_game_entry.py builds it; link seams that this path does not
 * reach are stubs that abort (source_fixture_link.py). */
#include "cpu_state.h"
#include "crc32.h"
#include "dirty_ram_interp.h"
#include "fntrace.h"
#include "kernel_patch_ranges.h"
#include "netplay_state_digest.h"
#include "psx_bios_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_BYTES (2u << 20)

/* ---- memory.c under test ------------------------------------------------ */
uint8_t *memory_get_ram_ptr(void);
uint32_t psx_read_word(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t val);
void psx_write_half(uint32_t addr, uint16_t val);
void psx_write_byte(uint32_t addr, uint8_t val);
void memory_set_low_ram_view_old(int on);
void memory_set_low_ram_view_filtered(int on);
const uint8_t *memory_low_ram_view(void);
int memory_low_ram_at_entry(uint8_t out[16]);
uint64_t memory_low_ram_older_dropped_stores(void);

/* ---- what the three sources read from the rest of the runtime ----------- */
void (*g_overlay_flush_pending_cycles)(void);
uint32_t g_debug_last_store_pc, g_debug_current_func_addr;
uint32_t g_overlay_region_floor, g_text_image_lo;
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS];
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
int g_dma_exec_depth;
int g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr, g_dma_cur_bcr, g_dma_initiator_pc;
volatile int g_ds_recording;
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

static int source_profile;
int source_gpu_runtime_active(void) { return source_profile; }
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
static unsigned cd_notices, captures;
void cdrom_notify_game_started(void) { cd_notices++; }
void boot_state_trigger_capture(const CPUState *cpu) { (void)cpu; captures++; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3], uint16_t target[3],
                         int32_t irq_line[3], uint32_t frac[3]) {
    memset(counter, 0, 3 * sizeof *counter);
    memset(mode, 0, 3 * sizeof *mode);
    memset(target, 0, 3 * sizeof *target);
    memset(irq_line, 0, 3 * sizeof *irq_line);
    memset(frac, 0, 3 * sizeof *frac);
}

/* ---- the fixture -------------------------------------------------------- */
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

/* The ten store addresses whose word store to address 0 an older build
 * dropped after game entry: eight in the SCPH1001 ROM, two in one game. */
static const uint32_t dropped_pc[10] = {
    0xBFC04E90u, 0xBFC04EF0u, 0xBFC05164u, 0xBFC0D634u, 0xBFC3EEB4u,
    0xBFC405E4u, 0xBFC40788u, 0xBFC41C50u, 0x80012434u, 0x800125ACu,
};

/* RAM as an older build holds it: every byte as in real RAM except 0..15,
 * which this fixture keeps by that build's rule. old_low is a build with the
 * clear at game entry; kept_low is a build that kept the bytes at game entry.
 * Both dropped the same stores. */
static uint8_t old_low[16];
static uint8_t kept_low[16];
static uint8_t model[RAM_BYTES];

/* A store that both older kinds of build let land. */
static void both_kept(uint32_t at, uint32_t value, uint32_t width) {
    for (uint32_t b = 0; b < width; ++b)
        old_low[at + b] = kept_low[at + b] = (uint8_t)(value >> (8 * b));
}

static NetplayCoreParts digest_parts(void) {
    static CPUState cpu;
    NetplayCoreParts parts;
    netplay_core_digest_parts(&cpu, &parts);
    return parts;
}
static uint32_t digest_ram_part(void) { return digest_parts().ram; }
static uint32_t crc_of_model(const uint8_t low[16]) {
    memcpy(model, memory_get_ram_ptr(), RAM_BYTES);
    memcpy(model, low, 16);
    return crc32_compute(model, RAM_BYTES);
}
static uint32_t crc_of_ram(void) {
    return crc32_compute(memory_get_ram_ptr(), RAM_BYTES);
}
static void check_view_is(const uint8_t low[16], const char *when, const char *which) {
    char what[200];
    snprintf(what, sizeof what, "%s: %s, the view is that build's 16 bytes", when, which);
    check(memory_low_ram_view() != memory_get_ram_ptr() &&
          !memcmp(memory_low_ram_view(), low, 16), what);
    snprintf(what, sizeof what, "%s: %s, the digest is that build's", when, which);
    check(digest_ram_part() == crc_of_model(low), what);
}
/* Every switch position, against real RAM and against the two models. */
static void check_views(const char *when) {
    char what[200];
    memory_set_low_ram_view_old(0);
    memory_set_low_ram_view_filtered(0);
    snprintf(what, sizeof what, "%s: switches off, the view is RAM itself", when);
    check(memory_low_ram_view() == memory_get_ram_ptr(), what);
    snprintf(what, sizeof what, "%s: switches off, the digest is of RAM", when);
    check(digest_ram_part() == crc_of_ram(), what);
    memory_set_low_ram_view_old(1);
    check_view_is(old_low, when, "a recording of a build with the clear");
    /* A build with the clear dropped the stores too: its switch decides. */
    memory_set_low_ram_view_filtered(1);
    check_view_is(old_low, when, "both switches on");
    memory_set_low_ram_view_old(0);
    check_view_is(kept_low, when, "a recording of a build that dropped stores");
    memory_set_low_ram_view_filtered(0);
}

int main(void) {
    /* The four words PSX-SPX gives for RAM 0..15 on a retail kernel. */
    static const uint32_t bios_words[4] = {0x00000003u, 0x275a0c80u, 0x03400008u, 0x00000000u};
    static CPUState cpu;
    uint8_t *ram = memory_get_ram_ptr();
    uint8_t expect[16], at_entry[16];
    uint32_t word, dirty_part;

    /* Before game entry: the kernel's stores land, and every build agrees.
     * The older builds dropped no store before game entry, so the stub copy
     * comes from 0xBFC10A00 here and the 3 from a delay-loop address. */
    g_debug_last_store_pc = 0xBFC10A00u;
    psx_write_word(0x80000000u, 0x3c1a0000u);
    for (uint32_t i = 1; i < 4; ++i) psx_write_word(0x80000000u + 4u * i, bios_words[i]);
    g_debug_last_store_pc = dropped_pc[0];
    psx_write_word(0x80000000u, bios_words[0]);
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t b = 0; b < 4; ++b) expect[4 * i + b] = (uint8_t)(bios_words[i] >> (8 * b));
    check(!memcmp(ram, expect, 16), "the stores before game entry are in RAM");
    memcpy(old_low, expect, 16);
    memcpy(kept_low, expect, 16);
    check(!memory_low_ram_at_entry(at_entry), "no bytes at entry before game entry");
    check_views("before game entry");

    /* Game entry, through the real latch. */
    fntrace_set_game_range(0x80010008u, 0);
    fntrace_mark_game_started(&cpu);
    check(fntrace_is_game_started() && cd_notices == 1 && captures == 1, "the latch ran its handoff");
    check(!memcmp(ram, expect, 16), "game entry leaves RAM 0..15 as the kernel left it");
    check(memory_low_ram_at_entry(at_entry) && !memcmp(at_entry, expect, 16),
          "the 16 bytes at game entry are kept");
    memset(old_low, 0, sizeof old_low);              /* the old host cleared here */
    check(crc_of_model(old_low) != crc_of_ram(), "the RAM of a build with the clear differs from this one's");
    check(crc_of_model(kept_low) == crc_of_ram(), "a build that kept the bytes holds this build's RAM at game entry");
    check_views("at game entry");

    /* Stores after game entry reach RAM and the older builds' bytes alike. */
    g_debug_last_store_pc = 0x80012000u;
    psx_write_word(0x80000008u, 0xCAFEF00Du);
    psx_write_half(0x80000004u, 0x1234u);
    psx_write_byte(0x8000000Fu, 0x56u);
    expect[8] = 0x0D; expect[9] = 0xF0; expect[10] = 0xFE; expect[11] = 0xCA;
    expect[4] = 0x34; expect[5] = 0x12; expect[15] = 0x56;
    check(!memcmp(ram, expect, 16), "the stores after game entry are in RAM");
    both_kept(8, 0xCAFEF00Du, 4);
    both_kept(4, 0x1234u, 2);
    both_kept(15, 0x56u, 1);
    check_views("after three stores");
    check(memory_low_ram_older_dropped_stores() == 0, "no store so far is one that the older builds dropped");

    /* A guest store to RAM 0..15 lands whatever instruction made it. The
     * older builds dropped two kinds of word store after game entry, by the
     * program counter of the store (PS1B-456): every word store to RAM 0..15
     * from 0xBFC10A00, where the kernel copies its exception stub to address
     * 0, and a word store to address 0 from ten listed addresses, the delay
     * loops that leave 3 in the first word. Both are guest stores, and RAM
     * takes them. The older builds' bytes do not: those builds dropped them.
     * The page was written before, so the digest's dirty-page part stays. */
    dirty_part = digest_parts().dirty;
    g_debug_last_store_pc = 0xBFC10A00u;
    for (uint32_t i = 0; i < 4; ++i) psx_write_word(0x80000000u + 4u * i, 0xA0B0C0D0u + i);
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t b = 0; b < 4; ++b) expect[4 * i + b] = (uint8_t)((0xA0B0C0D0u + i) >> (8 * b));
    check(!memcmp(ram, expect, 16), "word stores to RAM 0..15 from 0xBFC10A00 after game entry are in RAM");
    check_views("after the word stores from 0xBFC10A00");
    for (uint32_t i = 0; i < sizeof dropped_pc / sizeof dropped_pc[0]; ++i) {
        char what[96];
        g_debug_last_store_pc = dropped_pc[i];
        psx_write_word(0x80000000u, 0x00000100u + i);
        expect[0] = (uint8_t)i; expect[1] = 0x01; expect[2] = 0; expect[3] = 0;
        snprintf(what, sizeof what, "a word store to RAM 0 from 0x%08X after game entry is in RAM",
                 (unsigned)dropped_pc[i]);
        check(!memcmp(ram, expect, 16), what);
    }
    check_views("after the word stores to RAM 0 from the ten addresses");
    check(memcmp(ram, kept_low, 16) != 0 && memcmp(old_low, kept_low, 16) != 0,
          "RAM and the two older builds' bytes are three different things here");
    check(digest_parts().dirty == dirty_part, "those stores change no dirty-page bit");
    check(memory_low_ram_older_dropped_stores() == 14, "the run report's count holds the 4 and the 10 stores");
    /* The older builds dropped word stores only. A half and a byte store from
     * the same program counter landed there too. */
    g_debug_last_store_pc = 0xBFC10A00u;
    psx_write_half(0x80000002u, 0xBEEFu);
    psx_write_byte(0x8000000Cu, 0x77u);
    expect[2] = 0xEF; expect[3] = 0xBE; expect[12] = 0x77;
    check(!memcmp(ram, expect, 16), "a half and a byte store from 0xBFC10A00 are in RAM");
    both_kept(2, 0xBEEFu, 2);
    both_kept(12, 0x77u, 1);
    check_views("after a half and a byte store from 0xBFC10A00");
    /* Outside their two rules the older builds dropped nothing: a word at
     * 0x10 from 0xBFC10A00, and a word at 4 from a delay-loop address. */
    psx_write_word(0x80000010u, 0x22222222u);
    check(psx_read_word(0x80000010u) == 0x22222222u, "a word store at 0x10 from 0xBFC10A00 is in RAM");
    g_debug_last_store_pc = dropped_pc[0];
    psx_write_word(0x80000004u, 0x33333333u);
    expect[4] = expect[5] = expect[6] = expect[7] = 0x33;
    check(!memcmp(ram, expect, 16), "a word store at 4 from a delay-loop address is in RAM");
    both_kept(4, 0x33333333u, 4);
    check_views("after word stores that the older builds kept");

    /* A store outside 0..15 changes no byte of the 16. */
    g_debug_last_store_pc = 0x80012000u;
    psx_write_word(0x80000010u, 0x11111111u);
    check(!memcmp(ram, expect, 16), "a store at 0x10 leaves RAM 0..15");
    check_views("after a store at 0x10");

    /* A raw writer (a state load): RAM is replaced, and so are the bytes an
     * older build would hold; the writer reports the range, as the rule says. */
    for (uint32_t i = 0; i < 16; ++i) ram[i] = (uint8_t)(0xA0u + i);
    psx_kernel_bless_note_range(0, RAM_BYTES);
    memcpy(old_low, ram, 16);
    memcpy(kept_low, ram, 16);
    check_views("after a state load");

    /* A state from before game entry, then game entry again. */
    fntrace_restore_game_started(0);
    fntrace_mark_game_started(&cpu);
    memcpy(expect, ram, 16);
    check(memory_low_ram_at_entry(at_entry) && !memcmp(at_entry, expect, 16),
          "the second game entry keeps its own 16 bytes");
    memset(old_low, 0, sizeof old_low);
    check_views("at the second game entry");

    /* The source profile never had the clear, and it dropped no store. */
    source_profile = 1;
    psx_kernel_bless_note_range(0, RAM_BYTES);
    fntrace_restore_game_started(0);
    fntrace_mark_game_started(&cpu);
    memcpy(old_low, ram, 16);
    check_views("source profile");
    g_debug_last_store_pc = 0xBFC10A00u;
    psx_write_word(0x80000008u, 0x44444444u);
    g_debug_last_store_pc = dropped_pc[9];
    psx_write_word(0x80000000u, 0x55555555u);
    check(psx_read_word(0x80000008u) == 0x44444444u && psx_read_word(0x80000000u) == 0x55555555u,
          "source profile: the stores from those addresses are in RAM");
    both_kept(8, 0x44444444u, 4);
    both_kept(0, 0x55555555u, 4);
    check_views("source profile, after stores from those addresses");

    /* The known limit, kept as a negative case. SWL and SWR store part of a
     * word. The interpreter and the generated code read the aligned word,
     * merge the register into it and store the whole word. The older build's
     * bytes then take all four bytes from real RAM, also the bytes that the
     * instruction did not change. A build with the clear holds zero in those.
     * So after such a store the view is not that build's RAM, and the replay
     * of an older recording reports a difference that is not real. It cannot
     * hide one: the view's bytes of that word are the real ones. When the
     * store path follows byte by byte, the two checks marked "limit" fail:
     * put check_views() in their place. A build that kept the bytes at game
     * entry holds the real bytes there, so its view has no such limit here. */
    source_profile = 0;
    fntrace_restore_game_started(0);
    fntrace_mark_game_started(&cpu);
    memcpy(expect, ram, 16);
    memset(old_low, 0, sizeof old_low);
    check_views("at the third game entry");
    g_debug_last_store_pc = 0x80012000u;
    /* SWL at 0x80000001: the register's two high bytes go to bytes 0 and 1. */
    word = psx_read_word(0x80000000u);
    psx_write_word(0x80000000u, (word & 0xFFFF0000u) | (0x12345678u >> 16));
    /* SWR at 0x80000006: the register's two low bytes go to bytes 6 and 7. */
    word = psx_read_word(0x80000004u);
    psx_write_word(0x80000004u, (word & 0x0000FFFFu) | (0x9ABCDEF0u << 16));
    expect[0] = 0x34; expect[1] = 0x12; expect[6] = 0xF0; expect[7] = 0xDE;
    check(!memcmp(ram, expect, 16), "SWL and SWR store the merged words in RAM");
    /* The older builds merged the same four bytes into what they held. */
    both_kept(0, 0x1234u, 2);
    both_kept(6, 0xDEF0u, 2);
    memory_set_low_ram_view_old(1);
    check(!memcmp(memory_low_ram_view(), ram, 8),
          "limit: after SWL and SWR the view holds both whole words of real RAM");
    check(memcmp(memory_low_ram_view(), old_low, 8) != 0 && digest_ram_part() != crc_of_model(old_low),
          "limit: after SWL and SWR the view is not the RAM of a build with the clear");
    check(!memcmp(memory_low_ram_view() + 8, old_low + 8, 8),
          "SWL and SWR leave the view's other two words");
    memory_set_low_ram_view_old(0);
    memory_set_low_ram_view_filtered(1);
    check_view_is(kept_low, "after SWL and SWR", "a recording of a build that dropped stores");
    memory_set_low_ram_view_filtered(0);
    check(digest_ram_part() == crc_of_ram(), "after SWL and SWR: switches off, the digest is of RAM");
    check(memory_low_ram_older_dropped_stores() == 14,
          "the count took no store that the older builds kept, and none in the source profile");

    puts("low RAM: game entry keeps it, every guest store lands, both older-recording views exact, "
         "SWL/SWR limit unchanged");
    return 0;
}
