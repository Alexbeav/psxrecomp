/* PS1B-192: the SPU envelope unit (spu_envelope.h) against oracle fixture set
 * S-spu E2-E7 (see spu_envelope_fixture.txt).
 *
 * C/R: a voice keyed on at cycle 0 with the given ADSR registers, ticked at
 *      3976 + 768n cycles (the tick grid observed in E7), with an optional
 *      current-level register write at envx_rel. Every logged read must equal
 *      the level after the last tick at or before it. [ORACLE FIXTURE E2, E7]
 * D:   exponential-decrease traces from 3FFFh: the sequence of distinct levels
 *      must have the logged length, CRC-32 and final value. [ORACLE FIXTURE E2]
 *
 * argv[1]: spu_envelope_fixture.txt */
#include "spu_envelope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

typedef struct {
    uint16_t lo, hi;
    long envx_rel;
    int envx_value;
    uint16_t level;
    uint32_t counter;
    uint8_t phase;
    long next_tick;
    int written;
} Case;

static void case_start(Case *c)
{
    c->level = 0; c->counter = 0; c->phase = SPU_ENV_ATTACK;
    c->next_tick = 3976; c->written = c->envx_rel < 0;
}

/* Advance the case to cycle rel and return the level a read there sees. */
static int case_read(Case *c, long rel)
{
    for (;;) {
        long next_event = c->next_tick;
        if (!c->written && c->envx_rel < next_event) next_event = c->envx_rel;
        if (next_event > rel) break;
        if (!c->written && c->envx_rel == next_event) {
            c->level = (uint16_t)(int16_t)c->envx_value;
            c->written = 1;
            continue;
        }
        spu_env_adsr_tick(&c->level, &c->counter, &c->phase, c->lo, c->hi);
        c->next_tick += 768;
    }
    return (int16_t)c->level;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s spu_envelope_fixture.txt\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", argv[1]); return 1; }
    char line[256];
    Case c; int have_case = 0;
    unsigned cases = 0, reads = 0, traces = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'C') {
            unsigned lo, hi; long rel; int value;
            if (sscanf(line + 1, "%u %u %ld %d", &lo, &hi, &rel, &value) != 4) continue;
            memset(&c, 0, sizeof c);
            c.lo = (uint16_t)lo; c.hi = (uint16_t)hi; c.envx_rel = rel; c.envx_value = value;
            case_start(&c); have_case = 1; ++cases;
        } else if (line[0] == 'R' && have_case) {
            long rel; int want;
            if (sscanf(line + 1, "%ld %d", &rel, &want) != 2) continue;
            int got = case_read(&c, rel);
            ++reads;
            if (got != want) {
                if (bad < 10) fprintf(stderr, "FAIL case %u (lo %04X hi %04X) read at +%ld: %d, fixture %d\n",
                                      cases, c.lo, c.hi, rel, got, want);
                ++bad;
            }
        } else if (line[0] == 'D') {
            unsigned shift, step, count; int start, last; unsigned long crc_want;
            if (sscanf(line + 1, "%u %u %d %u %lu %d", &shift, &step, &start, &count, &crc_want, &last) != 6) continue;
            int32_t level = start; uint32_t counter = 0; uint32_t crc = 0; unsigned n = 0; int prev = 0;
            for (unsigned guard = 0; n < count && guard < 50000000u; ++guard) {
                if (n == 0 || level != prev) {
                    int16_t v = (int16_t)level; uint8_t b[2] = { (uint8_t)v, (uint8_t)((uint16_t)v >> 8) };
                    crc = crc32_update(crc, b, 2); prev = level; ++n;
                    if (n == count) break;
                }
                spu_env_tick(&level, &counter, 1, 1, shift, step, 0);
            }
            ++traces;
            if (n != count || crc != (uint32_t)crc_want || prev != last) {
                if (bad < 10) fprintf(stderr, "FAIL decrease trace shift %u step %u: %u values, last %d\n",
                                      shift, step, n, prev);
                ++bad;
            }
        }
    }
    fclose(f);
    if (cases != 34 || reads != 30759 || traces != 128 || bad) {
        fprintf(stderr, "FAIL: %u mismatches (%u cases, %u reads, %u traces)\n", bad, cases, reads, traces);
        return 1;
    }
    printf("SPU envelope (oracle fixtures E2, E7): %u cases, %u reads, %u decrease traces match\n",
           cases, reads, traces);
    return 0;
}
