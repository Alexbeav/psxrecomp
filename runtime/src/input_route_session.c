/* Route identity, markers, release replay and the diagnostic recorder (T101).
 * See input_route_session.h. Every entry point is one branch when no route
 * or recording is armed. */
#include "input_route_session.h"
#include "input_route_v3_file.h"
#include "input_dualshock_delivery.h"
#include "psx_sha256.h"
#include "sio.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern uint8_t *g_psx_ram;
extern uint64_t psx_cycle_count;

/* Full 40-hex psxrecomp commit, stamped by runtime.cmake on this file only. */
#ifndef PSX_FRAMEWORK_PIN
#define PSX_FRAMEWORK_PIN ""
#endif

#define RAM_PAGE_BYTES 4096u
#define PATH_BYTES 4096u

static char s_product_serial[INPUT_ROUTE_V3_TEXT];
static char s_product_disc[PATH_BYTES];
static char s_product_bios_stem[INPUT_ROUTE_V3_TEXT];

/* Armed state shared by replay and record. */
static int s_active;         /* markers to check, or recording */
static int s_owns_ports;     /* PSXRTI3 digital route or recording */
static int s_recording;
static int s_exit_after_markers;
static int s_all_matched = 1;
static uint32_t s_frame;     /* inputs supplied before the current boundary */
static InputRouteV3 s_meta;
static InputRouteMarker s_markers[INPUT_ROUTE_V3_MAX_MARKERS];
static InputRouteCheckpoint *s_checkpoints;
static uint32_t s_next_marker;

/* Release replay. */
static InputRouteStep *s_digital;
static InputDualShockRouteStep *s_dual;
static uint32_t s_step_count, s_step_index, s_step_remaining;
static int s_release_loaded, s_release_dual;
static uint8_t s_dual_axes[4] = {128, 128, 128, 128};

/* Recorder (diagnostic product only). */
#ifndef PSX_NO_DEBUG_TOOLS
static char s_record_path[PATH_BYTES];
static uint16_t *s_words;
static uint32_t s_word_count, s_record_steps;
static int s_record_truncated;
static unsigned s_pending[8];
static unsigned s_pending_count;
#endif

static void hex(const uint8_t *bytes, unsigned n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = 0;
}

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= size) n = size - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

void input_route_session_set_product(const char *disc_serial,
                                     const char *disc_path,
                                     const char *bios_path)
{
    const char *base = bios_path ? bios_path : "";
    const char *dot;
    size_t n;
    copy_text(s_product_serial, sizeof(s_product_serial), disc_serial);
    copy_text(s_product_disc, sizeof(s_product_disc), disc_path);
    for (const char *p = base; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof(s_product_bios_stem)) n = sizeof(s_product_bios_stem) - 1;
    memcpy(s_product_bios_stem, base, n);
    s_product_bios_stem[n] = 0;
}

/* ---- Disc digest (run_native.checkpoint_asset_digest) ---- */

static int sha256_file(const char *path, uint8_t out[32])
{
    enum { CHUNK = 1 << 20 };
    psx_sha256_ctx ctx;
    unsigned char *buffer;
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    buffer = (unsigned char *)malloc(CHUNK);
    if (!buffer) { fclose(f); return 0; }
    psx_sha256_init(&ctx);
    while ((n = fread(buffer, 1, CHUNK, f)) > 0) psx_sha256_update(&ctx, buffer, n);
    n = (size_t)ferror(f);
    free(buffer);
    if (fclose(f) || n) return 0;
    psx_sha256_final(&ctx, out);
    return 1;
}

static int ends_with_ci(const char *text, const char *suffix)
{
    size_t a = strlen(text), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; ++i)
        if (tolower((unsigned char)text[a - b + i]) != tolower((unsigned char)suffix[i]))
            return 0;
    return 1;
}

static int word_ci(const char *p, const char *word)
{
    for (; *word; ++p, ++word)
        if (toupper((unsigned char)*p) != *word) return 0;
    return 1;
}

/* One cue line. Returns 0 when not a FILE line, 1 with `name` filled for a
 * quoted BINARY FILE line, -1 for any other FILE line. */
