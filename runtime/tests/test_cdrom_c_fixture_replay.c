/* CD controller replay of the C1-C9, C2b-C3b and C8b oracle fixtures
 * [ORACLE FIXTURE C1-C9, C2b-C3b, C8b, Octoshock 2.3] through the production
 * cdrom.c MMIO interface, per pad-free CD profile.
 *
 * Each case starts from a fresh power-on and replays the fixture program's own
 * input schedule (command writes, acknowledges, data requests) at the logged
 * CPU cycle. The authored disc is rebuilt in memory from the fixture driver's
 * formulas. The controller's status | int_flag << 8 is sampled every SAMPLE
 * cycles; each oracle change row must be matched, in order, by a change to the
 * same value within TOLERANCE cycles, and each oracle response must carry the
 * same bytes.
 *
 * usage: cdrom_c_fixture_replay_test <cd_c_fixture_replay.txt> <profile> [tape]
 *        [--classes C3,C4,...] [--verbose]
 * profile: default | tekken (octoshock-2.2.2 sub-models) | pepsiman (tekken
 * plus octoshock-2.3 CD-DA) | route (pepsiman plus the nymashock-1.29.0
 * drive; the Octoshock rows' ack schedule does not fit that drive).
 * --case NAME runs one case (a model that rejects an unqualified path exits
 * the process, so reports run one case per process).
 * Exit status: 0 when every case of the selected classes matches. */
#include "../src/cdrom.c"
#include <stdarg.h>

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
void psx_irq_raise(uint32_t a, uint32_t b) { (void)b; i_stat |= 1u << a; }
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

