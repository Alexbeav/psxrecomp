/* A 32-bit CPU access to the SPU register block is two 16-bit register
 * accesses, at addr and addr+2: the SPU sits on a 16-bit bus (psx-spx, SPU
 * overview; Beetle libretro.cpp splits a word store into Write(A|0, V) and
 * Write(A|2, V >> 16), a word load into Read(A) | Read(A|2) << 16).
 * PS1B-604: a word store of 00FF0000h to KOFF (1F801D8C) must reach the
 * KOFF-high register 1F801D8E with 00FFh, so voices 16-23 enter release.
 * Through the production memory map; the SPU is a recorder here.
 * GCC -O2 -fwhole-program removes unrelated device dependencies. */
#include "../src/memory.c"

void psx_devices_mmio_sync(void) {}
void debug_server_trace_mmio_read(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_mmio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }

/* SPU recorder: 16-bit registers, every access logged. */
static uint16_t spu_reg[0x200];
static struct { char rw; uint32_t addr, val; } spu_log[16];
static int spu_n;
static void spu_note(char rw, uint32_t a, uint32_t v) {
    if (spu_n < 16) { spu_log[spu_n].rw = rw; spu_log[spu_n].addr = a; spu_log[spu_n].val = v; }
    spu_n++;
}
uint32_t spu_read(uint32_t a) {
    uint32_t v = spu_reg[((a - 0x1F801C00u) >> 1) & 0x1FFu];
    spu_note('r', a, v);
    return v;
}
void spu_write(uint32_t a, uint32_t v) {
    spu_note('w', a, v);
    spu_reg[((a - 0x1F801C00u) >> 1) & 0x1FFu] = (uint16_t)v;
}

/* Devices and tracing the memory map reaches but these cases do not use. */
uint32_t g_debug_current_func_addr, g_debug_last_store_pc;
int g_dma_exec_depth, g_ls_mode, g_ls_suppress_record, g_ram_read_watch_active;
volatile int g_ds_recording;
uint64_t g_psx_device_gen;
void (*g_overlay_flush_pending_cycles)(void);
void debug_server_trace_ram_read_watch(uint32_t p, uint32_t v) { (void)p; (void)v; }
void ds_note_dma_write(void) {}
void ds_note_read(uint32_t a, uint32_t s) { (void)a; (void)s; }
void ds_note_write(uint32_t a, uint32_t s) { (void)a; (void)s; }
int fntrace_is_game_started(void) { return 0; }
uint32_t ls_read_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; return v; }
void ls_write_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; (void)v; }
int psx_get_in_exception(void) { return 0; }
int source_gpu_runtime_active(void) { return 0; }
void psx_irq_refresh_cause_ip2(void) {}
void sio_card_handoff_on_imask(uint32_t o, uint32_t n) { (void)o; (void)n; }
void sio_tick(int c) { (void)c; }
uint32_t cdrom_read(uint32_t a) { (void)a; abort(); }
void cdrom_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
uint32_t dma_read(uint32_t a) { (void)a; abort(); }
uint32_t sio_read(uint32_t a) { (void)a; abort(); }
uint32_t timers_read(uint32_t a) { (void)a; abort(); }
uint32_t mdec_read(uint32_t a) { (void)a; abort(); }
uint32_t gpu_read_gpuread(void) { abort(); }
uint32_t gpu_read_gpustat(void) { abort(); }
void dma_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void dma_write_masked(uint32_t a, uint32_t v, uint32_t m) { (void)a; (void)v; (void)m; abort(); }
void mdec_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void timers_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void sio_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void gpu_write_gp0(uint32_t v) { (void)v; abort(); }
void gpu_write_gp1(uint32_t v) { (void)v; abort(); }
void gpu_set_gp0_source(uint32_t a) { (void)a; abort(); }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define REG(a) spu_reg[((a) - 0x1F801C00u) >> 1]

int main(void) {
    /* KOFF word store: voices 16-23 live in the high halfword. */
    spu_n = 0;
    psx_write_word(0xA0000000u | 0x1F801D8Cu, 0x00FF0000u);
    CHECK(REG(0x1F801D8Eu) == 0x00FFu, "KOFF high 1F801D8E = %04X, want 00FF", REG(0x1F801D8Eu));
    CHECK(REG(0x1F801D8Cu) == 0x0000u, "KOFF low 1F801D8C = %04X, want 0000", REG(0x1F801D8Cu));
    CHECK(spu_n == 2, "word store made %d SPU writes, want 2", spu_n);
    CHECK(spu_n >= 1 && spu_log[0].addr == 0x1F801D8Cu && spu_log[0].val == 0x0000u,
          "first write %08X=%X, want 1F801D8C=0", spu_log[0].addr, spu_log[0].val);
    CHECK(spu_n >= 2 && spu_log[1].addr == 0x1F801D8Eu && spu_log[1].val == 0x00FFu,
          "second write %08X=%X, want 1F801D8E=FF", spu_log[1].addr, spu_log[1].val);

    /* KON word store reaches both halves through the KSEG0 and KUSEG maps. */
    psx_write_word(0x80000000u | 0x1F801D88u, 0x00AB1234u);
    CHECK(REG(0x1F801D88u) == 0x1234u && REG(0x1F801D8Au) == 0x00ABu,
          "KON %04X/%04X, want 1234/00AB", REG(0x1F801D88u), REG(0x1F801D8Au));

    /* ENDX word load returns both halves (voices 16-23 in bits 16-23). */
    REG(0x1F801D9Cu) = 0x8001u; REG(0x1F801D9Eu) = 0x0042u;
    spu_n = 0;
    uint32_t endx = psx_read_word(0xA0000000u | 0x1F801D9Cu);
    CHECK(endx == 0x00428001u, "ENDX word load %08X, want 00428001", endx);
    CHECK(spu_n == 2 && spu_log[0].addr == 0x1F801D9Cu && spu_log[1].addr == 0x1F801D9Eu,
          "word load made %d reads (%08X, %08X), want 1F801D9C then 1F801D9E",
          spu_n, spu_log[0].addr, spu_log[1].addr);

    /* A voice register word store: volume L then volume R of voice 0. */
    psx_write_word(0xA0000000u | 0x1F801C00u, 0x3FFF2ABCu);
    CHECK(REG(0x1F801C00u) == 0x2ABCu && REG(0x1F801C02u) == 0x3FFFu,
          "voice 0 volume %04X/%04X, want 2ABC/3FFF", REG(0x1F801C00u), REG(0x1F801C02u));


    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: a word access to the SPU is two halfword register accesses");
    return 0;
}

/* No render pass is active; an isolated-cache store is outside these cases. */
int g_psx_render_pass_active;
int render_pass_mmio_read_allowed(uint32_t phys, uint32_t width) { (void)phys; (void)width; return 1; }
#include "render_pass_plan.h"
int render_pass_store_to(const RenderPassStoreTarget *target, uint32_t address,
                        uint32_t value, uint32_t width) {
    (void)target; (void)address; (void)value; (void)width; abort();
}
void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) { (void)biu; (void)addr; (void)value; abort(); }

#include "../src/psx_ram_geometry.c"