static int cue_file_line(const char *line, char *name, size_t size)
{
    const char *p = line, *start;
    while (isspace((unsigned char)*p)) ++p;
    if (!word_ci(p, "FILE") || isalnum((unsigned char)p[4]) || p[4] == '_') return 0;
    p += 4;
    if (!isspace((unsigned char)*p)) return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '"') return -1;
    start = p;
    while (*p && *p != '"' && *p != '\r' && *p != '\n') ++p;
    if (*p != '"' || p == start || (size_t)(p - start) >= size) return -1;
    memcpy(name, start, (size_t)(p - start));
    name[p - start] = 0;
    ++p;
    if (!isspace((unsigned char)*p)) return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (!word_ci(p, "BINARY")) return -1;
    p += 6;
    while (isspace((unsigned char)*p)) ++p;
    return *p ? -1 : 1;
}

static const char *disc_digest(uint32_t *kind, uint8_t out[32])
{
    uint8_t digest[32];
    char text[65];
    if (!s_product_disc[0]) return "no disc is mounted";
    if (!sha256_file(s_product_disc, digest)) return "cannot read the disc image";
    if (!ends_with_ci(s_product_disc, ".cue")) {
        *kind = INPUT_ROUTE_DISC_DIGEST_FILE;
        memcpy(out, digest, 32);
        return NULL;
    }
    FILE *f = fopen(s_product_disc, "rb");
    char *cue;
    long size;
    if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || size > (1L << 20) ||
        fseek(f, 0, SEEK_SET)) { if (f) fclose(f); return "cannot read the cue sheet"; }
    cue = (char *)malloc((size_t)size + 1);
    if (!cue || fread(cue, 1, (size_t)size, f) != (size_t)size) {
        free(cue); fclose(f); return "cannot read the cue sheet";
    }
    fclose(f);
    cue[size] = 0;
    psx_sha256_ctx ctx;
    const char *error = NULL;
    int tracks = 0;
    char dir[PATH_BYTES];
    size_t dir_len = 0;
    for (size_t i = 0; s_product_disc[i]; ++i)
        if (s_product_disc[i] == '/' || s_product_disc[i] == '\\') dir_len = i + 1;
    memcpy(dir, s_product_disc, dir_len);
    psx_sha256_init(&ctx);
    hex(digest, 32, text);
    psx_sha256_update(&ctx, (const uint8_t *)text, 64);
    char *line = cue;
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) line += 3;
    while (!error && line && *line) {
        char *next = strpbrk(line, "\r\n");
        char name[PATH_BYTES], track[PATH_BYTES];
        if (next) *next++ = 0;
        int kind_line = cue_file_line(line, name, sizeof(name));
        if (kind_line < 0) error = "cue FILE line is not a quoted BINARY track";
        else if (kind_line > 0) {
            if (dir_len + strlen(name) >= sizeof(track)) error = "cue track path too long";
            else {
                memcpy(track, dir, dir_len);
                strcpy(track + dir_len, name);
                if (!sha256_file(track, digest)) error = "cannot read a cue track";
                else {
                    hex(digest, 32, text);
                    psx_sha256_update(&ctx, (const uint8_t *)"\n", 1);
                    psx_sha256_update(&ctx, (const uint8_t *)text, 64);
                    ++tracks;
                }
            }
        }
        line = next;
    }
    free(cue);
    if (error) return error;
    if (!tracks) return "cue sheet has no tracks";
    psx_sha256_final(&ctx, out);
    *kind = INPUT_ROUTE_DISC_DIGEST_CUE;
    return NULL;
}

/* ---- Checkpoints ---- */

static void checkpoint_now(uint32_t frame, InputRouteCheckpoint *c)
{
    c->frame = frame;
    c->cycle = psx_cycle_count;
    psx_sha256_compute(g_psx_ram, INPUT_ROUTE_RAM_PAGES * RAM_PAGE_BYTES, c->ram_sha256);
    for (uint32_t p = 0; p < INPUT_ROUTE_RAM_PAGES; ++p) {
        uint64_t hash = UINT64_C(14695981039346656037);
        const uint8_t *bytes = g_psx_ram + p * RAM_PAGE_BYTES;
        for (uint32_t i = 0; i < RAM_PAGE_BYTES; ++i)
            hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
        c->pages[p] = hash;
    }
}

static const char *marker_name(uint32_t kind)
{
    return kind == INPUT_ROUTE_MARKER_MENU ? "menu" : "gameplay";
}

