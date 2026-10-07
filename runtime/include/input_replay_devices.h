#ifndef PSX_INPUT_REPLAY_DEVICES_H
#define PSX_INPUT_REPLAY_DEVICES_H

#include "input_route_file.h"
#include <string.h>

/* PSXRTI4 mandatory device stream. The other port's pad is included too.
 * Host mapping has already happened: mouse motion is the bounded pending
 * accumulator, neGcon axes are twist/I/II/L, GunCon X/Y are wire coordinates.
 * A boundary installs this state once; it must not reset consumed mouse
 * motion during a second, low-latency host sample.
 * Header: u32 schema=1,count; u8 kind[2],reserved[2]=0; i32 GunCon offset=-11;
 * then two 16-byte initial inputs for the anchor-load settling frame.
 * Each run: u32 duration, then two 16-byte port inputs encoded below. */
#define INPUT_REPLAY_DEVICE_HEADER_BYTES 48u
#define INPUT_REPLAY_DEVICE_RUN_BYTES 36u
#define INPUT_REPLAY_GUNCON_X_OFFSET (-11)
typedef struct {
    uint8_t kind, connected; /* 0 pad, 1 mouse, 2 neGcon, 3 GunCon */
    uint16_t buttons;       /* mouse: pressed bits 0/1; others active low */
    uint8_t axes[4];        /* pad LX/LY/RX/RY; neGcon twist/I/II/L */
    int16_t motion[2];      /* mouse pending X/Y, -1024..1024 */
    uint16_t xy[2];         /* GunCon; no light = 1,10 */
} InputReplayDevicePort;
typedef struct {
    uint32_t frames;
    InputReplayDevicePort ports[2];
} InputReplayDeviceRun;

