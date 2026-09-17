#ifndef PSX_INPUT_ROUTE_FILE_H
#define PSX_INPUT_ROUTE_FILE_H

/* Bounded PSXRTI1 digital route reader. Source-owned, no retail data.
 * Parse into caller staging storage; publish only after the complete file
 * passes. No guest state, clocks or live input are touched by this parser. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define INPUT_ROUTE_MAX_STEPS 4096u
#define INPUT_ROUTE_MAX_FRAMES 1000000u
typedef struct { uint32_t frames; uint16_t buttons; } InputRouteStep;

static inline uint32_t input_route_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Record loop shared by PSXRTI1 and PSXRTI3 digital bodies. The step count is
 * published only when all `n` records pass. */
static inline const char *input_route_read_records(FILE *f, uint32_t n,
                                                  InputRouteStep *steps,
                                                  uint32_t *step_count)
{
    unsigned char r[8];
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t pad;
        if (fread(r, 1, sizeof(r), f) != sizeof(r)) return "short record";
        if (input_route_le32(r) != i+1 || r[6] || r[7])
            return "record sequence/reserved";
        pad = (uint16_t)((unsigned)r[4] | ((unsigned)r[5] << 8));
        if (count && steps[count-1].buttons == pad) ++steps[count-1].frames;
        else {
            if (count == INPUT_ROUTE_MAX_STEPS) return "step capacity";
            steps[count].buttons = pad;
            steps[count++].frames = 1;
        }
    }
    *step_count = count;
    return NULL;
}

static inline const char *input_route_read(FILE *f, InputRouteStep *steps,
                                          uint32_t *step_count,
                                          uint32_t *frame_count)
{
    unsigned char h[24];
    uint32_t n, count = 0;
    const char *error;
    *step_count = *frame_count = 0;
    if (!f || fread(h, 1, sizeof(h), f) != sizeof(h)) return "short header";
    if (memcmp(h, "PSXRTI1\0", 8) || input_route_le32(h+8) != 1 ||
        input_route_le32(h+12) != 8) return "header identity";
    n = input_route_le32(h+16);
    if (input_route_le32(h+20) || !n || n > INPUT_ROUTE_MAX_FRAMES)
        return "frame count";
    error = input_route_read_records(f, n, steps, &count);
    if (error) return error;
    if (fgetc(f) != EOF || ferror(f)) return "trailing bytes/read error";
    *step_count = count;
    *frame_count = n;
    return NULL;
}
#endif
