/* PS1B-454 part A row 1: GetID reports the inserted disc's licence region.
 *
 * The fix is b82e72cb0 (Kula World). GetID's second response ends with the
 * four-byte licence string of the disc in the drive. It was hardcoded "SCEI"
 * (NTSC-J). Kula World (PAL, SCES-01000) runs the BIOS CD driver, which
 * revalidates the disc with ReadTOC and GetID after the first file load; a
 * "Japanese" answer threw it into an endless GetStat/Init retry loop and the
 * game stood on a black screen at sector 307.
 *
 * Rows:
 *   R1  Without a call to cdrom_set_disc_scex the drive answers "SCEA", the
 *       region of the default BIOS (SCPH1001, NTSC-U).
 *   R2  For each of SCEE, SCEI and SCEA set through cdrom_set_disc_scex,
 *       GetID answers INT3, then INT2 with eight bytes; bytes 4 to 7 are the
 *       string that was set.
 *   R3  The region reaches the drive at launch, at a disc swap and at a
 *       save-state disc mount: main.cpp maps PAL to SCEE, NTSC-J to SCEI and
 *       NTSC-U to SCEA in each place. This row reads main.cpp as text (it
 *       proves the mapping is present, not that it runs), because main.cpp
 *       needs the whole runtime to link.
 *
 * Controller-level, through the production cdrom.c and its MMIO interface.
 * Synthetic sectors only; no BIOS or game data.
 *
 * usage: cdrom_getid_region_test <runtime/src/main.cpp> */
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
static void advance(int n) { psx_cycle_count += (uint32_t)n; cdrom_advance((uint32_t)n); }
static void set_model(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

/* GetID through the registers: INT3, then INT2 with eight bytes. Writes the
 * second response to reply and returns 1 when both responses came. */
static int get_id(uint8_t reply[8]) {
    ack(); command(0x1A);
    if (irq_flag != CDIRQ_ACK) return 0;
    ack();
    for (int i = 0; i < 400 && irq_flag == 0; i++) advance(1000);
    if (irq_flag != CDIRQ_COMPLETE || response_count - response_read != 8) { ack(); return 0; }
    for (int i = 0; i < 8; i++) reply[i] = (uint8_t)cdrom_read(0x1f801801);
    ack();
    return 1;
}

static void fresh_drive(void) {
    set_model("PSX_CD_FIRMWARE_MODEL", ""); set_model("PSX_CD_COLD_STATUS_MODEL", "");
    set_model("PSX_CD_EXPLICIT_SEEK_MODEL", ""); set_model("PSX_CD_TOC_SEEK_MODEL", "");
    set_model("PSX_CD_READ_START_MODEL", ""); set_model("PSX_CD_SOURCE_CLOCK_TAPE", "");
    set_model("PSX_CD_DRIVE_MODEL", "");
    psx_cycle_count = 0; i_stat = 0;
    cdrom_init("synthetic");
    /* Two GetStat commands take the cold shell-open latch away, as the BIOS
     * does before it asks for the disc's identity. */
    for (int i = 0; i < 2; i++) { ack(); command(0x01); ack(); }
}

static void check_region(const char *scex, int set) {
    char what[160];
    uint8_t reply[8] = { 0 };
    if (set) cdrom_set_disc_scex(scex);
    fresh_drive();
    snprintf(what, sizeof what, "%s: GetID answers INT3 and then INT2 with eight bytes", scex);
    CHECK(get_id(reply), what);
    snprintf(what, sizeof what, "%s: the disc is licensed (byte 1 is 00h, byte 2 is 20h), got %02X %02X",
             scex, reply[1], reply[2]);
    CHECK(reply[1] == 0x00 && reply[2] == 0x20, what);
    snprintf(what, sizeof what, "%s%s: bytes 4 to 7 are the licence string, got \"%c%c%c%c\"",
             set ? "R2 " : "R1 default ", scex, reply[4], reply[5], reply[6], reply[7]);
    CHECK(memcmp(reply + 4, scex, 4) == 0, what);
}

/* R3. Each of the three places that tells the drive the disc's region names
 * the three regions with the right string. A place is a line holding the
 * region test; the string must follow within the next few lines. */
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(text); return NULL; }
    text[n] = 0;
    fclose(f);
    return text;
}
/* Counts the places where `test` is followed, within `span` characters, by
 * `string`. */
static int mapped(const char *text, const char *test, const char *string, int span) {
    int places = 0;
    for (const char *at = strstr(text, test); at; at = strstr(at + 1, test)) {
        const char *hit = strstr(at, string);
        if (hit && hit - at <= span) places++;
    }
    return places;
}
static void check_mapping(const char *main_path) {
    char what[200];
    char *text = read_text(main_path);
    CHECK(text != NULL, "R3: main.cpp can be read");
    if (!text) return;
    static const struct { const char *region, *scex; } rows[3] = {
        { "\"PAL\"", "SCEE" }, { "\"NTSC-J\"", "SCEI" }, { "\"NTSC-U\"", "SCEA" },
    };
    for (int i = 0; i < 3; i++) {
        char quoted[8];
        snprintf(quoted, sizeof quoted, "\"%s\"", rows[i].scex);
        char launch[64], swap[64], mount[64];
        snprintf(launch, sizeof launch, "ident.region == %s", rows[i].region);
        snprintf(swap, sizeof swap, "identity.region == %s", rows[i].region);
        snprintf(mount, sizeof mount, "region == %s) return", rows[i].region);
        snprintf(what, sizeof what, "R3 launch: %s sets %s through cdrom_set_disc_scex",
                 rows[i].region, rows[i].scex);
        char call[48];
        snprintf(call, sizeof call, "cdrom_set_disc_scex(%s)", quoted);
        CHECK(mapped(text, launch, call, 120) == 1, what);
        snprintf(what, sizeof what, "R3 disc swap: %s copies %s", rows[i].region, rows[i].scex);
        CHECK(mapped(text, swap, quoted, 80) == 1, what);
        snprintf(what, sizeof what, "R3 save-state mount: %s returns %s", rows[i].region, rows[i].scex);
        CHECK(mapped(text, mount, quoted, 40) == 1, what);
    }
    CHECK(strstr(text, "disc_scex_for_region(id.region)") != NULL,
          "R3 save-state mount: the mount passes disc_scex_for_region to the drive");
    free(text);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <runtime/src/main.cpp>\n", argv[0]); return 64; }
    check_region("SCEA", 0);
    check_region("SCEE", 1);
    check_region("SCEI", 1);
    check_region("SCEA", 1);
    check_region("SCEE", 1);
    check_mapping(argv[1]);
    (void)reads; (void)irq_raises;
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    puts("PASS: GetID answers the licence string of the disc in the drive");
    return 0;
}
