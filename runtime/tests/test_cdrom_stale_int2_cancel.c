/* PS1B-454 part A row 2: a new command cancels the second response that the
 * previous command still owes (PSX-CD-007).
 *
 * The fix is 2d67bb9a7. A Pause sent during a read owes its INT2 about 1.1
 * million cycles later. The game sends Setmode, Setloc and ReadN before that.
 * Without the cancel, the Pause's INT2 fires in the middle of the new read as
 * a stale COMPLETE. LIBDS drivers take that for a read failure: Medal of
 * Honor Underground mission 1 locked in a DsRead retry loop, and Gran
 * Turismo's race loading shows the same overlap during ReadS.
 *
 * Rows:
 *   S0  Control: a Pause with no later command does deliver its INT2. This
 *       shows the test sees a Pause's INT2 when one is owed.
 *   S1  Pause from a read, then Setmode, Setloc and ReadN before the Pause's
 *       INT2 is due. Run 2 million cycles past the old deadline, taking every
 *       interrupt: no INT2 comes, and the first interrupt after ReadN's ACK
 *       is the read's INT1 for the new target.
 *   S2  Pause from a read, then GetStat (one response only). Run past the old
 *       deadline: no INT2 comes.
 *
 * Controller-level, default path, through the production cdrom.c and its MMIO
 * interface. Synthetic sectors only; no BIOS or game data. */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
