/* CD register read sample order, through the production memory map and load
 * timing (PS1B-173). No text of memory.c is read: the unit is compiled whole.
 *
 * A CPU load from a CD register (1F801800-1F80180F) waits six cycles on the
 * bus for each byte of its width. The rule held in place here:
 *   default profile: the register is sampled after that wait;
 *   source profile:  the register is sampled before that wait, so a response
 *                    that arrives during the wait is not seen by this read.
 * In both profiles the load takes the same time and gives back the same
 * number of cycles to the instructions after it.
 *
 * The fixture supplies the clock, a scheduler with one CD event and a CD
 * register that reads 1 from the cycle of that event on. It records the guest
 * cycle at which the memory map brings the devices up to date for the read,
 * the cycle at which the CD register is read, and the value the load returns.
 * GCC -fwhole-program removes the device dependencies these loads do not reach. */
#include "../src/memory.c"

/* The guest clock and the deadline bookkeeping that psx_cycles.c owns. */
uint64_t psx_cycle_count, psx_next_service_cycle, g_psx_cycle_fast_limit, g_psx_device_gen;
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active, g_psx_cyc_bb_defer;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit, *g_psx_cyc_local_acc, g_dma_cpu_read_wait;
void psx_advance_cycles_slow(uint32_t cycles) { (void)cycles; abort(); }

/* One CD event. A register read returns 1 once the event has been serviced. */
static uint64_t event_due, sync_cycle, cd_cycle;
static unsigned event_seen, syncs, cd_reads;
static uint32_t cd_addr;
void psx_devices_service_to_now(void) {
    if (psx_cycle_count >= event_due) event_seen = 1;
    psx_next_service_cycle = event_seen ? psx_cycle_count + 16384u : event_due;
}
void psx_devices_mmio_sync(void) {
    g_psx_device_gen++;
    psx_cyc_batch_flush();
    psx_devices_service_to_now();
    sync_cycle = psx_cycle_count;
    syncs++;
}
uint32_t cdrom_read(uint32_t addr) {
    cd_addr = addr;
    cd_cycle = psx_cycle_count;
    cd_reads++;
    return event_seen;
}
void cdrom_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; abort(); }

static int source_active;
int source_gpu_runtime_active(void) { return source_active; }
int timers_source_raster_enabled(void) { return 0; }
int timers_source_hblank_counter_read(uint32_t a) { (void)a; return 0; }
uint32_t dma_cpu_read_penalty(void) { return 0; }
void debug_server_trace_mmio_read(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_mmio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }

/* Devices and tracing the memory map reaches but these loads do not use. */
uint32_t g_debug_current_func_addr, g_debug_last_store_pc;
int g_dma_exec_depth, g_ls_mode, g_ls_suppress_record, g_ram_read_watch_active;
volatile int g_ds_recording;
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

static const struct { const char *name; unsigned width, coprocessor; uint32_t reg; } loads[] = {
    {"byte status", 1, 0, 0x0}, {"byte response", 1, 0, 0x1}, {"byte data", 1, 0, 0x2},
    {"byte interrupt", 1, 0, 0x3}, {"byte mirror", 1, 0, 0xF}, {"half", 2, 0, 0x0},
    {"half upper", 2, 0, 0x2}, {"word", 4, 0, 0x0}, {"word mirror", 4, 0, 0xC}, {"LWC2", 4, 1, 0x0},
};
static const uint32_t segments[] = {0x00000000u, 0x80000000u, 0xA0000000u};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { if (failures++ < 24) { \
    printf("FAIL %s %s at %08X, event %u cycles after the start, %s: ", \
           source_active ? "source" : "default", loads[k].name, (unsigned)addr, offset, \
           pending ? "after a load" : "after no load"); \
    printf(__VA_ARGS__); putchar('\n'); } } } while (0)

int main(void) {
    const uint64_t start = 1000;
    unsigned cases = 0;
    for (source_active = 0; source_active < 2; ++source_active)
    for (size_t segment = 0; segment < sizeof segments / sizeof segments[0]; ++segment)
    for (size_t k = 0; k < sizeof loads / sizeof loads[0]; ++k)
    for (unsigned pending = 0; pending < 2; ++pending)
    for (unsigned offset = 0; offset <= 34; ++offset) {
        const unsigned width = loads[k].width, coprocessor = loads[k].coprocessor;
        const uint32_t addr = segments[segment] | 0x1F801800u | loads[k].reg;
        /* The instruction before this one either was a load to r5 (pending)
         * or was not a load. Only the second case costs the two extra cycles. */
        CPUState cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.ld_which_t = pending ? 5 : 32;
        cpu.read_fudge = pending ? 5 : 32;
        psx_cycle_count = start;
        g_psx_cyc_batch = g_psx_cyc_batch_limit = 0;
        event_due = start + offset;
        event_seen = 0;
        psx_devices_service_to_now();
        syncs = cd_reads = 0;
        sync_cycle = cd_cycle = 0;

        /* The entries that generated code and the interpreter call. */
        uint32_t value = coprocessor ? psx_cyc_lwc2_read(&cpu, addr) :
            width == 4 ? psx_cyc_load_word(&cpu, addr, 3, 0) :
            width == 2 ? psx_cyc_load_half(&cpu, addr, 3, 0) :
                         psx_cyc_load_byte(&cpu, addr, 3, 0);
        psx_cyc_batch_flush();

        /* A CPU load pays the instruction's own cycle first. The LWC2 entry
         * starts at the memory access. Completion is 2 cycles, 1 for LWC2. */
        const unsigned base = coprocessor ? 0 : 1, extra = pending ? 0 : 2;
        const unsigned completion = coprocessor ? 1 : 2;
        const uint64_t want_sample = start + base + extra + (source_active ? 0 : 6 * width);
        const uint64_t want_end = start + base + extra + 6 * width + completion;
        CHECK(syncs == 1 && sync_cycle == want_sample,
              "devices brought up to date %u time(s), last at cycle %llu, want once at %llu",
              syncs, (unsigned long long)sync_cycle, (unsigned long long)want_sample);
        /* A 16-bit read of the CD block does not reach the CD registers in
         * this memory map; the other widths do. */
        if (width != 2)
            CHECK(cd_reads == 1, "CD register read %u time(s), want once", cd_reads);
        if (cd_reads) {
            CHECK(cd_cycle == want_sample && cd_addr == (0x1F801800u | (loads[k].reg & 3u)),
                  "CD register %08X sampled at cycle %llu, want %08X at %llu", (unsigned)cd_addr,
                  (unsigned long long)cd_cycle, (unsigned)(0x1F801800u | (loads[k].reg & 3u)),
                  (unsigned long long)want_sample);
            CHECK(value == (event_due <= want_sample), "read %u, want %u", (unsigned)value,
                  (unsigned)(event_due <= want_sample));
        }
        CHECK(psx_cycle_count == want_end, "load ended at cycle %llu, want %llu",
              (unsigned long long)psx_cycle_count, (unsigned long long)want_end);
        CHECK(cpu.ld_absorb == 6 * width + completion, "gives back %u cycles, want %u",
              (unsigned)cpu.ld_absorb, 6 * width + completion);
        cases++;
    }
    if (failures) {
        printf("%d failure(s) in %u CD register loads\n", failures, cases);
        return 1;
    }
    printf("PASS %u CD register loads: widths, registers, mirrors, segments, LWC2, "
           "both profiles, with the event at every cycle of the load\n", cases);
    return 0;
}
