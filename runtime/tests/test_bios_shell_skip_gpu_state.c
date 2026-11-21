#ifdef NDEBUG
#undef NDEBUG
#endif

/* The skipped boot leaves the GPU registers the shell leaves (PS1G-96).
 *
 * SaGa Frontier (USA) never issues GP1(03h) in its boot, intro or title
 * screen. With the shell skipped and the GPU left in its reset state the
 * display stayed off: the game ran and played sound with no picture.
 *
 * Drives the real bios_hle.c hook against the real gpu.c. The expected
 * values are the ones read from the running shells of the three images at
 * hand-over (GPUSTAT, the display registers and the draw area, sampled over
 * the debug server with the shell run in HLE and in LLE):
 *
 *   SCPH-1001, SCPH-5500   display on, start (0,2), H 608..3168, V 16..255,
 *                          GPUSTAT bits 16-22 = 0x4E, bits 0-12 = 0x060B,
 *                          draw area (0,2)-(639,479), draw offset (0,2)
 *   SCPH-5552              the same with H 638..3198, V 43..282 and
 *                          GPUSTAT bits 16-22 = 0x5E (PAL)
 *
 * Checks:
 *  - the shell skip on each measured image leaves exactly that state, through
 *    guest stores to GP0/GP1 and nothing else;
 *  - an image with no measured row keeps the reset state, byte for byte;
 *  - the skip fires once, and not at all with the skip off or after game
 *    entry, and then writes nothing.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

#include "../src/bios_hle.c"

#define SHELL_ENTRY 0x00030000u

/* ---- what bios_hle.c links against outside this fixture ---- */
PsxBiosImageInfo psx_bios_image;
static int fixture_game_started;
int fntrace_is_game_started(void) { return fixture_game_started; }
void psx_dispatch_call(CPUState* cpu, uint32_t target, uint32_t ret) {
    (void)cpu; (void)target; (void)ret;
    assert(!"the shell skip dispatches no guest code");
}
/* The cycle charge of the B0 services (psx_cycles.h); the shell skip charges
 * nothing, so reaching the clock here is a failure. */
int g_event_step_conservative, g_ls_replay_active, psx_in_device_service;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
void psx_advance_cycles_slow(uint32_t cycles) {
    (void)cycles;
    assert(!"the shell skip charges no guest time");
}
void psx_devices_service_to_now(void) {
    assert(!"the shell skip services no device");
}

/* ---- guest bus: only the GPU ports may be written ---- */
static unsigned gp0_stores, gp1_stores, other_stores;
static uint32_t bus_read_word(uint32_t addr) { (void)addr; return 0u; }
static void bus_write_word(uint32_t addr, uint32_t value) {
    if (addr == 0x1F801810u) { gp0_stores++; gpu_write_gp0(value); }
    else if (addr == 0x1F801814u) { gp1_stores++; gpu_write_gp1(value); }
    else other_stores++;
}

typedef struct {
    const char* image_id;
    uint32_t h1, h2, v1, v2;
    uint32_t stat_16_22;
} Measured;

static const Measured k_measured[] = {
    { "SCPH-1001", 608u, 3168u, 16u, 255u, 0x4Eu },
    { "SCPH-5500", 608u, 3168u, 16u, 255u, 0x4Eu },
    { "SCPH-5552", 638u, 3198u, 43u, 282u, 0x5Eu },
};

static uint8_t* snapshot(uint32_t* n) {
    *n = gpu_snapshot_bytes();
    uint8_t* buf = (uint8_t*)malloc(*n);
    assert(buf);
    gpu_snapshot_write(buf);
    return buf;
}

static void power_on(CPUState* cpu, const char* image_id) {
    memset(cpu, 0, sizeof *cpu);
    cpu->read_word = bus_read_word;
    cpu->write_word = bus_write_word;
    cpu->gpr[2] = 0xDEADBEEFu;
    gpu_reset_state(1);
    gp0_stores = gp1_stores = other_stores = 0;
    fixture_game_started = 0;
    memset(&psx_bios_image, 0, sizeof psx_bios_image);
    psx_bios_image.shell_entry_phys = SHELL_ENTRY;
    psx_bios_image.image_id = image_id;
}

static void expect_reset_display(void) {
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    assert(di.disabled == 1);
    assert((gpu_read_gpustat() & 0x00FF1FFFu) == 0x00800000u);
}