/* ---- the authored disc (fixture driver formulas) ------------------------ */
#define XA0 300u
#define XAN 64u
#define AU0 60000u
#define AUN 450u
static uint8_t iso_user[4][2048];
static uint8_t tobcd(uint32_t v) { return (uint8_t)((v / 10u) << 4 | (v % 10u)); }
static void msf(uint32_t lba, uint8_t *o) { o[0] = tobcd(lba / 4500u); o[1] = tobcd(lba / 75u % 60u); o[2] = tobcd(lba % 75u); }
static uint16_t crc16(const uint8_t *d, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) {
        r ^= (uint32_t)d[i] << 8;
        for (int k = 0; k < 8; ++k) r = (r & 0x8000u) ? ((r << 1) ^ 0x1021u) & 0xFFFFu : (r << 1) & 0xFFFFu;
    }
    return (uint16_t)(r ^ 0xFFFFu);
}
static void subq(uint32_t lba, uint8_t *q) {
    uint32_t tno = lba >= AU0 ? 2u : 1u, rel = lba >= AU0 ? lba - AU0 : lba;
    q[0] = lba >= AU0 ? 0x01u : 0x41u; q[1] = tobcd(tno); q[2] = 1; msf(rel, q + 3); q[6] = 0; msf(lba + 150u, q + 7);
    uint16_t c = crc16(q, 10); q[10] = (uint8_t)(c >> 8); q[11] = (uint8_t)c;
}
static void raw_sector(uint32_t lba, uint8_t *s) {
    memset(s, 0, 2352);
    if (lba >= AU0) {
        for (uint32_t i = 0; i < 588; ++i) {
            int32_t v = (int32_t)(((lba * 588u + i) * 37u) % 20000u) - 10000;
            s[i * 4] = (uint8_t)v; s[i * 4 + 1] = (uint8_t)((uint32_t)v >> 8);
            s[i * 4 + 2] = (uint8_t)(4000 & 0xFF); s[i * 4 + 3] = (uint8_t)(4000 >> 8);
        }
        return;
    }
    s[0] = 0; memset(s + 1, 0xFF, 10); s[11] = 0; msf(lba + 150u, s + 12); s[15] = 2;
    if (lba >= XA0 && lba < XA0 + XAN) {
        static const uint8_t sh[4] = { 1, 0, 0x64, 0 };
        memcpy(s + 16, sh, 4); memcpy(s + 20, sh, 4);
        for (int g = 0; g < 18; ++g) { memset(s + 24 + g * 128, 0x04, 16); memset(s + 24 + g * 128 + 16, 0x55, 112); }
    } else {
        static const uint8_t sh[4] = { 0, 0, 0x08, 0 };
        memcpy(s + 16, sh, 4); memcpy(s + 20, sh, 4);
        if (lba >= 16 && lba < 20) memcpy(s + 24, iso_user[lba - 16], 2048);
        else for (uint32_t i = 0; i < 512; ++i) {
            uint32_t w = ((lba & 0xFFFFu) << 16) | i;
            s[24 + i * 4] = (uint8_t)w; s[25 + i * 4] = (uint8_t)(w >> 8);
            s[26 + i * 4] = (uint8_t)(w >> 16); s[27 + i * 4] = (uint8_t)(w >> 24);
        }
    }
}
void *iso_open(const char *p) { (void)p; return (void *)1; }
void iso_close(void *p) { (void)p; }
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    uint8_t s[2352]; (void)p;
    if (lba >= AU0 + AUN) return 0;
    raw_sector(lba, s); memcpy(b, s, (size_t)(n < 2352 ? n : 2352)); return 1;
}
int iso_read_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    uint8_t s[2352]; (void)p;
    if (lba >= AU0 + AUN) return 0;
    raw_sector(lba, s); memcpy(b, s + 24, (size_t)(n < 2048 ? n : 2048)); return 1;
}
int iso_read_subq(void *p, uint32_t lba, uint8_t *b, int n, int *v) {
    uint8_t q[12]; (void)p;
    if (lba >= AU0 + AUN) { *v = 0; return 0; }
    subq(lba, q); memcpy(b, q, (size_t)(n < 12 ? n : 12)); *v = 1; return 1;
}
int iso_has_subq_replacements(void *p) { (void)p; return 0; }
uint32_t iso_sector_count(void *p) { (void)p; return AU0 + AUN; }
int iso_track_count(void *p) { (void)p; return 2; }
uint32_t iso_track_start_lba(void *p, int t) { (void)p; return t == 2 ? AU0 : 0u; }
uint32_t iso_track_pregap_lba(void *p, int t) { (void)p; return t == 2 ? AU0 : 0u; }
int iso_track_is_audio(void *p, int t) { (void)p; return t == 2; }

/* ---- schedule ------------------------------------------------------------ */
typedef struct { char op; uint64_t t; int a, n; uint8_t p[8]; char text[40]; } Ev;
typedef struct { char name[64]; int first, count; } Case;
static Ev *evs; static int n_evs, cap_evs;
static Case cases[512]; static int n_cases;

static void load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    char line[4200];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'I') {
            unsigned lba; char hex[4100];
            if (sscanf(line, "I %u %4099s", &lba, hex) == 2 && lba >= 16 && lba < 20)
                for (int i = 0; i < 2048; ++i) { unsigned b; sscanf(hex + i * 2, "%2x", &b); iso_user[lba - 16][i] = (uint8_t)b; }
            continue;
        }
        if (line[0] == 'C') {
            Case *c = &cases[n_cases++];
            sscanf(line, "C %63s", c->name); c->first = n_evs; c->count = 0;
            continue;
        }
        if (n_evs == cap_evs) { cap_evs = cap_evs ? cap_evs * 2 : 65536; evs = realloc(evs, (size_t)cap_evs * sizeof *evs); }
        Ev *e = &evs[n_evs]; memset(e, 0, sizeof *e);
        e->op = line[0];
        unsigned long long t; int off = 0;
        if (sscanf(line + 2, "%llu%n", &t, &off) != 1) continue;
        e->t = t;
        const char *rest = line + 2 + off;
        if (e->op == 'W') {
            int cmd, np, pos = 0;
            sscanf(rest, "%d %d%n", &cmd, &np, &pos); e->a = cmd; e->n = np;
            for (int i = 0; i < np && i < 8; ++i) { int v, k = 0; sscanf(rest + pos, "%d%n", &v, &k); e->p[i] = (uint8_t)v; pos += k; }
        } else if (e->op == 'S') {
            unsigned v; sscanf(rest, "%x", &v); e->a = (int)v;
        } else if (e->op == 'Q' || e->op == 'D' || e->op == 'P') {
            sscanf(rest, " %39[^\n]", e->text);
        }
        n_evs++; cases[n_cases - 1].count++;
    }
    fclose(f);
}