static int reads, failures, irq_raises;
void psx_irq_raise(uint32_t a, uint32_t b) { (void)b; i_stat |= 1u << a; irq_raises++; }
void event_ring_record(uint16_t a, uint8_t b) { (void)a; (void)b; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void audio_trace_event(uint16_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
void spu_cd_audio_push(const int16_t *a, int b) { (void)a; (void)b; }
void spu_cd_audio_reset(void) {}
int psx_netplay_active(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
void *iso_open(const char *p) { (void)p; return (void *)1; }
void iso_close(void *p) { (void)p; }
int iso_read_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; memset(b, (int)(lba & 255u), (size_t)n); reads++; return 1;
}
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba; (void)b; (void)n; return 0;
}
int iso_read_subq(void *p, uint32_t lba, uint8_t *b, int n, int *v) {
    (void)p; (void)lba; (void)b; (void)n; *v = 0; return 0;
}
int iso_has_subq_replacements(void *p) { (void)p; return 0; }
uint32_t iso_sector_count(void *p) { (void)p; return 333000; }
int iso_track_count(void *p) { (void)p; return 1; }
uint32_t iso_track_start_lba(void *p, int t) { (void)p; (void)t; return 0; }
uint32_t iso_track_pregap_lba(void *p, int t) { (void)p; (void)t; return 0; }
int iso_track_is_audio(void *p, int t) { (void)p; (void)t; return 0; }

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)
static void ack(void) {
    cdrom_write(0x1f801800, 1); cdrom_write(0x1f801803, 0x1f);
    cdrom_write(0x1f801800, 0);
}
static void command(uint8_t c) { cdrom_write(0x1f801800, 0); cdrom_write(0x1f801801, c); }
static void param(uint8_t p) { cdrom_write(0x1f801800, 0); cdrom_write(0x1f801802, p); }
static void advance(int n) { psx_cycle_count += (uint32_t)n; cdrom_advance((uint32_t)n); }
static void set_model(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
/* Setloc through the registers, ACK taken. */
static void target(int lba) {
    int n = lba + 150;
    param(bin_to_bcd((uint8_t)(n / 4500)));
    param(bin_to_bcd((uint8_t)(n / 75 % 60)));
    param(bin_to_bcd((uint8_t)(n % 75)));
    command(0x02);
    CHECK(irq_flag == CDIRQ_ACK, "Setloc is acknowledged");
    ack();
}
static void setmode(uint8_t mode) {
    param(mode); command(0x0E);
    CHECK(irq_flag == CDIRQ_ACK, "Setmode is acknowledged");
    ack();
}
static void default_path(void) {
    set_model("PSX_CD_FIRMWARE_MODEL", ""); set_model("PSX_CD_COLD_STATUS_MODEL", "");
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0; reads = 0;
    cdrom_init("synthetic");
    for (int i = 0; i < 2; i++) { ack(); command(0x01); ack(); }
}
static void wait_for(uint8_t irq) {
    for (int i = 0; i < 40000 && irq_flag != irq; i++) advance(1000);
}

/* Starts a double-speed read at `lba`, takes its first sector and sends a
 * Pause. Returns the cycle at which the Pause's INT2 is due. The Pause's ACK
 * has been taken. */
static uint64_t pause_during_read(int lba) {
    setmode(0x80);
    target(lba);
    command(0x06);
    CHECK(irq_flag == CDIRQ_ACK, "ReadN is acknowledged");
    ack(); wait_for(CDIRQ_DATA_READY);
    CHECK(irq_flag == CDIRQ_DATA_READY && reading, "the read delivers its first sector");
    ack();
    command(0x09);
    CHECK(irq_flag == CDIRQ_ACK, "Pause is acknowledged");
    CHECK(pending.pending && pending.cmd == 0x09, "the Pause owes its INT2");
    uint64_t due = pending.due_cyc;
    CHECK(due > psx_cycle_count + 500000, "a Pause from a read is due far ahead (about 1.1M cycles)");
    ack();
    return due;
}

/* Runs to `until` in steps of 1000 cycles, taking every interrupt. Counts the
 * INT2 and INT1 that arrive and records the first one after the start. */
struct run { int completes, data_ready, first; uint64_t first_at; };
static struct run run_to(uint64_t until) {
    struct run r = { 0, 0, 0, 0 };
    while (psx_cycle_count < until) {
        advance(1000);
        if (irq_flag) {
            if (!r.first) { r.first = irq_flag; r.first_at = psx_cycle_count; }
            if (irq_flag == CDIRQ_COMPLETE) r.completes++;
            if (irq_flag == CDIRQ_DATA_READY) r.data_ready++;
            ack();
        }
    }
    return r;
}

int main(void) {
    char what[200];

    /* S0: the control. */
    default_path();
    uint64_t due = pause_during_read(1000);
    struct run r = run_to(due + 2000000);
    snprintf(what, sizeof what, "S0: a Pause with no later command delivers one INT2 (got %d)", r.completes);
    CHECK(r.completes == 1 && r.first == CDIRQ_COMPLETE, what);

    /* S1: Setmode, Setloc and ReadN before the Pause's INT2 is due. */
    default_path();
    due = pause_during_read(1000);
    setmode(0x80);
    target(2000);
    command(0x06);
    CHECK(irq_flag == CDIRQ_ACK, "S1: the new ReadN is acknowledged");
    ack();
    CHECK(psx_cycle_count < due, "S1: the new commands come before the Pause's INT2 is due");
    r = run_to(due + 2000000);
    snprintf(what, sizeof what, "S1: no stale INT2 from the Pause arrives during the new read (got %d)",
             r.completes);
    CHECK(r.completes == 0, what);
    snprintf(what, sizeof what, "S1: the first interrupt after ReadN's ACK is the read's INT1 (got INT%d)",
             r.first);
    CHECK(r.first == CDIRQ_DATA_READY, what);
    CHECK(r.data_ready > 0 && reading && last_sector_lba >= 2000,
          "S1: the read streams sectors from the new target");

    /* S2: one single-response command before the Pause's INT2 is due. */
    default_path();
    due = pause_during_read(1000);
    command(0x01);
    CHECK(irq_flag == CDIRQ_ACK, "S2: GetStat is acknowledged");
    ack();
    r = run_to(due + 2000000);
    snprintf(what, sizeof what, "S2: after GetStat no INT2 from the Pause arrives (got %d)", r.completes);
    CHECK(r.completes == 0 && r.first == 0, what);

    (void)irq_raises;
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: a new command cancels the second response the previous one owed");
    return 0;
}