static void expect_measured(const Measured* m) {
    GpuDisplayInfo di;
    GpuDrawArea da;
    uint32_t x1, x2, y1, y2, hr1, hr2;
    uint32_t stat = gpu_read_gpustat();
    gpu_get_display_info(&di);
    gpu_get_draw_area(&da);
    gpu_get_crtc_debug(&x1, &x2, &y1, &y2, &hr1, &hr2);
    printf("%s: disabled=%d start=(%u,%u) h=%u..%u v=%u..%u stat16_22=0x%02X stat0_12=0x%04X "
           "area=(%u,%u)-(%u,%u) offset=(%d,%d)\n", m->image_id, di.disabled,
           (unsigned)di.display_x, (unsigned)di.display_y, x1, x2, y1, y2,
           (stat >> 16) & 0x7Fu, stat & 0x1FFFu, da.left, da.top, da.right, da.bottom,
           da.offset_x, da.offset_y);
    assert(di.disabled == 0);                       /* the PS1G-96 fault */
    assert(((stat >> 23) & 1u) == 0u);
    assert(di.display_x == 0 && di.display_y == 2);
    assert(x1 == m->h1 && x2 == m->h2);
    assert(y1 == m->v1 && y2 == m->v2);
    assert(((stat >> 16) & 0x7Fu) == m->stat_16_22);
    assert((stat & 0x1FFFu) == 0x060Bu);
    assert(da.left == 0 && da.top == 2 && da.right == 639 && da.bottom == 479);
    assert(da.offset_x == 0 && da.offset_y == 2);
    assert(gp0_state == GP0_IDLE);
}

int main(void) {
    CPUState cpu;

    /* Each measured image: the skip leaves the measured state. */
    for (size_t i = 0; i < sizeof k_measured / sizeof k_measured[0]; i++) {
        power_on(&cpu, k_measured[i].image_id);
        psx_bios_hle_configure(0, 1);
        assert(g_psx_bios_hle_hook != NULL);
        expect_reset_display();
        assert(g_psx_bios_hle_hook(&cpu, SHELL_ENTRY) == 1);
        assert(cpu.gpr[2] == 0u);
        expect_measured(&k_measured[i]);
        assert(other_stores == 0);

        /* One shot: a second shell dispatch runs the shell and writes nothing. */
        unsigned g0 = gp0_stores, g1 = gp1_stores;
        gpu_write_gp1(0x03000001u);
        assert(g_psx_bios_hle_hook(&cpu, SHELL_ENTRY) == 0);
        assert(gp0_stores == g0 && gp1_stores == g1 && other_stores == 0);

        /* Rematch re-arms the skip; it leaves the same state again. */
        gpu_reset_state(1);
        psx_bios_hle_configure(0, 1);
        assert(g_psx_bios_hle_hook(&cpu, SHELL_ENTRY) == 1);
        expect_measured(&k_measured[i]);
    }

    /* An image with no measured row: the skip fires and the GPU keeps its
     * reset state, byte for byte. */
    {
        static const char* const unmeasured[] = { "OpenBIOS", "SCPH-7000", NULL };
        for (size_t i = 0; i < sizeof unmeasured / sizeof unmeasured[0]; i++) {
            uint32_t n0, n1;
            power_on(&cpu, unmeasured[i]);
            psx_bios_hle_configure(0, 1);
            uint8_t* before = snapshot(&n0);
            assert(g_psx_bios_hle_hook(&cpu, SHELL_ENTRY) == 1);
            uint8_t* after = snapshot(&n1);
            assert(n0 == n1 && memcmp(before, after, n0) == 0);
            assert(gp0_stores == 0 && gp1_stores == 0 && other_stores == 0);
            expect_reset_display();
            free(before);
            free(after);
        }
    }

    /* After game entry the hook does not skip and does not write. */
    power_on(&cpu, "SCPH-1001");
    psx_bios_hle_configure(0, 1);
    fixture_game_started = 1;
    assert(g_psx_bios_hle_hook(&cpu, SHELL_ENTRY) == 0);
    assert(gp0_stores == 0 && gp1_stores == 0 && other_stores == 0);
    expect_reset_display();

    /* Skip off (the real intro): no hook, nothing written. */
    power_on(&cpu, "SCPH-1001");
    psx_bios_hle_configure(0, 0);
    assert(g_psx_bios_hle_hook == NULL);
    expect_reset_display();

    printf("bios_shell_skip_gpu_state_test: OK\n");
    return 0;
}