/* ---- profile ------------------------------------------------------------- */
static void set_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}
static void apply_profile(const char *profile, const char *tape) {
    static const char *const keys[] = { "PSX_CD_FIRMWARE_MODEL", "PSX_CD_COLD_STATUS_MODEL", "PSX_CD_TOC_SEEK_MODEL",
        "PSX_CD_EXPLICIT_SEEK_MODEL", "PSX_CD_READ_START_MODEL", "PSX_CD_DMA_MODEL", "PSX_CD_DRIVE_MODEL",
        "PSX_CD_CDDA_MODEL", "PSX_CD_SOURCE_CLOCK_TAPE" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) set_env(keys[i], NULL);
    if (!strcmp(profile, "default")) return;
    /* The octoshock-2.2.2 sub-models every TAS route selects (tools/tasreplays). */
    set_env("PSX_CD_FIRMWARE_MODEL", "octoshock-2.2.2");
    set_env("PSX_CD_COLD_STATUS_MODEL", "octoshock-2.2.2");
    set_env("PSX_CD_TOC_SEEK_MODEL", "octoshock-2.2.2");
    set_env("PSX_CD_EXPLICIT_SEEK_MODEL", "octoshock-2.2.2");
    set_env("PSX_CD_READ_START_MODEL", "octoshock-2.2.2-pipeline");
    set_env("PSX_CD_DMA_MODEL", "octoshock-2.2.2");
    if (tape) set_env("PSX_CD_SOURCE_CLOCK_TAPE", tape);
    if (!strcmp(profile, "route")) {
        set_env("PSX_CD_DRIVE_MODEL", "nymashock-1.29.0");
        set_env("PSX_CD_CDDA_MODEL", "octoshock-2.3");
    }
    if (!strcmp(profile, "pepsiman"))   /* tools/tasreplays/pepsiman.py, abesoddysee.py */
        set_env("PSX_CD_CDDA_MODEL", "octoshock-2.3");
}

/* ---- replay -------------------------------------------------------------- */
#define SAMPLE 8u
#define TOLERANCE 500
static int verbose, full_status, trace;
/* The compared projection of status | int_flag << 8: the pending interrupt
 * type, or with --full-status the whole value (BUSYSTS and the parameter FIFO
 * bits are drive behaviour, compared only on request). */
static unsigned project(unsigned v) { return full_status ? v : (v >> 8) & 7u; }
static uint8_t index_now;
static void wr(uint32_t a, uint8_t v) { cdrom_write(a, v); if (a == 0x1F801800u) index_now = v & 3u; }
static void advance_to(uint64_t t) {
    if (t > psx_cycle_count) { uint64_t d = t - psx_cycle_count; psx_cycle_count = t; cdrom_advance((uint32_t)d); }
}
static unsigned sample(void) { return (cdrom_read(0x1F801800u) & 0xFFu) | (cdrom_read(0x1F801803u) & 0xFFu) << 8; }

typedef struct { uint64_t t; unsigned v; } Change;
static Change ours[200000]; static int n_ours;