static const InputRouteCheckpoint *recorded_checkpoint(uint32_t frame)
{
    for (uint32_t i = 0; i < s_meta.checkpoint_count; ++i)
        if (s_checkpoints[i].frame == frame) return &s_checkpoints[i];
    return NULL;
}

static void check_marker(const InputRouteMarker *m)
{
    InputRouteCheckpoint now;
    const InputRouteCheckpoint *want = recorded_checkpoint(m->frame);
    char sha[65];
    checkpoint_now(m->frame, &now);
    hex(now.ram_sha256, 32, sha);
    const char *verdict = "unrecorded";
    if (want) {
        int ram = !memcmp(want->ram_sha256, now.ram_sha256, 32);
        int cycle = want->cycle == now.cycle;
        verdict = ram && cycle ? "match" : "mismatch";
        if (!(ram && cycle)) s_all_matched = 0;
    }
    fprintf(stdout,
            "input_route_marker: frame=%u kind=%s cycle=%llu ram_sha256=%s checkpoint=%s\n",
            (unsigned)m->frame, marker_name(m->kind),
            (unsigned long long)now.cycle, sha, verdict);
    if (want && strcmp(verdict, "match")) {
        char want_sha[65];
        unsigned shown = 0, differing = 0;
        hex(want->ram_sha256, 32, want_sha);
        fprintf(stdout, "input_route_marker_expected: frame=%u cycle=%llu ram_sha256=%s differing_pages=",
                (unsigned)m->frame, (unsigned long long)want->cycle, want_sha);
        for (uint32_t p = 0; p < INPUT_ROUTE_RAM_PAGES; ++p)
            if (want->pages[p] != now.pages[p]) {
                ++differing;
                if (shown++ < 16) fprintf(stdout, "%s0x%06X", shown > 1 ? "," : "", p * RAM_PAGE_BYTES);
            }
        fprintf(stdout, "%s (%u)\n", differing > 16 ? ",..." : "", differing);
    }
    fflush(stdout);
}

/* ---- Admission ---- */

static int refuse(const char *path, const char *why)
{
    fprintf(stderr, "input route rejected: %s (%s)\n", why, path);
    return 0;
}

static int alloc_steps(InputRouteStep **digital, InputDualShockRouteStep **dual)
{
    *digital = (InputRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof(**digital));
    *dual = (InputDualShockRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof(**dual));
    if (*digital && *dual) return 1;
    free(*digital); free(*dual);
    *digital = NULL; *dual = NULL;
    return 0;
}

int input_route_session_admit(const char *path)
{
    unsigned char magic[8];
    InputRouteStep *digital;
    InputDualShockRouteStep *dual;
    InputRouteV3 meta;
    uint32_t count = 0, frames = 0;
    const char *error = NULL;
    int v3, dualshock;
    FILE *f;
    if (s_active || s_release_loaded) return refuse(path, "a route is already armed");
    f = fopen(path, "rb");
    if (!f) return refuse(path, "cannot open");
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) || fseek(f, 0, SEEK_SET)) {
        fclose(f); return refuse(path, "short header");
    }
    v3 = !memcmp(magic, "PSXRTI3\0", 8);
#ifndef PSX_NO_DEBUG_TOOLS
    /* PSXRTI1/PSXRTI2 stay entirely with the debug-server preload. */
    if (!v3) { fclose(f); return 1; }
#else
    if (!v3 && memcmp(magic, "PSXRTI1\0", 8) && memcmp(magic, "PSXRTI2\0", 8)) {
        fclose(f); return refuse(path, "unsupported route format");
    }
#endif
    if (!alloc_steps(&digital, &dual)) { fclose(f); return refuse(path, "out of memory"); }
    memset(&meta, 0, sizeof(meta));
    if (v3) {
        if (!s_checkpoints)
            s_checkpoints = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS,
                                                         sizeof(*s_checkpoints));
        error = s_checkpoints ? input_route_v3_read(f, &meta, digital, dual, s_markers, s_checkpoints)
                              : "out of memory";
        count = meta.steps; frames = meta.frames;
        dualshock = meta.record_size == INPUT_DUALSHOCK_ROUTE_RECORD_BYTES;
    } else if (!memcmp(magic, "PSXRTI2\0", 8)) {
        error = input_dualshock_route_read(f, dual, &count, &frames);
        dualshock = 1;
    } else {
        error = input_route_read(f, digital, &count, &frames);
        dualshock = 0;
    }
    if (fclose(f) && !error) error = "close error";
    for (uint32_t i = 0; !error && dualshock && i < count; ++i)
        if (dual[i].analog_button) error = "physical Analog press is not qualified";
    if (error) {
        free(digital); free(dual);
        memset(s_markers, 0, sizeof(s_markers));
        return refuse(path, error);
    }
