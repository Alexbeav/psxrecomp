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
static void braw(Buf *b, const void *data, size_t n) { if (n) memcpy(b->p + b->n, data, n); b->n += n; }
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

static void refuses(size_t n, InputRouteV3 *meta, const char *error, const char *what)
{
    const char *actual = read_bytes(n, meta);
    check(actual && !strcmp(actual, error), what);
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

static FILE *long_route(uint32_t frames, int capacity)
{
    unsigned char h[28] = {0}, entry[16] = {0}, record[8] = {0};
    FILE *f = tmpfile();
    check(f != NULL, "long route tmpfile");
    memcpy(h, "PSXRTI3\0", 8);
    input_route_put32(h + 8, 3); input_route_put32(h + 12, 8);
    input_route_put32(h + 16, frames); input_route_put32(h + 24, capacity ? 16 : 0);
    check(fwrite(h, 1, sizeof(h), f) == sizeof(h), "long route header");
    if (capacity) {
        input_route_put32(entry, INPUT_ROUTE_TAG_CAPACITY);
        input_route_put32(entry + 4, 8);
        input_route_put32(entry + 8, 1);
        input_route_put32(entry + 12, INPUT_ROUTE_V3_CAPACITY_V1_FRAMES);
        check(fwrite(entry, 1, sizeof(entry), f) == sizeof(entry), "capacity entry");
    }
    record[4] = record[5] = 0xff;
    for (uint32_t i = 0; i < frames; ++i) {
        input_route_put32(record, i + 1);
        check(fwrite(record, 1, sizeof(record), f) == sizeof(record), "long route record");
    }
    rewind(f);
    return f;
}

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

    /* Capacity admission is exact, unique, versioned and bounded. */
    {
        unsigned char capacity[8];
        Buf b = {ext, 0};
        input_route_put32(capacity, 1);
        input_route_put32(capacity + 4, INPUT_ROUTE_V3_CAPACITY_V1_FRAMES);
        FILE *f = long_route(INPUT_ROUTE_MAX_FRAMES + 1u, 0);
        const char *err = input_route_v3_read(f, &meta, digital, NULL, markers, checkpoints);
        fclose(f);
        check(err && !strcmp(err, "long route lacks capacity admission"),
              "long route without capacity admission refused");
        f = long_route(INPUT_ROUTE_MAX_FRAMES + 1u, 1);
        err = input_route_v3_read(f, &meta, digital, NULL, markers, checkpoints);
        fclose(f);
        check(!err && meta.frames == INPUT_ROUTE_MAX_FRAMES + 1u &&
              meta.capacity_version == 1u && meta.steps == 1u,
              "capacity v1 admits a lossless long route");
        bentry(&b, INPUT_ROUTE_TAG_CAPACITY, capacity, 8);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        capacity[0] = 2;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_CAPACITY, capacity, 8);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "capacity admission",
                "unknown capacity version refused");
        input_route_put32(capacity, 1); input_route_put32(capacity + 4, 3999999u);
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_CAPACITY, capacity, 8);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "capacity admission",
                "wrong capacity ceiling refused");
        input_route_put32(capacity + 4, INPUT_ROUTE_V3_CAPACITY_V1_FRAMES);
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_CAPACITY, capacity, 8);
        bentry(&b, INPUT_ROUTE_TAG_CAPACITY, capacity, 8);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "duplicate capacity admission",
                "duplicate capacity admission refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_CAPACITY, capacity, 4);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "capacity admission",
                "malformed capacity length refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_CAPACITY, capacity, 8);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        input_route_put32(bytes + 16, INPUT_ROUTE_V3_CAPACITY_V1_FRAMES);
        refuses(n, &meta, "file size", "four-million header reaches the file-size gate");
        input_route_put32(bytes + 16, INPUT_ROUTE_V3_CAPACITY_V1_FRAMES + 1u);
        refuses(n, &meta, "frame count", "four-million-and-one frames refused");
        cases += 8;
    }

    /* Tags. */
    {
        Buf b = {ext, 0};
        bentry(&b, 0x80000300u, "skip", 4);
        bentry(&b, 0x8000ffffu, "x", 1);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(!read_bytes(n, &meta), "unknown skippable tags are skipped");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\1\0\0\0\1", 5);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(!read_bytes(n, &meta) && meta.port_count == 1 &&
              meta.port_devices[0] == INPUT_ROUTE_DEVICE_DIGITAL,
              "declared digital port layout admitted");
        bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\1\0\0\0\1", 5);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "duplicate port layout",
                "duplicate port layout refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\1\0\0\0", 4);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "port layout length",
                "short port layout refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\0\0\0\0\1", 5);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "port layout",
                "zero port count refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\3\0\0\0\1\1", 6);
        refuses(build(12, 5, 0, ext, (uint32_t)b.n), &meta, "port layout",
                "port count overflow refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\1\1\0\0\1", 5);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "port layout",
                "port reserved byte refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\1\0\0\0\3", 5);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "port device",
                "unknown port device refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\1\0\0\0\1", 5);
        refuses(build(12, 5, 0, ext, (uint32_t)b.n), &meta, "port record size",
                "port record-size mismatch refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\1\0\0\0\2", 5);
        n = (uint32_t)build(12, 5, 0, ext, (uint32_t)b.n);
        check(!read_bytes(n, &meta) && meta.port_devices[0] == INPUT_ROUTE_DEVICE_DUALSHOCK,
              "declared DualShock port layout admitted");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_PORT_LAYOUT, "\2\0\0\0\1\1", 6);
        refuses(build(12, 5, 0, ext, (uint32_t)b.n), &meta,
                "declared port layout not supported", "correctly sized two-port route refused");
        b.n = 0; bentry(&b, 0x800001ffu, "x", 1);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "unknown 0x800001xx refused");
        b.n = 0; bentry(&b, 0x80000300u, "x", 1); ext[b.n - 1] = 1;
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "nonzero padding");
        b.n = 0; b32(&b, 0x80000300u); b32(&b, 64);
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(read_bytes(n, &meta) != NULL, "entry longer than block");
        cases += 14;
    }

    /* Console events are ordered boundary actions bound to a verified disc
     * roster. No event or disc index can be skipped or invented. */
    {
        unsigned char discs[76] = {0}, events[28] = {0};
        unsigned char many_events[4 + 8 * (INPUT_ROUTE_V3_MAX_EVENTS + 1)] = {0};
        unsigned char many_discs[4 + 36 * (INPUT_ROUTE_V3_MAX_DISCS + 1)] = {0};
        Buf b = {ext, 0};
        input_route_put32(discs, 2);
        discs[4] = discs[40] = INPUT_ROUTE_DISC_DIGEST_FILE;
        for (unsigned i = 0; i < 32; ++i) {
            discs[8 + i] = (unsigned char)i;
            discs[44 + i] = (unsigned char)(0x80 + i);
        }
        input_route_put32(events, 3);
        input_route_put32(events + 4, 2); events[8] = INPUT_ROUTE_EVENT_TRAY_OPEN;
        input_route_put32(events + 12, 3); events[16] = INPUT_ROUTE_EVENT_DISC_SELECT; events[17] = 2;
        input_route_put32(events + 20, 3); events[24] = INPUT_ROUTE_EVENT_TRAY_CLOSE; events[25] = 2;
        bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        bentry(&b, INPUT_ROUTE_TAG_DISC_SET, discs, sizeof(discs));
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        check(!read_bytes(n, &meta) && meta.event_count == 3 && meta.disc_count == 2 &&
              meta.events[0].event == INPUT_ROUTE_EVENT_TRAY_OPEN &&
              meta.events[1].event == INPUT_ROUTE_EVENT_DISC_SELECT &&
              meta.events[2].event == INPUT_ROUTE_EVENT_TRAY_CLOSE &&
              meta.events[1].frame == meta.events[2].frame && meta.events[1].disc_index == 2,
              "same-frame event order and disc set admitted");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        n = (uint32_t)build(8, 5, 0, ext, (uint32_t)b.n);
        refuses(n, &meta, "console events require disc set", "events without disc set refused");
        bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "duplicate console events",
                "duplicate console events refused");
        input_route_put32(many_events, INPUT_ROUTE_V3_MAX_EVENTS + 1u);
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, many_events, sizeof(many_events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "console events length",
                "console event count overflow refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, 4);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "console events length",
                "malformed console event length refused");
        events[10] = 1;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta,
                "console event frame/reserved/order", "console event reserved byte refused");
        events[10] = 0; input_route_put32(events + 12, 1);
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta,
                "console event frame/reserved/order", "decreasing console event frame refused");
        input_route_put32(events + 12, 3); input_route_put32(events + 4, 6);
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta,
                "console event frame/reserved/order", "out-of-range console event frame refused");
        input_route_put32(events + 4, 2); events[8] = 9;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "console event kind",
                "unknown console event kind refused");
        events[8] = INPUT_ROUTE_EVENT_TRAY_OPEN; events[9] = 1;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "console event disc",
                "tray event with nonzero disc refused");
        events[9] = 0; events[16] = INPUT_ROUTE_EVENT_DISC_SELECT; events[17] = 0;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "console event disc",
                "disc event with zero disc refused");
        events[17] = 3;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_EVENTS, events, sizeof(events));
        bentry(&b, INPUT_ROUTE_TAG_DISC_SET, discs, sizeof(discs));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "console event disc index",
                "event outside disc set refused");

        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_DISC_SET, discs, sizeof(discs));
        bentry(&b, INPUT_ROUTE_TAG_DISC_SET, discs, sizeof(discs));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "duplicate disc set",
                "duplicate disc set refused");
        input_route_put32(many_discs, INPUT_ROUTE_V3_MAX_DISCS + 1u);
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_DISC_SET, many_discs, sizeof(many_discs));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "disc set length",
                "disc count overflow refused");
        memset(many_discs, 0, sizeof(many_discs));
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_DISC_SET, many_discs, 4);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "disc set length",
                "zero disc count refused");
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_DISC_SET, discs, 4);
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "disc set length",
                "malformed disc set length refused");
        discs[4] = 3;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_DISC_SET, discs, sizeof(discs));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "disc set kind/reserved",
                "unknown disc digest kind refused");
        discs[4] = INPUT_ROUTE_DISC_DIGEST_FILE; discs[5] = 1;
        b.n = 0; bentry(&b, INPUT_ROUTE_TAG_DISC_SET, discs, sizeof(discs));
        refuses(build(8, 5, 0, ext, (uint32_t)b.n), &meta, "disc set kind/reserved",
                "disc set reserved byte refused");
        cases += 20;
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

    /* PS1B-316 power-on replay: POWER_ON + CARDS instead of an anchor, and
     * the optional PRODUCT lines. */
    {
        static unsigned char card2[INPUT_ROUTE_REPLAY_CARD_BYTES];
        static InputRouteCheckpoint end;
        InputRouteDualShockWord words[6];
        InputRouteMarker end_marker = {6, INPUT_ROUTE_MARKER_END};
        InputRouteV3 w;
        InputRouteV3ReplayOut rx;
        InputRouteV3Replay rp;
        size_t size;
        FILE *f;
        memset(&w, 0, sizeof(w));
        w.has_identity = 1;
        strcpy(w.pin, pin); strcpy(w.disc_serial, "SLUS-00662");
        strcpy(w.bios_stem, "SCPH1001"); strcpy(w.boot_mode, "lle");
        w.disc_digest_kind = INPUT_ROUTE_DISC_DIGEST_FILE;
        w.marker_count = w.checkpoint_count = 1;
        end.frame = 6;
        for (unsigned i = 0; i < 6; ++i) {
            words[i].buttons = (uint16_t)(0xffff ^ (i << 4));
            memset(words[i].axes_ly_lx_ry_rx, 0x80, 4);
        }
        for (unsigned i = 0; i < sizeof(card2); ++i) card2[i] = (unsigned char)(i * 5u + 3u);
        memset(&rx, 0, sizeof(rx));
        rx.settings = "cd_game_speed=2\n";
        rx.power_on = 1;
        rx.cards = card2;
        rx.cards_mask = 2u;                        /* only card 2 inserted */
        rx.product = "exe_sha256=ab\nbios_crc32=1234abcd\n";
        f = tmpfile();
        check(f && !input_route_v3_write_ex(f, &w, NULL, words, 6, &end_marker, &end, &rx), "write power-on replay");
        rewind(f);
        check(!input_route_v3_read_ex(f, &meta, NULL, dual, markers, checkpoints, &rp), "read power-on replay");
        check(rp.power_on && !rp.has_anchor && rp.has_cards && rp.cards_mask == 2u &&
              !strcmp(rp.product, "exe_sha256=ab\nbios_crc32=1234abcd\n") &&
              !strcmp(rp.settings, "cd_game_speed=2\n") && meta.frames == 6, "power-on round trip");
        {
            static unsigned char back[INPUT_ROUTE_REPLAY_CARD_BYTES];
            check(!fseek(f, rp.cards_offset, SEEK_SET) && fread(back, 1, sizeof(back), f) == sizeof(back) &&
                  !memcmp(back, card2, sizeof(back)), "card 2's image at cards_offset");
        }
        rewind(f);
        check(input_route_v3_read(f, &meta, NULL, dual, markers, checkpoints) != NULL,
              "the route reader refuses a power-on replay");
        fseek(f, 0, SEEK_END);
        size = (size_t)ftell(f);
        rewind(f);
        check(size <= sizeof(bytes) && fread(bytes, 1, size, f) == size, "read back bytes");
        fclose(f);
        /* Locate an entry by tag in the extension block. */
        {
            const uint32_t ext_len = input_route_le32(bytes + 24);
            size_t cards_at = 0, power_at = 0;
            for (size_t at = 28; at < 28u + ext_len; ) {
                const uint32_t tag = input_route_le32(bytes + at), len = input_route_le32(bytes + at + 4);
                if (tag == INPUT_ROUTE_TAG_REPLAY_CARDS) cards_at = at;
                if (tag == INPUT_ROUTE_TAG_REPLAY_POWER_ON) power_at = at;
                at += 8u + ((len + 3u) & ~3u);
            }
            check(cards_at && power_at, "POWER_ON and CARDS entries present");
            FILE *g;
            const char *err;
            input_route_put32(bytes + power_at + 8, 5);   /* start vblank must be 0 */
            g = as_file(bytes, size);
            err = input_route_v3_read_ex(g, &meta, NULL, dual, markers, checkpoints, &rp);
            fclose(g);
            check(err && !strcmp(err, "replay power-on vblank"), "power-on at another vblank refused");
            input_route_put32(bytes + power_at + 8, 0);
            input_route_put32(bytes + cards_at, 0x80000399u);   /* CARDS hidden as skippable */
            g = as_file(bytes, size);
            err = input_route_v3_read_ex(g, &meta, NULL, dual, markers, checkpoints, &rp);
            fclose(g);
            check(err && !strcmp(err, "replay cards missing"), "power-on without cards refused");
            input_route_put32(bytes + cards_at, INPUT_ROUTE_TAG_REPLAY_CARDS);
            input_route_put32(bytes + cards_at + 8, 4u);         /* mask bit 2: no such slot */
            g = as_file(bytes, size);
            err = input_route_v3_read_ex(g, &meta, NULL, dual, markers, checkpoints, &rp);
            fclose(g);
            check(err && !strcmp(err, "replay cards length"), "card mask beyond two slots refused");
        }
        /* An anchor and a power-on start cannot both be written. */
        {
            static const unsigned char blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            rx.anchor = blob;
            rx.anchor_length = sizeof(blob);
            f = tmpfile();
            check(f && input_route_v3_write_ex(f, &w, NULL, words, 6, &end_marker, &end, &rx) != NULL,
                  "anchor plus power-on refused by the writer");
            if (f) fclose(f);
        }
        cases += 9;
    }

    printf("input_route_v3_file: %d cases passed\n", cases);
    return 0;
}