static int case_class_selected(const char *name, const char *classes) {
    if (!classes) return 1;
    char cls[16]; int i = 0;
    while (name[i] && name[i] != '-' && i < 15) { cls[i] = name[i]; i++; }
    cls[i] = 0;
    const char *p = classes;
    size_t n = strlen(cls);
    while ((p = strstr(p, cls))) {
        if ((p == classes || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return 1;
        p += n;
    }
    return 0;
}

/* Returns the number of mismatched rows; prints the first few. */
static int replay(const Case *c, int *rows_out, long *worst_out) {
    psx_cycle_count = 0; i_stat = 0; index_now = 0; n_ours = 0;
    cdrom_init("synthetic");
    /* Fixture program prologue: enable all CD IRQs, ack, reset the parameter
     * FIFO, CD audio volume to identity, back to index 1. */
    wr(0x1F801800u, 1); wr(0x1F801802u, 0x1F); wr(0x1F801803u, 0x1F); wr(0x1F801803u, 0x40);
    wr(0x1F801800u, 2); wr(0x1F801802u, 0x80); wr(0x1F801803u, 0x00);
    wr(0x1F801800u, 3); wr(0x1F801801u, 0x80); wr(0x1F801802u, 0x00); wr(0x1F801803u, 0x20);
    wr(0x1F801800u, 1);
    unsigned last = sample();
    uint64_t measure = UINT64_MAX;
    unsigned oracle_last = 0;
    int bad = 0, rows = 0; long worst = 0;
    int oi = 0; /* next unmatched change of ours */
    for (int k = 0; k < c->count; ++k) {
        const Ev *e = &evs[c->first + k];
        /* Run the controller up to the event, recording every change. */
        while (psx_cycle_count + SAMPLE <= e->t) {
            advance_to(psx_cycle_count + SAMPLE);
            unsigned v = sample();
            if (project(v) != project(last) && n_ours < 200000) ours[n_ours++] = (Change){ psx_cycle_count, project(v) };
            last = v;
        }
        advance_to(e->t);
        switch (e->op) {
        case 'M': measure = e->t; oi = n_ours; oracle_last = project(last); break;
        case 'W':
            if (queued_cmd.pending) {
                /* The oracle program writes the next command without polling
                 * BUSYSTS; a model still busy here has diverged already. */
                printf("  %s @%llu: command %02X written while %02X is still queued\n", c->name,
                       (unsigned long long)e->t, (unsigned)e->a, (unsigned)queued_cmd.cmd);
                *rows_out = rows; *worst_out = worst;
                return bad + 1;
            }
            wr(0x1F801800u, 0);
            for (int i = 0; i < e->n; ++i) wr(0x1F801802u, e->p[i]);
            wr(0x1F801801u, (uint8_t)e->a);
            wr(0x1F801800u, 1);
            break;
        case 'A': wr(0x1F801803u, 0x1F); break;
        case 'R': wr(0x1F801800u, 0); wr(0x1F801803u, 0x80); wr(0x1F801800u, 1); break;
        case 'P': wr(0x1F801800u, 3); wr(0x1F801803u, (uint8_t)(atoi(e->text) ? 1 : 0)); wr(0x1F801800u, 1); break;
        case 'F': { int n = 0; while ((cdrom_read(0x1F801800u) & 0x40u) && n < 3000) { (void)cdrom_read(0x1F801802u); n++; }
                    const Ev *d = &evs[c->first + k + 1];
                    if (e->t >= measure && d->op == 'D') { rows++; int want = atoi(d->text);
                        if (n != want) { bad++; if (verbose && bad <= 4) printf("  %s fifo @%llu: %d bytes, want %d\n", c->name, (unsigned long long)e->t, n, want); } }
                    break; }
        case 'S': {
            unsigned want = project((unsigned)e->a);
            if (e->t < measure || want == oracle_last) { if (e->t >= measure) oracle_last = want; break; }
            oracle_last = want;
            rows++;
            /* The oracle's poll saw this value at e->t: our change to the same
             * value must lie within TOLERANCE, after the previous match. */
            int hit = -1;
            for (int j = oi; j < n_ours; ++j) {
                if (ours[j].t + TOLERANCE < e->t) continue;
                if (ours[j].t > e->t + TOLERANCE) break;
                if (ours[j].v == want) { hit = j; break; }
            }
            long d = hit >= 0 ? (long)ours[hit].t - (long)e->t : 0;
            if (hit >= 0) {
                oi = hit + 1; if (labs(d) > labs(worst)) worst = d;
            } else {
                long nearest = 0;
                for (int j = oi; j < n_ours; ++j) if (ours[j].v == want) { nearest = (long)ours[j].t - (long)e->t; break; }
                bad++;
                if (verbose && bad <= 6)
                    printf("  %s @%llu oracle -> %X: ours %s%ld\n", c->name, (unsigned long long)(e->t - measure), want,
                           nearest ? "dt " : "never", nearest);
            }
            break;
        }
        case 'Q': {
            if (e->t < measure) { while (cdrom_read(0x1F801800u) & 0x20u) (void)cdrom_read(0x1F801801u); break; }
            rows++;
            char got[17] = { 0 }; int n = 0;
            uint8_t b[8] = { 0 };
            while ((cdrom_read(0x1F801800u) & 0x20u) && n < 8) b[n++] = (uint8_t)cdrom_read(0x1F801801u);
            for (int i = 0; i < 8; ++i) snprintf(got + i * 2, 3, "%02X", b[i]);
            const char *want = strchr(e->text, ' ');
            want = want ? want + 1 : e->text;
            if (strncasecmp(got, want, 16)) {
                bad++;
                if (verbose && bad <= 6) printf("  %s resp @%llu: %s, want %s\n", c->name, (unsigned long long)(e->t - measure), got, want);
            }
            break;
        }
        default: break;
        }
    }
    if (trace) {
        printf("  ours (measure phase):");
        for (int j = 0; j < n_ours; ++j)
            if (ours[j].t >= measure) printf(" %llu:%X", (unsigned long long)(ours[j].t - measure), ours[j].v);
        printf("\n  oracle:");
        unsigned prev = 99;
        for (int k = 0; k < c->count; ++k) {
            const Ev *e = &evs[c->first + k];
            if (e->t < measure) continue;
            if (e->op == 'W') printf(" [W%02X@%llu]", (unsigned)e->a, (unsigned long long)(e->t - measure));
            if (e->op == 'S' && project((unsigned)e->a) != prev) {
                prev = project((unsigned)e->a);
                printf(" %llu:%X", (unsigned long long)(e->t - measure), prev);
            }
        }
        printf("\n");
    }
    *rows_out = rows; *worst_out = worst;
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s data profile [tape] [--classes C3,C4] [--verbose]\n", argv[0]); return 2; }
    const char *tape = NULL, *classes = NULL, *only = NULL;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--full-status")) full_status = 1;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--classes") && i + 1 < argc) classes = argv[++i];
        else if (!strcmp(argv[i], "--case") && i + 1 < argc) only = argv[++i];
        else tape = argv[i];
    }
    load(argv[1]);
    apply_profile(argv[2], tape);
    int cases_bad = 0, cases_run = 0;
    for (int i = 0; i < n_cases; ++i) {
        if (!case_class_selected(cases[i].name, classes)) continue;
        if (only && strcmp(only, cases[i].name)) continue;
        int rows; long worst;
        int bad = replay(&cases[i], &rows, &worst);
        cases_run++;
        if (bad) cases_bad++;
        printf("%s %s rows=%d bad=%d worst_dt=%ld\n", bad ? "DIFF" : "OK  ", cases[i].name, rows, bad, worst);
    }
    printf("profile %s: %d of %d cases match\n", argv[2], cases_run - cases_bad, cases_run);
    return cases_bad ? 1 : 0;
}