#ifdef PSX_NO_DEBUG_TOOLS
    /* Release replay owns the steps. The diagnostic product reparses them in
     * its debug-server preload, which keeps its evidence observers. */
    if (dualshock) { s_dual = dual; free(digital); }
    else { s_digital = digital; free(dual); }
    s_step_count = count;
    s_step_index = 0;
    s_step_remaining = dualshock ? s_dual[0].frames : s_digital[0].frames;
    s_release_dual = dualshock;
    s_release_loaded = 1;
    if (dualshock) input_route_admit_cold_p1(1);
    fprintf(stdout, "input_route_preloaded: start_frame=0 frames=%u steps=%u format=%s product=release\n",
            (unsigned)frames, (unsigned)count, v3 ? "PSXRTI3" : dualshock ? "PSXRTI2" : "PSXRTI1");
#else
    free(digital); free(dual);
#endif
    if (!v3) return 1;
    s_meta = meta;
    s_next_marker = 0;
    s_frame = 0;
    s_active = meta.marker_count != 0;
    if (!dualshock) {
        /* A PSXRTI3 digital body declares one digital pad at P1. */
        s_owns_ports = 1;
        input_route_admit_cold_p1(0);
    }
    const char *exit_after = getenv("PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS");
    s_exit_after_markers = exit_after && !strcmp(exit_after, "1") && meta.marker_count;
    fprintf(stdout, "input_route_extension: identity=%s markers=%u checkpoints=%u\n",
            meta.has_identity ? "present" : "absent", (unsigned)meta.marker_count,
            (unsigned)meta.checkpoint_count);
    return 1;
}

const char *input_route_session_read_v3_digital(FILE *f, InputRouteStep *steps,
                                                uint32_t *step_count,
                                                uint32_t *frame_count)
{
    InputRouteV3 meta;
    const char *error;
    *step_count = *frame_count = 0;
    if (!s_checkpoints) return "PSXRTI3 route was not admitted";
    error = input_route_v3_read(f, &meta, steps, NULL, s_markers, s_checkpoints);
    if (error) return error;
    *step_count = meta.steps;
    *frame_count = meta.frames;
    return NULL;
}

const char *input_route_session_read_v3_dualshock(FILE *f,
                                                  InputDualShockRouteStep *steps,
                                                  uint32_t *step_count,
                                                  uint32_t *frame_count)
{
    InputRouteV3 meta;
    const char *error;
    *step_count = *frame_count = 0;
    if (!s_checkpoints) return "PSXRTI3 route was not admitted";
    error = input_route_v3_read(f, &meta, NULL, steps, s_markers, s_checkpoints);
    if (error) return error;
    *step_count = meta.steps;
    *frame_count = meta.frames;
    return NULL;
}

/* ---- Identity ---- */

static const char *boot_mode_name(int call_hle, int boot_skip)
{
    if (call_hle && boot_skip) return "hle";
    if (call_hle) return "hle-calls";
    if (boot_skip) return "hle-boot";
    return "lle";
}

static int valid_pin(const char *pin)
{
    if (strlen(pin) != 40) return 0;
    for (unsigned i = 0; i < 40; ++i)
        if (!((pin[i] >= '0' && pin[i] <= '9') || (pin[i] >= 'a' && pin[i] <= 'f')))
            return 0;
    return 1;
}

static int stem_equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static const char *digest_kind_name(uint32_t kind)
{
    return kind == INPUT_ROUTE_DISC_DIGEST_CUE ? "cue" : "file";
}

