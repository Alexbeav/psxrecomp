/* Authored PSXRTI4 vectors: strict bounds, legacy admission and both ports. */
#include "input_route_v3_file.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static void check(int ok, const char *why) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); }
}
static InputReplayDeviceRun runs[3], back[3];
static InputReplayDevicePort initial[2];
static unsigned char payload[48 + 3*36];
static const char *read_payload(uint32_t length, uint32_t frames) {
    FILE *f = tmpfile(); uint8_t profile[2]; uint32_t count;
    check(f != NULL, "owned temp file");
    check(fwrite(payload, 1, length, f) == length, "write payload");
    rewind(f);
    const char *e = input_replay_devices_read(f, length, frames, back, profile, initial, &count);
    fclose(f); return e;
}
static void restore_payload(void) {
    FILE *f = tmpfile(); check(f != NULL, "codec temp file");
    check(!input_replay_devices_write(f, runs, 3, 4, runs[0].ports), "write stream");
    rewind(f); check(fread(payload, 1, sizeof payload, f) == sizeof payload, "read stream bytes");
    fclose(f);
}
static void error_is(const char *e, const char *want) {
    check(e && !strcmp(e, want), want);
}
static InputDualShockRouteStep dual[INPUT_ROUTE_MAX_STEPS];
static InputRouteMarker marks[INPUT_ROUTE_V3_MAX_MARKERS];
static InputRouteCheckpoint cps[INPUT_ROUTE_V3_MAX_MARKERS];
static void container(void) {
    InputRouteV3 meta = {0}, parsed;
    InputRouteV3Replay rp;
    InputRouteV3ReplayOut rx = {0};
    InputRouteDualShockWord words[4];
    InputRouteMarker end = {4, INPUT_ROUTE_MARKER_END};
    InputRouteCheckpoint cp = {0};
    const uint8_t anchor[8] = {1,2,3,4,5,6,7,8};
    meta.has_identity = 1; meta.marker_count = meta.checkpoint_count = 1;
    strcpy(meta.pin, "0123456789abcdef0123456789abcdef01234567");
    strcpy(meta.disc_serial, "SLUS-00001"); strcpy(meta.bios_stem, "SCPH1001");
    strcpy(meta.boot_mode, "lle"); meta.disc_digest_kind = INPUT_ROUTE_DISC_DIGEST_FILE;
    cp.frame = 4;
    for (unsigned i = 0; i < 4; ++i) {
        words[i].buttons = 0xffff; memset(words[i].axes_ly_lx_ry_rx, 128, 4);
    }
    rx.anchor = anchor; rx.anchor_length = sizeof anchor; rx.settings = "cd_speed=1\n";
    rx.devices = runs; rx.devices_count = 3; rx.device_initial = runs[0].ports;
    FILE *f = tmpfile(); check(f != NULL, "container file");
    check(!input_route_v3_write_ex(f, &meta, NULL, words, 4, &end, &cp, &rx), "write v4");
    unsigned char h[28]; rewind(f); check(fread(h, 1, 28, f) == 28, "header");
    check(!memcmp(h, "PSXRTI4\0", 8) && input_route_le32(h+8) == 4, "device version bump");
    rewind(f); error_is(input_route_v3_read(f, &parsed, NULL, dual, marks, cps), "device replay not admitted");
    rewind(f); check(!input_route_v3_read_ex(f, &parsed, NULL, dual, marks, cps, &rp), "replay reader admits v4");
    check(rp.devices_count == 3 && rp.device_profile[0] == 1 && rp.device_profile[1] == 0,
          "mandatory two-port profile");
    check(fseek(f, rp.devices_offset, SEEK_SET) == 0, "located device stream");
    uint8_t profile[2]; uint32_t count;
    check(!input_replay_devices_read(f, rp.devices_length, 4, back, profile, initial, &count), "located inputs");
    check(back[1].ports[1].buttons == 0xffef && back[2].ports[0].motion[0] == -1024,
          "other port pad and signed mouse boundary values");
    {
        const long original_size = 28 + (long)input_route_le32(h+24) + 4*12;
        const uint32_t entry_bytes = 8 + rp.devices_length;
        unsigned char *copy = (unsigned char *)malloc((size_t)original_size + entry_bytes);
        check(copy != NULL, "duplicate entry buffer"); rewind(f);
        check(fread(copy, 1, (size_t)original_size, f) == (size_t)original_size, "original container");
        const uint32_t body = 28 + input_route_le32(h+24);
        memmove(copy + body + entry_bytes, copy + body, (size_t)original_size - body);
        memcpy(copy + body, copy + rp.devices_offset - 8, entry_bytes);
        input_route_put32(copy+24, input_route_le32(h+24) + entry_bytes);
        FILE *d = tmpfile(); check(d != NULL, "duplicate stream file");
        check(fwrite(copy, 1, (size_t)original_size+entry_bytes, d) == (size_t)original_size+entry_bytes,
              "duplicate entry copy"); rewind(d);
        error_is(input_route_v3_read_ex(d, &parsed, NULL, dual, marks, cps, &rp), "duplicate device stream");
        fclose(d); free(copy);
        /* Re-publish the original offset after the failed parse zeroed rp. */
        rewind(f); check(!input_route_v3_read_ex(f, &parsed, NULL, dual, marks, cps, &rp), "original still valid");
    }
    /* Removing the mandatory stream, or relabelling it as v3, cannot silently
     * fall back to the pad body. Old v3 writer bytes have their own fixture. */
    check(fseek(f, rp.devices_offset - 8, SEEK_SET) == 0, "device entry");
    unsigned char tag[4]; input_route_put32(tag, 0x80000909u);
    check(fwrite(tag, 1, 4, f) == 4, "replace entry with skippable tag");
    rewind(f); error_is(input_route_v3_read_ex(f, &parsed, NULL, dual, marks, cps, &rp), "missing device stream");
    /* Fresh file for the reverse-version guard. */
    fclose(f); f = tmpfile(); check(f != NULL, "reverse-version file");
    check(!input_route_v3_write_ex(f, &meta, NULL, words, 4, &end, &cp, &rx), "write second v4");
    memcpy(h, "PSXRTI3\0", 8); input_route_put32(h+8, 3);
    rewind(f); check(fwrite(h, 1, 28, f) == 28, "relabel as v3");
    rewind(f); error_is(input_route_v3_read_ex(f, &parsed, NULL, dual, marks, cps, &rp), "device replay not admitted");
    fclose(f);
    rx.devices = NULL; rx.devices_count = 0;
    f = tmpfile(); check(f != NULL, "legacy writer file");
    check(!input_route_v3_write_ex(f, &meta, NULL, words, 4, &end, &cp, &rx), "legacy v3 writer");
    rewind(f); check(fread(h, 1, 28, f) == 28 && !memcmp(h, "PSXRTI3\0", 8), "pad-only remains v3");
    rewind(f); check(!input_route_v3_read_ex(f, &parsed, NULL, dual, marks, cps, &rp) && !rp.devices_count,
                     "old replay still admitted");
    fclose(f);
}
int main(void) {
    memset(runs, 0, sizeof runs);
    for (unsigned i = 0; i < 3; ++i) {
        runs[i].frames = i ? 1 : 2;
        runs[i].ports[0].kind = 1; runs[i].ports[0].connected = 1;
        runs[i].ports[0].buttons = i % 3;
        runs[i].ports[0].motion[0] = i == 2 ? -1024 : 300;
        runs[i].ports[0].motion[1] = i == 2 ? 1024 : -300;
        runs[i].ports[1].connected = 1; runs[i].ports[1].buttons = i == 1 ? 0xffef : 0xffff;
        memset(runs[i].ports[1].axes, 128, 4);
    }
    restore_payload(); check(!read_payload(sizeof payload, 4), "signed stream round trip");
    check(!memcmp(runs, back, sizeof runs), "all both-port values/durations survive");
    payload[10] = 1; error_is(read_payload(sizeof payload, 4), "device mapping schema");
    restore_payload(); payload[12] ^= 1; error_is(read_payload(sizeof payload, 4), "device mapping schema");
    restore_payload(); payload[16] = 2; error_is(read_payload(sizeof payload, 4), "device initial input");
    restore_payload(); input_route_put32(payload+4, INPUT_ROUTE_MAX_STEPS+1);
    error_is(read_payload(sizeof payload, 4), "device stream length");
    restore_payload(); input_route_put32(payload+48, 0); error_is(read_payload(sizeof payload, 4), "device frame coverage");
    restore_payload(); input_route_put32(payload+48, UINT32_MAX); error_is(read_payload(sizeof payload, 4), "device frame coverage");
    restore_payload(); error_is(read_payload(sizeof payload, 5), "device frame coverage");
    restore_payload(); payload[52+8] = 1; payload[52+9] = 4; error_is(read_payload(sizeof payload, 4), "device input");
    restore_payload(); payload[52] = 0; error_is(read_payload(sizeof payload, 4), "device input");
    restore_payload(); payload[53] = 2; error_is(read_payload(sizeof payload, 4), "device input");
    restore_payload(); memcpy(payload+48+36+4, payload+48+4, 32);
    error_is(read_payload(sizeof payload, 4), "duplicate device run");
    /* A partial write trims a run; the uncompleted trailing frame is omitted. */
    FILE *f = tmpfile(); check(f != NULL, "partial file");
    check(!input_replay_devices_write(f, runs, 3, 1, runs[0].ports), "partial prefix"); rewind(f);
    uint8_t profile[2]; uint32_t count;
    check(!input_replay_devices_read(f, 48+36, 1, back, profile, initial, &count) && count == 1 && back[0].frames == 1,
          "partial frame coverage"); fclose(f);
    container();
    {
        InputReplayDeviceRun *many = (InputReplayDeviceRun *)calloc(INPUT_ROUTE_MAX_STEPS, sizeof *many);
        check(many != NULL, "maximum density buffer");
        for (uint32_t i=0;i<INPUT_ROUTE_MAX_STEPS;++i) {
            many[i].frames=1; memcpy(many[i].ports,runs[0].ports,sizeof many[i].ports);
            many[i].ports[0].motion[0]=(int16_t)(i % 1025);
        }
        f=tmpfile(); check(f != NULL,"maximum density file");
        check(!input_replay_devices_write(f,many,INPUT_ROUTE_MAX_STEPS,INPUT_ROUTE_MAX_STEPS,many[0].ports),
              "maximum valid change count"); rewind(f);
        const uint32_t length=48+36*INPUT_ROUTE_MAX_STEPS;
        check(length < INPUT_ROUTE_V3_MAX_EXT/4,"stream stays below one quarter of extension cap");
        check(!input_replay_devices_read(f,length,INPUT_ROUTE_MAX_STEPS,NULL,profile,initial,&count) &&
              count == INPUT_ROUTE_MAX_STEPS,"maximum stream admitted without allocation"); fclose(f);
        check(!input_replay_devices_prefix(many,INPUT_ROUTE_MAX_STEPS+1,1),"extra change refused before array access");
        many[0].frames=INPUT_ROUTE_MAX_FRAMES;
        check(input_replay_devices_prefix(many,1,INPUT_ROUTE_MAX_FRAMES)==1,"maximum frame coverage");
        check(!input_replay_devices_prefix(many,1,INPUT_ROUTE_MAX_FRAMES+1),"extra frame refused"); free(many);
    }
    printf("input_replay_devices: %u checks passed\n", checks); return 0;
}
