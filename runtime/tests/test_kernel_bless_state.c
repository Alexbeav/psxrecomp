/* PS1B-306: kernel bless must work on tables larger than the old 4,096-row
 * cap, and an oversized table must be refused, never run without bless.
 *
 * The retail tables hold 5,054-5,083 rows since every interruptible ROM
 * instruction became a dispatch key (T110). The runtime used to turn bless off
 * for such a table without a word, so every relocated kernel routine ran in
 * the interpreter. This drives the real memory.c bless code with a synthetic
 * 5,054-row table: no BIOS image, game data or timing stubs. LTO discards the
 * unrelated memory-map paths, as in the other single-source fixtures.
 *
 *   (no argument)  rows beyond 4,096 bless; a patched body falls back to the
 *                  interpreter and returns once its bytes match again
 *   overflow       a table of PSX_KBLESS_MAX_ENTRIES + 1 rows stops the run
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/memory.c"

#define ROWS        5054u          /* SCPH1001's table size today */
#define WIN_LO      0x00000500u    /* relocated kernel window in RAM */
#define WIN_HI      0x00008500u
#define ROM_OFF     0x00010000u    /* its ROM source: 0x1FC10000 */
#define BODY_BYTES  0x100u         /* 64 four-byte keys share one body */

/* The backend selection normally publishes these (psx_bios_backend.c). */
static PsxKernelBody s_bodies[PSX_KBLESS_MAX_ENTRIES + 1u];
const PsxKernelBody *psx_bios_kernel_bodies = s_bodies;
uint32_t psx_bios_kernel_body_count = 0;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges = 0;
uint32_t psx_bios_kernel_patch_range_count = 0;
PsxBiosImageInfo psx_bios_image;

static uint32_t row_key(uint32_t i) { return WIN_LO + 4u * i; }
static uint32_t row_body(uint32_t i) { return WIN_LO + (i / 64u) * BODY_BYTES; }

static void build_table(uint32_t rows) {
    for (uint32_t i = 0; i < rows; ++i) {
        s_bodies[i].key = row_key(i);
        s_bodies[i].body_lo = row_body(i);
        s_bodies[i].body_hi = row_body(i) + BODY_BYTES;
    }
    psx_bios_kernel_body_count = rows;
    memset(&psx_bios_image, 0, sizeof(psx_bios_image));
    psx_bios_image.kbless_ram_lo = WIN_LO;
    psx_bios_image.kbless_ram_hi = WIN_HI;
    psx_bios_image.kbless_rom_off = ROM_OFF;
    /* The BIOS copies this ROM slice into the window at boot. */
    for (uint32_t i = 0; i < WIN_HI - WIN_LO; ++i)
        bios_rom[ROM_OFF + i] = (uint8_t)(i * 7u + 3u);
    memcpy(ram + WIN_LO, bios_rom + ROM_OFF, WIN_HI - WIN_LO);
    psx_kernel_bless_reset_for_boot();
}

static uint64_t stat(unsigned index) {
    uint64_t s[8];
    psx_kernel_bless_stats(s);
    return s[index];
}

int main(int argc, char **argv) {
    uint32_t entries = 0, capacity = 0;

    if (argc > 1 && strcmp(argv[1], "overflow") == 0) {
        build_table(PSX_KBLESS_MAX_ENTRIES + 1u);
        assert(!psx_kernel_bless_table_fits(&entries, &capacity));
        assert(entries == PSX_KBLESS_MAX_ENTRIES + 1u);
        fflush(stdout);
        (void)psx_kernel_bless_dispatchable(row_key(0));
        puts("kernel_bless_state: an oversized table was not refused");
        return 0; /* reaching here is the failure CTest's regex catches */
    }

    assert(PSX_KBLESS_MAX_ENTRIES >= ROWS);
    build_table(ROWS);
    assert(psx_kernel_bless_table_fits(&entries, &capacity));
    assert(entries == ROWS && capacity == PSX_KBLESS_MAX_ENTRIES);
    assert(psx_kernel_bless_state() == -1); /* not latched before a dispatch */

    /* A row past the old cap blesses; so does every other row. */
    assert(psx_kernel_bless_dispatchable(row_key(5000u)) == 1);
    assert(psx_kernel_bless_state() == 1);
    for (uint32_t i = 0; i < ROWS; ++i)
        assert(psx_kernel_bless_dispatchable(row_key(i)) == 1);
    assert(stat(0) == ROWS && stat(1) == ROWS && stat(2) == 0);

    /* Not a table key, and outside the window: never native. */
    assert(psx_kernel_bless_dispatchable(row_key(5000u) + 2u) == 0);
    assert(psx_kernel_bless_dispatchable(WIN_HI + 0x100u) == 0);

    /* A guest store into a body sends its rows back to the interpreter, and
     * only those rows. */
    const uint32_t patched = row_body(5000u) + 0x10u;
    ram[patched] ^= 0xFFu;
    kbless_note_write(patched);
    assert(psx_kernel_bless_dispatchable(row_key(5000u)) == 0);
    assert(psx_kernel_bless_dispatchable(row_key(4999u)) == 0);  /* same body */
    assert(psx_kernel_bless_dispatchable(row_key(100u)) == 1);   /* another body */
    assert(stat(2) == 2u);

    /* Restoring the ROM bytes makes the body native again. */
    ram[patched] ^= 0xFFu;
    kbless_note_write(patched);
    assert(psx_kernel_bless_dispatchable(row_key(5000u)) == 1);
    assert(stat(2) == 0);

    /* A bulk write over the window and a state restore re-verify every row. */
    psx_kernel_bless_note_range(WIN_LO, WIN_HI - WIN_LO);
    assert(stat(1) == 0);
    assert(psx_kernel_bless_dispatchable(row_key(ROWS - 1u)) == 1);
    psx_kernel_bless_resync_after_restore();
    assert(stat(1) == 0);
    assert(psx_kernel_bless_dispatchable(row_key(4097u)) == 1);

    /* PSX_KERNEL_BLESS=0 still turns it off after a re-boot latch. */
#ifdef _WIN32
    _putenv("PSX_KERNEL_BLESS=0");
#else
    setenv("PSX_KERNEL_BLESS", "0", 1);
#endif
    psx_kernel_bless_reset_for_boot();
    assert(psx_kernel_bless_dispatchable(row_key(5000u)) == 0);
    assert(psx_kernel_bless_state() == 0);

    printf("kernel_bless_state: PASS (%u rows, capacity %u)\n",
           (unsigned)ROWS, (unsigned)PSX_KBLESS_MAX_ENTRIES);
    return 0;
}