int input_route_session_verify_identity(int call_hle, int boot_skip)
{
    const char *boot = boot_mode_name(call_hle, boot_skip);
    const char *pin = PSX_FRAMEWORK_PIN;
    uint32_t kind = 0;
    uint8_t digest[32];
    const char *error;
    int mismatches = 0;
#ifndef PSX_NO_DEBUG_TOOLS
    if (s_recording) {
        error = NULL;
        if (!valid_pin(pin))
            error = "this product has no framework pin (built outside a psxrecomp git checkout)";
        else if (!s_product_serial[0] || !s_product_bios_stem[0])
            error = "a disc with a boot serial and a BIOS file are required";
        else
            error = disc_digest(&kind, digest);
        if (error) {
            s_recording = 0;
            fprintf(stderr, "input route recording refused: %s (disc %s)\n", error, s_product_disc);
            return 0;
        }
        s_meta.has_identity = 1;
        copy_text(s_meta.pin, sizeof(s_meta.pin), pin);
        copy_text(s_meta.disc_serial, sizeof(s_meta.disc_serial), s_product_serial);
        copy_text(s_meta.bios_stem, sizeof(s_meta.bios_stem), s_product_bios_stem);
        copy_text(s_meta.boot_mode, sizeof(s_meta.boot_mode), boot);
        s_meta.disc_digest_kind = kind;
        memcpy(s_meta.disc_digest, digest, 32);
        fprintf(stdout, "input_route_recording: path=%s pin=%s disc=%s bios=%s boot=%s\n",
                s_record_path, pin, s_product_serial, s_product_bios_stem, boot);
        return 1;
    }
#endif
    if (!s_meta.has_identity) return 1;
#define MISMATCH(field, route_value, product_value) do { \
        if (!mismatches++) fprintf(stderr, "psxrecomp: input route refused: it was recorded on a different product\n"); \
        fprintf(stderr, "  %-10s route=%s  product=%s\n", field, route_value, product_value); \
    } while (0)
    if (!valid_pin(pin)) MISMATCH("pin", s_meta.pin, "(none: built outside a psxrecomp git checkout)");
    else if (strcmp(pin, s_meta.pin)) MISMATCH("pin", s_meta.pin, pin);
    if (strcmp(s_meta.disc_serial, s_product_serial))
        MISMATCH("disc", s_meta.disc_serial, s_product_serial[0] ? s_product_serial : "(none)");
    if (!stem_equal(s_meta.bios_stem, s_product_bios_stem))
        MISMATCH("bios", s_meta.bios_stem, s_product_bios_stem[0] ? s_product_bios_stem : "(none)");
    if (strcmp(s_meta.boot_mode, boot)) MISMATCH("boot", s_meta.boot_mode, boot);
    error = disc_digest(&kind, digest);
    if (error) {
        MISMATCH("disc hash", "(recorded)", error);
    } else if (kind != s_meta.disc_digest_kind) {
        char route_kind[24], product_kind[24];
        snprintf(route_kind, sizeof(route_kind), "%s image", digest_kind_name(s_meta.disc_digest_kind));
        snprintf(product_kind, sizeof(product_kind), "%s image", digest_kind_name(kind));
        MISMATCH("disc hash", route_kind, product_kind);
    } else if (memcmp(digest, s_meta.disc_digest, 32)) {
        char want[65], have[65];
        hex(s_meta.disc_digest, 32, want);
        hex(digest, 32, have);
        MISMATCH("disc hash", want, have);
    }
#undef MISMATCH
    if (mismatches) return 0;
    fprintf(stdout, "input_route_identity: match pin=%s disc=%s bios=%s boot=%s\n",
            pin, s_product_serial, s_product_bios_stem, boot);
    return 1;
}

int input_route_session_owns_ports(void)
{
    return s_owns_ports;
}

/* ---- Per-vblank boundary ---- */

static void record_marker_at_boundary(uint32_t frame);

int input_route_session_boundary(void)
{
    if (!s_active) return -1;
    const uint32_t frame = s_frame++;
    if (s_recording) {
        record_marker_at_boundary(frame);
        return -1;
    }
    int checked = 0;
    while (s_next_marker < s_meta.marker_count && s_markers[s_next_marker].frame == frame) {
        check_marker(&s_markers[s_next_marker++]);
        checked = 1;
    }
    if (checked && s_next_marker == s_meta.marker_count) {
        fprintf(stdout, "input_route_markers_complete: markers=%u result=%s\n",
                (unsigned)s_meta.marker_count, s_all_matched ? "match" : "mismatch");
        fflush(stdout);
        if (s_exit_after_markers) return s_all_matched ? 0 : 3;
    }
    return -1;
}

/* ---- Release replay ---- */

