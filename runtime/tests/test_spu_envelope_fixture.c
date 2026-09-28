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
 * T/S/r: E1 (rates), E3 (Decay exit), E5 (Release after key off) and E9 (volume
 *      sweep) traces; see run_traces. [ORACLE FIXTURE E1, E3, E5, E9]
 *
 * The same trace check covers set S-spu E1R-E10-E8b classes E1R (Release at
 *      every shift) and E10 (voice sweeps with no voice keyed on).
 *      [ORACLE FIXTURE E1R, E10]
 *
 * argv[1]: spu_envelope_fixture.txt  argv[2]: spu_envelope_fixture_e1_e9.txt
 * argv[3]: spu_envelope_fixture_e1r_e10.txt */
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

/* ---- E1/E3/E5/E9 traces (spu_envelope_fixture_e1_e9.txt) ----------------
 * Each trace is every logged level of one register while polled. The model
 * trace is the unit's sequence of distinct levels with the tick of each change.
 * The logged trace is aligned on its first level; then every level must match
 * (CRC-32 over the whole trace) and, for the kept rows, the timer difference
 * between rows must equal 768 cycles per model tick, modulo the 16-bit timer,
 * within 1.5 poll periods plus 60 cycles. */
typedef struct { uint32_t tick; int32_t value; } Change;

static Change *model_trace(char kind, unsigned lo, unsigned hi, unsigned mode, int start,
                           unsigned need, unsigned *n_out)
{
    Change *ch = malloc(sizeof *ch * (need + 1u));
    unsigned n = 0;
    uint16_t level = 0; uint32_t counter = 0; uint8_t phase = SPU_ENV_ATTACK;
    int16_t sweep = 0;
    if (kind == 'T' && mode == 1) { level = (uint16_t)(int16_t)start; phase = SPU_ENV_RELEASE; }
    int32_t cur = kind == 'T' ? (int16_t)level : 0;
    ch[n++] = (Change){ 0, cur };
    for (uint32_t tick = 1; n < need + 1u && tick < 4000000u; ++tick) {
        int32_t next;
        if (kind == 'T') { spu_env_adsr_tick(&level, &counter, &phase, (uint16_t)lo, (uint16_t)hi); next = (int16_t)level; }
        else { spu_env_sweep_tick(&sweep, &counter, (uint16_t)lo); next = sweep; }
        if (next != cur) { ch[n++] = (Change){ tick, next }; cur = next; }
        if (kind == 'T' && phase == SPU_ENV_RELEASE && cur == 0) break;
    }
    *n_out = n;
    return ch;
}

static unsigned run_traces(const char *path, unsigned *traces_out, unsigned *rows_out)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", path); return 1; }
    char line[256];
    unsigned bad = 0, traces = 0, rows = 0;
    Change *m = NULL;
    unsigned mn = 0, j = 0, P = 0, count = 0, lo = 0, hi = 0, mode = 0;
    uint32_t crc_want = 0; int last = 0, start = 0, skip = 1, aligned = 0;
    char kind = 0;
    long prev_idx = 0, prev_t = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'T' || line[0] == 'S') {
            unsigned long crcw;
            int ok;
            kind = line[0]; lo = hi = mode = 0; start = 0;
            if (kind == 'T')
                ok = sscanf(line + 1, "%u %u %u %d %u %u %lu %d", &lo, &hi, &mode, &start, &P, &count, &crcw, &last) == 8;
            else
                ok = sscanf(line + 1, "%u %u %u %lu %d", &lo, &P, &count, &crcw, &last) == 5;
            skip = !ok;
            if (!ok) continue;
            crc_want = (uint32_t)crcw;
            free(m);
            m = model_trace(kind, lo, hi, mode, start, count + 64u, &mn);
            ++traces; aligned = 0;
        } else if (line[0] == 'r' && m && !skip) {
            long idx, t; int value;
            if (sscanf(line + 1, "%ld %ld %d", &idx, &t, &value) != 3) continue;
            ++rows;
            if (!aligned) {
                /* Row 0: align on the first logged level, then check the whole trace's levels. */
                unsigned a = 0;
                while (a < mn && m[a].value != value) ++a;
                if (idx != 0 || a + count > mn) {
                    if (bad < 10) fprintf(stderr, "FAIL trace %u (%c %u %u): first level %d not reached\n", traces, kind, lo, hi, value);
                    ++bad; skip = 1; continue;
                }
                j = a;
                uint32_t crc = 0;
                for (unsigned k = 0; k < count; ++k) {
                    int16_t v = (int16_t)m[j + k].value;
                    uint8_t b[2] = { (uint8_t)v, (uint8_t)((uint16_t)v >> 8) };
                    crc = crc32_update(crc, b, 2);
                }
                if (crc != crc_want || m[j + count - 1].value != last) {
                    if (bad < 10) fprintf(stderr, "FAIL trace %u (%c %u %u): level sequence differs\n", traces, kind, lo, hi);
                    ++bad; skip = 1; continue;
                }
                aligned = 1; prev_idx = 0; prev_t = t;
                continue;
            }
            if ((unsigned long)idx >= count || m[j + idx].value != value) {
                if (bad < 10) fprintf(stderr, "FAIL trace %u row %ld: level %d\n", traces, idx, value);
                ++bad; skip = 1; continue;
            }
            long dt = t - prev_t;
            long dticks = (long)m[j + idx].tick - (long)m[j + prev_idx].tick;
            long diff = ((dt - 768 * dticks) % 65536 + 65536 + 32768) % 65536 - 32768;
            if (labs(diff) * 2 > 3 * (long)P + 120) {
                if (bad < 10) fprintf(stderr, "FAIL trace %u (%c %u %u) row %ld: interval off by %ld cycles\n",
                                      traces, kind, lo, hi, idx, diff);
                ++bad; skip = 1; continue;
            }
            prev_idx = idx; prev_t = t;
        }
    }
    free(m);
    fclose(f);
    *traces_out = traces; *rows_out = rows;
    return bad;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s spu_envelope_fixture.txt spu_envelope_fixture_e1_e9.txt "
                "spu_envelope_fixture_e1r_e10.txt\n", argv[0]);
        return 2;
    }
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
            for (unsigned guard = 0; n < count && guard < 4000000u; ++guard) {
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
    unsigned traces2 = 0, rows2 = 0, traces3 = 0, rows3 = 0;
    bad += run_traces(argv[2], &traces2, &rows2);
    bad += run_traces(argv[3], &traces3, &rows3);
    if (cases != 34 || reads != 30759 || traces != 128 || traces2 != 1114 || rows2 != 53625 ||
        traces3 != 106 || rows3 != 5815 || bad) {
        fprintf(stderr, "FAIL: %u mismatches (E2/E7: %u cases, %u reads, %u traces; E1-E9: %u traces, %u rows)\n",
                bad, cases, reads, traces, traces2, rows2);
        return 1;
    }
    printf("SPU envelope (oracle fixtures E2, E7): %u cases, %u reads, %u decrease traces match\n",
           cases, reads, traces);
    printf("SPU envelope and sweep (oracle fixtures E1, E3, E5, E9): %u traces, %u timed rows match\n",
           traces2, rows2);
    printf("SPU release and idle-voice sweep (oracle fixtures E1R, E10): %u traces, %u timed rows match\n",
           traces3, rows3);
    return 0;
}
