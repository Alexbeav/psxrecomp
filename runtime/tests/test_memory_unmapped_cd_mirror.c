/* Unmapped I/O and expansion reads [ORACLE FIXTURE R2] and the CD register
 * mirror across 1F801800-180F, through the production memory map, in the
 * default and the source profile.
 * GCC -O2 -fwhole-program removes unrelated device dependencies. */
#include "../src/memory.c"

static int source_active;
int source_gpu_runtime_active(void) { return source_active; }
int timers_source_raster_enabled(void) { return 0; }
void psx_devices_mmio_sync(void) {}
void debug_server_trace_mmio_read(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_mmio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
static uint32_t cd_last_read, cd_last_write, cd_last_value;
uint32_t cdrom_read(uint32_t addr) { cd_last_read = addr; return addr == 0x1F801800u ? 0x18u : 0x5Au; }
void cdrom_write(uint32_t addr, uint32_t val) { cd_last_write = addr; cd_last_value = val; }

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
void psx_irq_refresh_cause_ip2(void) {}
void sio_card_handoff_on_imask(uint32_t o, uint32_t n) { (void)o; (void)n; }
void sio_tick(int c) { (void)c; }
uint32_t dma_read(uint32_t a) { (void)a; abort(); }
uint32_t sio_read(uint32_t a) { (void)a; abort(); }
uint32_t spu_read(uint32_t a) { (void)a; abort(); }
uint32_t timers_read(uint32_t a) { (void)a; abort(); }
uint32_t mdec_read(uint32_t a) { (void)a; abort(); }
uint32_t gpu_read_gpuread(void) { abort(); }
uint32_t gpu_read_gpustat(void) { abort(); }
void dma_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void dma_write_masked(uint32_t a, uint32_t v, uint32_t m) { (void)a; (void)v; (void)m; abort(); }
void mdec_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void spu_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void timers_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void sio_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void gpu_write_gp0(uint32_t v) { (void)v; abort(); }
void gpu_write_gp1(uint32_t v) { (void)v; abort(); }
void gpu_set_gp0_source(uint32_t a) { (void)a; abort(); }

/* [ORACLE FIXTURE R2, Octoshock 2.3]: address, value read (both patterns agree, no exception). */
static const struct { uint32_t addr, value; } r2_rows[] = {    {0x1F801024u,0x00000000u},
    {0x1F801028u,0x00000000u},
    {0x1F80102Cu,0x00000000u},
    {0x1F801030u,0x00000000u},
    {0x1F80103Cu,0x00000000u},
    {0x1F801064u,0x00000000u},
    {0x1F801068u,0x00000000u},
    {0x1F80106Cu,0x00000000u},
    {0x1F801078u,0x00000000u},
    {0x1F80107Cu,0x00000000u},
    {0x1F801130u,0x00000000u},
    {0x1F801140u,0x00000000u},
    {0x1F801200u,0x00000000u},
    {0x1F801400u,0x00000000u},
    {0x1F8017FCu,0x00000000u},
    {0x1F801804u,0x00000018u},
    {0x1F801808u,0x00000018u},
    {0x1F80180Cu,0x00000018u},
    {0x1F801818u,0x00000000u},
    {0x1F80181Cu,0x00000000u},
    {0x1F801828u,0x00000000u},
    {0x1F801830u,0x00000000u},
    {0x1F801900u,0x00000000u},
    {0x1F801A00u,0x00000000u},
    {0x1F801BFCu,0x00000000u},
    {0x1F802000u,0x00000000u},
    {0x1F802004u,0x00000000u},
    {0x1F802020u,0x00000000u},
    {0x1F802040u,0x00000000u},
    {0x1F802080u,0x00000000u},
    {0x1F803000u,0x00000000u},
    {0x1F000000u,0x00000000u},
    {0x1F000004u,0x00000000u},
    {0x1F000080u,0x00000000u},
    {0x1FA00000u,0x00000000u},
};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s line %d: ", source_active ? "source" : "default", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int expansion1(uint32_t addr) { return addr >= 0x1F000000u && addr <= 0x1F7FFFFFu; }

int main(void) {
    for (source_active = 0; source_active < 2; ++source_active) {
        for (size_t i = 0; i < sizeof r2_rows / sizeof r2_rows[0]; ++i) {
            uint32_t a = r2_rows[i].addr, want = r2_rows[i].value;
            /* The default path keeps PSX-SPX's open-bus FFh for expansion 1
             * [NOT OBSERVED: release policy; R2 reads 0]. */
            if (!source_active && expansion1(a)) want = 0xFFFFFFFFu;
            uint32_t got = psx_read_word(0xA0000000u | a);
            CHECK(got == want, "R2 %08X: %08X != %08X", a, got, want);
        }
        /* The BIOS expansion-ROM probe word. */
        uint32_t probe = psx_read_word(0xBF000084u);
        CHECK(probe == (source_active ? 0u : 0xFFFFFFFFu), "1F000084 probe %08X", probe);
        CHECK(psx_read_half(0xBF000084u) == (source_active ? 0u : 0xFFFFu), "1F000084 half");
        CHECK(psx_read_byte(0xBF000084u) == (source_active ? 0u : 0xFFu), "1F000084 byte");
        /* The CD block mirrors at every width that reaches it. */
        for (uint32_t m = 0; m < 16; ++m) {
            uint32_t a = 0x1F801800u + m;
            cd_last_read = 0;
            (void)psx_read_byte(0xA0000000u | a);
            CHECK(cd_last_read == (0x1F801800u | (m & 3u)), "byte read %08X -> %08X", a, cd_last_read);
            cd_last_write = 0;
            psx_write_byte(0xA0000000u | a, (uint8_t)(0x40u + m));
            CHECK(cd_last_write == (0x1F801800u | (m & 3u)) && cd_last_value == 0x40u + m,
                  "byte write %08X -> %08X", a, cd_last_write);
            if (m & 3u) continue;
            cd_last_write = 0;
            psx_write_word(0xA0000000u | a, 0x11u);
            CHECK(cd_last_write == 0x1F801800u, "word write %08X -> %08X", a, cd_last_write);
        }
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: R2 unmapped reads and the CD register mirror in both profiles");
    return 0;
}