int input_route_session_release_override(void)
{
    if (!s_release_loaded) return -1;
    if (s_release_dual) {
        uint16_t current = 0xFFFF;
        memset(s_dual_axes, 128, sizeof(s_dual_axes));
        if (s_step_index < s_step_count) {
            current = s_dual[s_step_index].buttons;
            memcpy(s_dual_axes, s_dual[s_step_index].axes_ly_lx_ry_rx, 4);
            if (--s_step_remaining == 0 && ++s_step_index < s_step_count)
                s_step_remaining = s_dual[s_step_index].frames;
        }
        return current;
    }
    /* A file route never releases P1 to physical input at its end. */
    if (s_step_index >= s_step_count) return 0xFFFF;
    const int current = s_digital[s_step_index].buttons;
    if (--s_step_remaining == 0 && ++s_step_index < s_step_count)
        s_step_remaining = s_digital[s_step_index].frames;
    return current;
}

int input_route_session_release_dualshock(int buttons)
{
    if (!s_release_loaded || !s_release_dual) return 0;
    input_dualshock_deliver((uint16_t)buttons, s_dual_axes);
    return 1;
}

/* ---- Recorder (diagnostic product) ---- */

int input_route_session_recording(void)
{
    return s_recording;
}

#ifdef PSX_NO_DEBUG_TOOLS
int input_route_session_record_begin(const char *path)
{
    fprintf(stderr, "input route recording refused: recording needs the diagnostic product (%s)\n",
            path ? path : "");
    return 0;
}
static void record_marker_at_boundary(uint32_t frame) { (void)frame; }
void input_route_session_record_input(uint16_t buttons) { (void)buttons; }
int input_route_session_request_marker(unsigned kind) { (void)kind; return 0; }
void input_route_session_record_status(char *out, size_t size)
{
    snprintf(out, size, "\"recording\":false");
}
#else
static int new_file_exclusive(const char *path, FILE **out)
{
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    *out = fd < 0 ? NULL : _fdopen(fd, "wb");
    if (!*out && fd >= 0) _close(fd);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    *out = fd < 0 ? NULL : fdopen(fd, "wb");
    if (!*out && fd >= 0) close(fd);
#endif
    return *out != NULL;
}

static void record_write(void)
{
    FILE *f;
    const char *error;
    if (!s_recording) return;
    s_recording = 0;
    if (!s_word_count) {
        fprintf(stderr, "input route recording: no frames were recorded; %s not written\n", s_record_path);
        return;
    }
    /* Markers requested but not yet at a boundary are dropped. */
    s_meta.frames = s_word_count;
    s_meta.record_size = 8;
    if (!new_file_exclusive(s_record_path, &f)) {
        fprintf(stderr, "input route recording: cannot create %s: %s\n", s_record_path, strerror(errno));
        return;
    }
    error = input_route_v3_write_digital(f, &s_meta, s_words, s_word_count, s_markers, s_checkpoints);
    if (fclose(f) && !error) error = "close error";
    if (!error) {
        /* The written file must pass the same reader replay uses. */
        InputRouteV3 check;
        InputRouteStep *steps = (InputRouteStep *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof(*steps));
        InputRouteMarker *markers = (InputRouteMarker *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof(*markers));
        InputRouteCheckpoint *checkpoints = (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof(*checkpoints));
        FILE *r = fopen(s_record_path, "rb");
        error = !steps || !markers || !checkpoints || !r ? "cannot reopen the written route"
              : input_route_v3_read(r, &check, steps, NULL, markers, checkpoints);
        if (r) fclose(r);
        if (!error && (check.frames != s_word_count || check.steps != s_record_steps ||
                       check.marker_count != s_meta.marker_count))
            error = "written route does not read back identically";
        free(steps); free(markers); free(checkpoints);
    }
    if (error) {
        fprintf(stderr, "input route recording: %s failed: %s\n", s_record_path, error);
        return;
    }
    fprintf(stdout, "input_route_recorded: path=%s frames=%u steps=%u markers=%u truncated=%s\n",
            s_record_path, (unsigned)s_word_count, (unsigned)s_record_steps,
            (unsigned)s_meta.marker_count, s_record_truncated ? "true" : "false");
    fflush(stdout);
}