static inline void input_replay_device_put32(unsigned char *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8*i)); }
static inline uint16_t input_replay_device_le16(const unsigned char *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static inline void input_replay_device_put16(unsigned char *p, uint16_t v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static inline int input_replay_device_port_ok(const InputReplayDevicePort *p)
{
    if (p->kind > 3 || p->connected > 1) return 0;
    if (p->kind != 1 && (p->motion[0] || p->motion[1])) return 0;
    if (p->kind != 3 && (p->xy[0] || p->xy[1])) return 0;
    if ((p->kind == 1 || p->kind == 3) &&
        (p->axes[0] || p->axes[1] || p->axes[2] || p->axes[3])) return 0;
    if (p->kind == 1 && (p->buttons > 3 ||
        p->motion[0] < -1024 || p->motion[0] > 1024 ||
        p->motion[1] < -1024 || p->motion[1] > 1024)) return 0;
    if (p->kind == 2 && (p->buttons & 0xc707u) != 0xc707u) return 0;
    if (p->kind == 3 && ((p->buttons & 0x9ff7u) != 0x9ff7u ||
        (p->xy[0] == 1 ? p->xy[1] != 10 : p->xy[0] < 2))) return 0;
    return 1;
}

static inline void input_replay_device_encode(unsigned char out[16],
                                              const InputReplayDevicePort *p)
{
    out[0] = p->kind; out[1] = p->connected;
    input_replay_device_put16(out + 2, p->buttons);
    memcpy(out + 4, p->axes, 4);
    input_replay_device_put16(out + 8, (uint16_t)p->motion[0]);
    input_replay_device_put16(out + 10, (uint16_t)p->motion[1]);
    input_replay_device_put16(out + 12, p->xy[0]);
    input_replay_device_put16(out + 14, p->xy[1]);
}
static inline void input_replay_device_decode(InputReplayDevicePort *p,
                                              const unsigned char in[16])
{
    memset(p, 0, sizeof *p);
    p->kind = in[0]; p->connected = in[1];
    p->buttons = input_replay_device_le16(in + 2);
    memcpy(p->axes, in + 4, 4);
    const uint16_t x = input_replay_device_le16(in + 8);
    const uint16_t y = input_replay_device_le16(in + 10);
    /* Avoid implementation-defined conversion of a negative wire halfword. */
    p->motion[0] = (int16_t)(x <= 32767 ? (int)x : (int)x - 65536);
    p->motion[1] = (int16_t)(y <= 32767 ? (int)y : (int)y - 65536);
    p->xy[0] = input_replay_device_le16(in + 12);
    p->xy[1] = input_replay_device_le16(in + 14);
}

/* Read/validate without allocation when runs is NULL. The profile is fixed
 * for the whole file, at least one port is non-pad, and all frames are covered.
 * Mapping schema 1 names final GunCon coordinates with the fixed -11 offset;
 * live sensitivity, pointer position or CRTC mapping is never rerun on play. */
static inline const char *input_replay_devices_read(FILE *f, uint32_t length,
    uint32_t frames, InputReplayDeviceRun *runs, uint8_t profile[2],
    InputReplayDevicePort initial[2], uint32_t *count)
{
    unsigned char h[INPUT_REPLAY_DEVICE_HEADER_BYTES], b[36], previous[32];
    uint32_t total = 0;
    if (length < sizeof h || fread(h, 1, sizeof h, f) != sizeof h)
        return "device stream header";
    const uint32_t n = input_route_le32(h + 4);
    if (input_route_le32(h) != 1 || h[10] || h[11] ||
        input_route_le32(h + 12) != (uint32_t)INPUT_REPLAY_GUNCON_X_OFFSET)
        return "device mapping schema";
    if (h[8] > 3 || h[9] > 3 || !(h[8] || h[9])) return "device profile";
    if (!n || n > INPUT_ROUTE_MAX_STEPS ||
        length != sizeof h + n * INPUT_REPLAY_DEVICE_RUN_BYTES)
        return "device stream length";
    for (unsigned p = 0; p < 2; ++p) {
        input_replay_device_decode(&initial[p], h + 16 + 16*p);
        if (!input_replay_device_port_ok(&initial[p]) || initial[p].kind != h[8+p])
            return "device initial input";
    }
    for (uint32_t i = 0; i < n; ++i) {
        InputReplayDeviceRun r;
        if (fread(b, 1, sizeof b, f) != sizeof b) return "short device stream";
        r.frames = input_route_le32(b);
        if (!r.frames || r.frames > frames - total) return "device frame coverage";
        total += r.frames;
        for (unsigned p = 0; p < 2; ++p) {
            input_replay_device_decode(&r.ports[p], b + 4 + 16*p);
            if (!input_replay_device_port_ok(&r.ports[p]) || r.ports[p].kind != h[8+p])
                return "device input";
        }
        if (i && !memcmp(previous, b + 4, 32)) return "duplicate device run";
        memcpy(previous, b + 4, 32);
        if (runs) runs[i] = r;
    }
    if (total != frames) return "device frame coverage";
    profile[0] = h[8]; profile[1] = h[9]; *count = n;
    return NULL;
}

/* Recording keeps the current, not-yet-completed frame too. A partial file
 * writes only the prefix closed by its END checkpoint, trimming the last run. */
static inline uint32_t input_replay_devices_prefix(const InputReplayDeviceRun *r,
                                                   uint32_t n, uint32_t frames)
{
    uint32_t total = 0;
    if (!r || !n || n > INPUT_ROUTE_MAX_STEPS || !frames || frames > INPUT_ROUTE_MAX_FRAMES) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!r[i].frames || r[i].frames > INPUT_ROUTE_MAX_FRAMES - total) return 0;
        total += r[i].frames;
        if (total >= frames) return i + 1;
    }
    return 0;
}
static inline const char *input_replay_devices_write(FILE *f, const InputReplayDeviceRun *r,
    uint32_t n, uint32_t frames, const InputReplayDevicePort initial[2])
{
    unsigned char h[INPUT_REPLAY_DEVICE_HEADER_BYTES] = {0}, b[36], previous[32];
    const uint32_t count = input_replay_devices_prefix(r, n, frames);
    if (!count) return "device frame coverage";
    input_replay_device_put32(h, 1); input_replay_device_put32(h + 4, count);
    h[8] = r[0].ports[0].kind; h[9] = r[0].ports[1].kind;
    input_replay_device_put32(h + 12, (uint32_t)INPUT_REPLAY_GUNCON_X_OFFSET);
    if (!(h[8] || h[9])) return "device profile";
    if (!initial) return "device initial input";
    for (unsigned p = 0; p < 2; ++p) {
        if (!input_replay_device_port_ok(&initial[p]) || initial[p].kind != h[8+p])
            return "device initial input";
        input_replay_device_encode(h + 16 + 16*p, &initial[p]);
    }
    if (fwrite(h, 1, sizeof h, f) != sizeof h) return "write device header";
    for (uint32_t i = 0, left = frames; i < count; ++i) {
        const uint32_t take = r[i].frames < left ? r[i].frames : left;
        input_replay_device_put32(b, take);
        for (unsigned p = 0; p < 2; ++p) {
            if (!input_replay_device_port_ok(&r[i].ports[p]) || r[i].ports[p].kind != h[8+p])
                return "device input";
            input_replay_device_encode(b + 4 + 16*p, &r[i].ports[p]);
        }
        if (i && !memcmp(previous, b + 4, 32)) return "duplicate device run";
        memcpy(previous, b + 4, 32);
        if (fwrite(b, 1, sizeof b, f) != sizeof b) return "write device stream";
        left -= take;
    }
    return NULL;
}
#endif
