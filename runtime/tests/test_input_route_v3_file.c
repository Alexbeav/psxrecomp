/* PSXRTI3 wire-format cases: authored routes only, no retail data. */
#include "input_route_v3_file.h"
#include <stdlib.h>

static void check(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static InputRouteStep digital[INPUT_ROUTE_MAX_STEPS];
static InputDualShockRouteStep dual[INPUT_ROUTE_MAX_STEPS];
static InputRouteMarker markers[INPUT_ROUTE_V3_MAX_MARKERS];
static InputRouteCheckpoint checkpoints[INPUT_ROUTE_V3_MAX_MARKERS];
static unsigned char bytes[1 << 20];

typedef struct { unsigned char *p; size_t n; } Buf;

static void b32(Buf *b, uint32_t v) { input_route_put32(b->p + b->n, v); b->n += 4; }
static void braw(Buf *b, const void *data, size_t n) { memcpy(b->p + b->n, data, n); b->n += n; }
static void bentry(Buf *b, uint32_t tag, const void *payload, uint32_t n)
{
    b32(b, tag); b32(b, n); braw(b, payload, n);
    while (n++ % 4) b->p[b->n++] = 0;
}

static FILE *as_file(const unsigned char *data, size_t n)
{
    FILE *f = tmpfile();
    check(f != NULL, "tmpfile");
    check(fwrite(data, 1, n, f) == n, "fixture write");
    rewind(f);
    return f;
}

/* A digital v3 file with an arbitrary extension block and `frames` records. */
static size_t build(uint32_t record_size, uint32_t frames, uint32_t flags,
                    const unsigned char *ext, uint32_t ext_len)
{
    Buf b = {bytes, 0};
    braw(&b, "PSXRTI3\0", 8);
    b32(&b, 3); b32(&b, record_size); b32(&b, frames); b32(&b, flags); b32(&b, ext_len);
    braw(&b, ext, ext_len);
    for (uint32_t i = 0; i < frames; ++i) {
        b32(&b, i + 1);
        if (record_size == 8) {
            unsigned char r[4] = {(unsigned char)(i < 2 ? 0xff : 0xef), 0xff, 0, 0};
            braw(&b, r, 4);
        } else {
            unsigned char r[8] = {0xff, 0xff, 128, 128, 128, 128, 0, 0};
            braw(&b, r, 8);
        }
    }
    return b.n;
}

static const char *read_bytes(size_t n, InputRouteV3 *meta)
{
    FILE *f = as_file(bytes, n);
    const char *error = input_route_v3_read(f, meta, digital, dual, markers, checkpoints);
    fclose(f);
    return error;
}

static uint32_t identity_block(unsigned char *ext, const char *pin, int omit)
{
    Buf b = {ext, 0};
    unsigned char digest[36] = {INPUT_ROUTE_DISC_DIGEST_CUE, 0, 0, 0};
    for (unsigned i = 0; i < 32; ++i) digest[4 + i] = (unsigned char)i;
    if (omit != 1) bentry(&b, INPUT_ROUTE_TAG_PIN, pin, (uint32_t)strlen(pin));
    if (omit != 2) bentry(&b, INPUT_ROUTE_TAG_DISC_SERIAL, "SLUS-00662", 10);
    if (omit != 3) bentry(&b, INPUT_ROUTE_TAG_DISC_DIGEST, digest, 36);
    if (omit != 4) bentry(&b, INPUT_ROUTE_TAG_BIOS_STEM, "SCPH1001", 8);
    if (omit != 5) bentry(&b, INPUT_ROUTE_TAG_BOOT_MODE, "hle", 3);
    return (uint32_t)b.n;
}

static const char pin[] = "0123456789abcdef0123456789abcdef01234567";

int main(void)
{
    InputRouteV3 meta;
    unsigned char ext[65536];
    uint32_t n;
    int cases = 0;

    /* Writer -> reader round trip with identity, markers and checkpoints. */
    {
        uint16_t words[300];
        InputRouteV3 w;
        InputRouteMarker wm[2] = {{0, INPUT_ROUTE_MARKER_MENU}, {300, INPUT_ROUTE_MARKER_GAMEPLAY}};
        static InputRouteCheckpoint wc[2];
        memset(&w, 0, sizeof(w));
        w.has_identity = 1;
        strcpy(w.pin, pin); strcpy(w.disc_serial, "SLUS-00662");
        strcpy(w.bios_stem, "SCPH1001"); strcpy(w.boot_mode, "hle-boot");
        w.disc_digest_kind = INPUT_ROUTE_DISC_DIGEST_FILE;
        for (unsigned i = 0; i < 32; ++i) w.disc_digest[i] = (unsigned char)(0xa0 + i);
        w.marker_count = w.checkpoint_count = 2;
        for (unsigned i = 0; i < 2; ++i) {
            wc[i].frame = wm[i].frame;
            wc[i].cycle = UINT64_C(0x123456789a) * (i + 1);
            for (unsigned j = 0; j < 32; ++j) wc[i].ram_sha256[j] = (unsigned char)(i * 7 + j);
            for (unsigned j = 0; j < INPUT_ROUTE_RAM_PAGES; ++j)
                wc[i].pages[j] = UINT64_C(0xfedcba9876543210) ^ ((uint64_t)j << (i * 8));
        }
        for (unsigned i = 0; i < 300; ++i) words[i] = (uint16_t)(i < 100 ? 0xffff : i < 150 ? 0xfff7 : 0xbfff);
        FILE *f = tmpfile();
        check(f && !input_route_v3_write_digital(f, &w, words, 300, wm, wc), "write");
        rewind(f);
        check(!input_route_v3_read(f, &meta, digital, NULL, markers, checkpoints), "read written route");
        fclose(f);
        check(meta.has_identity && meta.frames == 300 && meta.steps == 3 && meta.record_size == 8,
              "round trip shape");
        check(!strcmp(meta.pin, pin) && !strcmp(meta.disc_serial, "SLUS-00662") &&
              !strcmp(meta.bios_stem, "SCPH1001") && !strcmp(meta.boot_mode, "hle-boot") &&
              meta.disc_digest_kind == INPUT_ROUTE_DISC_DIGEST_FILE &&
              !memcmp(meta.disc_digest, w.disc_digest, 32), "round trip identity");
        check(digital[0].frames == 100 && digital[0].buttons == 0xffff &&
              digital[1].frames == 50 && digital[1].buttons == 0xfff7 &&
              digital[2].frames == 150 && digital[2].buttons == 0xbfff, "round trip steps");
        check(meta.marker_count == 2 && markers[1].frame == 300 &&
              markers[1].kind == INPUT_ROUTE_MARKER_GAMEPLAY && meta.checkpoint_count == 2 &&
              !memcmp(&checkpoints[1], &wc[1], sizeof(wc[1])), "round trip markers");
        cases += 4;
    }

    /* Minimal file: no extension block. */
    n = (uint32_t)build(8, 5, 0, NULL, 0);
    check(!read_bytes(n, &meta) && !meta.has_identity && meta.steps == 2 && meta.frames == 5,
          "empty extension");
    /* DualShock record layout. */
    n = (uint32_t)build(12, 4, 0, NULL, 0);
    check(!read_bytes(n, &meta) && meta.steps == 1 && dual[0].frames == 4, "record size 12");
    {
        FILE *f = as_file(bytes, n);
        check(input_route_v3_read(f, &meta, digital, NULL, markers, checkpoints) != NULL,
              "NULL DualShock array refuses the layout");
        fclose(f);
    }
    /* The legacy readers refuse a v3 file. */
    {
        uint32_t c, fr;
        FILE *f = as_file(bytes, n);
        check(input_route_read(f, digital, &c, &fr) != NULL, "PSXRTI1 reader refuses v3");
        fclose(f);
        f = as_file(bytes, n);
        check(input_dualshock_route_read(f, dual, &c, &fr) != NULL, "PSXRTI2 reader refuses v3");
        fclose(f);
    }
    cases += 5;

    /* Header faults. */
    n = (uint32_t)build(8, 5, 1, NULL, 0);
    check(read_bytes(n, &meta) != NULL && !meta.frames, "flags must be zero");
    n = (uint32_t)build(9, 5, 0, NULL, 0);
    check(read_bytes(n, &meta) != NULL, "record size");
    n = (uint32_t)build(8, 0, 0, NULL, 0);
    check(read_bytes(n, &meta) != NULL, "zero frames");
    n = (uint32_t)build(8, 5, 0, NULL, 0);
    bytes[8] = 2;
    check(read_bytes(n, &meta) != NULL, "version");
    n = (uint32_t)build(8, 5, 0, NULL, 0);
    check(read_bytes(n - 1, &meta) != NULL, "truncated");
    bytes[n] = 0;
    check(read_bytes(n + 1, &meta) != NULL, "trailing byte");
    memset(ext, 0, 4);
    n = (uint32_t)build(8, 5, 0, ext, 4);
    input_route_put32(bytes + 24, 3);
    check(read_bytes(n - 1, &meta) != NULL, "extension size not a multiple of 4");
    cases += 7;

    /* Tags. */
    {
        Buf b = {ext, 0};
        bentry(&b, 0x80000300u, "skip", 4);
        bentry(&b, 0x8000ffffu, "x", 1);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(!read_bytes(n, &meta), "unknown skippable tags are skipped");
        b.n = 0; bentry(&b, 0x00000201u, "\1\0\0\0\1", 5);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "T98 mandatory tag refused here");
        b.n = 0; bentry(&b, 0x800001ffu, "x", 1);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "unknown 0x800001xx refused");
        b.n = 0; bentry(&b, 0x80000300u, "x", 1); ext[b.n - 1] = 1;
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "nonzero padding");
        b.n = 0; b32(&b, 0x80000300u); b32(&b, 64);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "entry longer than block");
        cases += 5;
    }

    /* Identity is all-or-nothing, strict, and unique. */
    n = identity_block(ext, pin, 0);
    n = (uint32_t)build(8, 5, 0, ext, n);
    check(!read_bytes(n, &meta) && meta.has_identity, "full identity");
    for (int omit = 1; omit <= 5; ++omit) {
        uint32_t len = identity_block(ext, pin, omit);
        n = (uint32_t)build(8, 5, 0, ext, len);
        check(read_bytes(n, &meta) != NULL && !meta.has_identity, "partial identity refused");
    }
    n = identity_block(ext, "0123456789ABCDEF0123456789abcdef01234567", 0);
    n = (uint32_t)build(8, 5, 0, ext, n);
    check(read_bytes(n, &meta) != NULL, "uppercase pin refused");
    n = identity_block(ext, "0123456789abcdef", 0);
    n = (uint32_t)build(8, 5, 0, ext, n);
    check(read_bytes(n, &meta) != NULL, "short pin refused");
    {
        uint32_t len = identity_block(ext, pin, 0);
        Buf b = {ext, len};
        bentry(&b, INPUT_ROUTE_TAG_BIOS_STEM, "SCPH5501", 8);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "duplicate identity tag refused");
        b.n = 0;
        bentry(&b, INPUT_ROUTE_TAG_BOOT_MODE, "fast", 4);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "unknown boot mode refused");
    }
    cases += 10;

    /* Markers and checkpoints. */
    {
        unsigned char m[8] = {0}, cp[INPUT_ROUTE_CHECKPOINT_BYTES] = {0};
        Buf b = {ext, 0};
        input_route_put32(m, 5); m[4] = INPUT_ROUTE_MARKER_GAMEPLAY;
        bentry(&b, INPUT_ROUTE_TAG_MARKER, m, 8);
        input_route_put32(cp, 5);
        bentry(&b, INPUT_ROUTE_TAG_CHECKPOINT, cp, sizeof(cp));
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(!read_bytes(n, &meta) && meta.marker_count == 1 && meta.checkpoint_count == 1,
              "marker at the final boundary");
        n = (uint32_t)build(8, 4, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "marker beyond the final boundary");
        b.n = 0;
        bentry(&b, INPUT_ROUTE_TAG_MARKER, m, 8);
        bentry(&b, INPUT_ROUTE_TAG_MARKER, m, 8);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "marker frames strictly increase");
        b.n = 0;
        m[4] = 3;
        bentry(&b, INPUT_ROUTE_TAG_MARKER, m, 8);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "marker kind");
        b.n = 0;
        bentry(&b, INPUT_ROUTE_TAG_CHECKPOINT, cp, sizeof(cp));
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "checkpoint without marker");
        {
            FILE *f;
            m[4] = INPUT_ROUTE_MARKER_MENU;
            b.n = 0;
            bentry(&b, INPUT_ROUTE_TAG_MARKER, m, 8);
            n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
            f = as_file(bytes, n);
            check(input_route_v3_read(f, &meta, digital, dual, NULL, checkpoints) != NULL,
                  "NULL marker array refuses markers");
            fclose(f);
        }
        cases += 6;
    }

    /* Record rules are the PSXRTI1 rules. */
    n = (uint32_t)build(8, 5, 0, NULL, 0);
    bytes[28 + 8 * 2] = 9;
    check(read_bytes(n, &meta) != NULL, "record sequence");
    n = (uint32_t)build(8, 5, 0, NULL, 0);
    bytes[28 + 8 * 2 + 7] = 1;
    check(read_bytes(n, &meta) != NULL, "record reserved");
    cases += 2;

    printf("input_route_v3_file: %d cases passed\n", cases);
    return 0;
}