int input_route_session_record_begin(const char *path)
{
    FILE *probe;
    if (!path || !path[0] || strlen(path) >= sizeof(s_record_path)) {
        fprintf(stderr, "input route recording refused: invalid path\n");
        return 0;
    }
    if (s_active || s_release_loaded || s_meta.frames) {
        fprintf(stderr, "input route recording refused: a route replay is armed\n");
        return 0;
    }
    if ((probe = fopen(path, "rb")) != NULL) {
        fclose(probe);
        fprintf(stderr, "input route recording refused: %s already exists\n", path);
        return 0;
    }
    s_words = (uint16_t *)malloc(INPUT_ROUTE_MAX_FRAMES * sizeof(*s_words));
    s_checkpoints = s_checkpoints ? s_checkpoints
                  : (InputRouteCheckpoint *)calloc(INPUT_ROUTE_V3_MAX_MARKERS, sizeof(*s_checkpoints));
    if (!s_words || !s_checkpoints) {
        fprintf(stderr, "input route recording refused: out of memory\n");
        return 0;
    }
    copy_text(s_record_path, sizeof(s_record_path), path);
    memset(&s_meta, 0, sizeof(s_meta));
    s_recording = s_active = s_owns_ports = 1;
    s_frame = 0;
    input_route_admit_cold_p1(0);
    atexit(record_write);
    return 1;
}

void input_route_session_record_input(uint16_t buttons)
{
    if (!s_recording || s_record_truncated) return;
    const int new_step = !s_word_count || s_words[s_word_count - 1] != buttons;
    if (s_word_count == INPUT_ROUTE_MAX_FRAMES ||
        (new_step && s_record_steps == INPUT_ROUTE_MAX_STEPS)) {
        s_record_truncated = 1;
        fprintf(stderr, "input route recording: %s cap reached at frame %u; later input is not recorded\n",
                s_word_count == INPUT_ROUTE_MAX_FRAMES ? "frame" : "step", (unsigned)s_word_count);
        return;
    }
    s_record_steps += (uint32_t)new_step;
    s_words[s_word_count++] = buttons;
}

static void record_marker_at_boundary(uint32_t frame)
{
    if (!s_pending_count) return;
    const unsigned kind = s_pending[0];
    memmove(s_pending, s_pending + 1, (--s_pending_count) * sizeof(s_pending[0]));
    /* The boundary index equals the words recorded so far. */
    if (frame != s_word_count || s_record_truncated) {
        fprintf(stderr, "input route recording: %s marker dropped (recording truncated)\n",
                marker_name(kind));
        return;
    }
    if (s_meta.marker_count == INPUT_ROUTE_V3_MAX_MARKERS) {
        fprintf(stderr, "input route recording: marker capacity reached; %s marker dropped\n",
                marker_name(kind));
        return;
    }
    InputRouteMarker *m = &s_markers[s_meta.marker_count++];
    m->frame = frame;
    m->kind = kind;
    InputRouteCheckpoint *c = &s_checkpoints[s_meta.checkpoint_count++];
    checkpoint_now(frame, c);
    char sha[65];
    hex(c->ram_sha256, 32, sha);
    fprintf(stdout, "input_route_marker: frame=%u kind=%s cycle=%llu ram_sha256=%s checkpoint=recorded\n",
            (unsigned)frame, marker_name(kind), (unsigned long long)c->cycle, sha);
    fflush(stdout);
}

int input_route_session_request_marker(unsigned kind)
{
    if (!s_recording ||
        (kind != INPUT_ROUTE_MARKER_MENU && kind != INPUT_ROUTE_MARKER_GAMEPLAY))
        return 0;
    if (s_pending_count == sizeof(s_pending) / sizeof(s_pending[0])) return 0;
    s_pending[s_pending_count++] = kind;
    return 1;
}

void input_route_session_record_status(char *out, size_t size)
{
    char path[2 * PATH_BYTES];
    size_t n = 0;
    for (const char *p = s_record_path; *p && n + 2 < sizeof(path); ++p) {
        if (*p == '"' || *p == '\\') path[n++] = '\\';
        path[n++] = (unsigned char)*p < 0x20 ? '?' : *p;
    }
    path[n] = 0;
    snprintf(out, size,
             "\"recording\":%s,\"path\":\"%s\",\"frames\":%u,\"steps\":%u,\"markers\":%u,"
             "\"pending_markers\":%u,\"truncated\":%s",
             s_recording ? "true" : "false", path,
             (unsigned)s_word_count, (unsigned)s_record_steps, (unsigned)s_meta.marker_count,
             (unsigned)s_pending_count, s_record_truncated ? "true" : "false");
}
#endif
