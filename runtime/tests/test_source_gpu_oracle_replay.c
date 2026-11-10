/* PS1B-182: replay oracle GPU logs through the projection and require exact
 * agreement.
 *
 * Input: a PS1B-182 log in TSV form on stdin ("#fields <event> <columns>"
 * header lines, then rows). For every row it checks:
 *   D  dispatch: the projection charge computed from the logged draw state
 *      and packet words equals the logged projection_charge;
 *   C  single credit event: budget_after_credit equals the credit rule;
 *   S  GPUSTAT run: bit 28 of the first and last read equals the readiness
 *      rule for the logged phase, FIFO count, poly-line flag and queue head.
 * Classes the table marks [NOT FITTED] or [NOT OBSERVED] are counted but
 * reported apart; "--strict" fails on them too.
 * Exit status is non-zero on any mismatch in a fitted class. */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 32
typedef struct { char ev; int n; char names[MAXF][32]; } Header;
static Header headers[8];
static int nheaders;

static const Header *header_for(char ev)
{
    for (int i = 0; i < nheaders; ++i) if (headers[i].ev == ev) return &headers[i];
    return NULL;
}
static int column(const Header *h, const char *name)
{
    for (int i = 0; i < h->n; ++i) if (!strcmp(h->names[i], name)) return i;
    fprintf(stderr, "log has no column %s for event %c\n", name, h->ev);
    exit(2);
}

typedef struct { const char *name; unsigned long long checked, bad; int fitted; char first_bad[200]; } Tally;
static Tally tallies[32];
static int ntallies;
static Tally *tally(const char *name, int fitted)
{
    for (int i = 0; i < ntallies; ++i) if (!strcmp(tallies[i].name, name)) return &tallies[i];
    tallies[ntallies] = (Tally){ name, 0, 0, fitted, "" };
    return &tallies[ntallies++];
}
static void record(Tally *t, int ok, const char *detail)
{
    t->checked++;
    if (!ok && !t->bad++) snprintf(t->first_bad, sizeof t->first_bad, "%s", detail);
}

static SourceGPUCommandProjection state_from(char **f, const int *c)
{
    SourceGPUCommandProjection s;
    source_gpu_command_cold(&s);
    s.clip_x0 = atoi(f[c[0]]); s.clip_y0 = atoi(f[c[1]]);
    s.clip_x1 = atoi(f[c[2]]); s.clip_y1 = atoi(f[c[3]]);
    s.offset_x = atoi(f[c[4]]); s.offset_y = atoi(f[c[5]]);
    s.draw_mode = (uint32_t)strtoul(f[c[6]], 0, 16);
    s.texture_window = (uint32_t)strtoul(f[c[7]], 0, 16);
    s.mask_bits = (uint32_t)strtoul(f[c[8]], 0, 16);
    s.display_mode = (uint32_t)strtoul(f[c[9]], 0, 16);
    s.field_valid = (unsigned)atoi(f[c[10]]);
    s.skip_field = (unsigned)atoi(f[c[11]]);
    return s;
}

/* The charge the projection makes for one logged dispatch; *cls names it. */
static long long expected_charge(const SourceGPUCommandProjection *s, int kind,
                                 const uint32_t *w, int segment, const char **cls, int *fitted)
{
    unsigned op = w[0] >> 24;
    *fitted = 1;
    if (kind == 4) { *cls = "A0h data word"; return SOURCE_GPU_T_UPLOAD_WORD; }
    if (source_gpu_polygon_supported(op)) {
        *cls = (op & 0x14u) == 0x14u ? "polygon gouraud+textured" : "polygon";
        return source_gpu_command_polygon_cost(s, w, kind == 3);
    }
    if (source_gpu_sprite_opcode(op)) { *cls = "rectangle"; return SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_block_cost(s, w); }
    if (op == 0x02u) { *cls = "fill"; return SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_block_cost(s, w); }
    if (op == 0x80u) { *cls = "copy"; return SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_block_cost(s, w); }
    if (source_gpu_line_supported(op)) {
        *cls = segment ? "poly-line segment" : "line (opening)";
        return (segment ? 0 : SOURCE_GPU_T_COMMAND_OVERHEAD) + source_gpu_command_line_cost(s, w);
    }
    if (source_gpu_command_immediate(op)) { *cls = "NOP/E3h-E5h"; return 0; }
    *cls = "attribute/transfer set-up";
    return SOURCE_GPU_T_COMMAND_OVERHEAD;
}

int main(int argc, char **argv)
{
    int strict = argc > 1 && !strcmp(argv[1], "--strict");
    static char line[1 << 16];
    int dc[12], dk = 0, dcharge = 0, dwords = 0;
    int cev = 0, cbefore = 0, cafter = 0, celapsed = 0;
    int sphase = 0, scount = 0, spline = 0, sfv = 0, slv = 0, sfh = 0, slh = 0, wword = 0;
    /* A poly-line opens with its first line dispatch and closes on a
     * terminator word; line dispatches in between are segments. */
    int pline_open = 0, close_after = 0, wcount = 0;
    while (fgets(line, sizeof line, stdin)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') line[--len] = 0;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!strncmp(line, "#fields\t", 8)) {
            Header *h = &headers[nheaders++];
            char *p = line + 8;
            h->ev = *p;
            p = strchr(p, '\t');
            while (p) {
                ++p;
                char *q = strchr(p, '\t');
                size_t n = q ? (size_t)(q - p) : strlen(p);
                memcpy(h->names[h->n], p, n);
                h->names[h->n++][n] = 0;
                p = q;
            }
            if (h->ev == 'D') {
                const char *names[12] = { "clip_x0", "clip_y0", "clip_x1", "clip_y1", "offset_x", "offset_y",
                                          "draw_mode", "texture_window", "mask_bits", "display_mode",
                                          "field_valid", "skip_field" };
                for (int i = 0; i < 12; ++i) dc[i] = column(h, names[i]);
                dk = column(h, "kind"); dcharge = column(h, "projection_charge"); dwords = column(h, "words");
            } else if (h->ev == 'C') {
                cev = column(h, "events"); cbefore = column(h, "budget_before_first");
                cafter = column(h, "budget_after_credit_last"); celapsed = column(h, "elapsed_total");
            } else if (h->ev == 'W') {
                wword = column(h, "word"); wcount = column(h, "count_before");
            } else if (h->ev == 'S') {
                sphase = column(h, "phase"); scount = column(h, "fifo_count"); spline = column(h, "pline");
                sfv = column(h, "first_value"); slv = column(h, "last_value");
                sfh = column(h, "first_queue_head"); slh = column(h, "last_queue_head");
            }
            continue;
        }
        if (!len || line[0] == '#' || !header_for(line[0])) continue;
        char *f[MAXF];
        int nf = 0;
        char *p = line + 2;
        f[nf++] = p;
        while ((p = strchr(p, '\t')) && nf < MAXF) { *p++ = 0; f[nf++] = p; }
        if (line[0] == 'W') {
            /* The terminator ends the poly-line once the words queued ahead of it
             * (the last segments) have been dispatched. */
            if (pline_open && source_gpu_line_terminator((uint32_t)strtoul(f[wword], 0, 16))) {
                close_after = atoi(f[wcount]);
                if (!close_after) pline_open = 0;
            }
        } else if (line[0] == 'D') {
            uint32_t w[16];
            int nw = 0;
            char *s = f[dwords];
            while (*s && nw < 16) { w[nw++] = (uint32_t)strtoul(s, &s, 16); while (*s == ' ') ++s; }
            SourceGPUCommandProjection st = state_from(f, dc);
            const char *cls;
            int fitted;
            unsigned op = w[0] >> 24;
            int is_line = source_gpu_line_supported(op) && atoi(f[dk]) == 1;
            int segment = is_line && pline_open;
            if (!is_line) pline_open = close_after = 0;
            else if (segment && close_after) {
                close_after -= (int)source_gpu_line_segment_length(op);
                if (close_after <= 0) pline_open = close_after = 0;
            } else if (source_gpu_line_polyline(op)) pline_open = 1;
            long long want = expected_charge(&st, atoi(f[dk]), w, segment, &cls, &fitted);
            long long got = atoll(f[dcharge]);
            char detail[200];
            snprintf(detail, sizeof detail, "logged %lld, model %lld: %s", got, want, f[dwords]);
            record(tally(cls, fitted), got == want, detail);
        } else if (line[0] == 'C') {
            if (atoi(f[cev]) != 1) continue;
            SourceGPUCommandProjection st;
            source_gpu_command_cold(&st);
            st.budget = atoi(f[cbefore]);
            long long want = SOURCE_GPU_T_CREDIT(&st, (uint64_t)atoll(f[celapsed]));
            char detail[200];
            snprintf(detail, sizeof detail, "before %s elapsed %s: logged %s, model %lld", f[cbefore], f[celapsed], f[cafter], want);
            record(tally("credit", 1), atoll(f[cafter]) == want, detail);
        } else if (line[0] == 'S') {
            for (int which = 0; which < 2; ++which) {
                SourceGPUCommandProjection st;
                source_gpu_command_cold(&st);
                st.phase = (uint32_t)atoi(f[sphase]);
                st.count = (uint32_t)atoi(f[scount]);
                st.pline = (unsigned)atoi(f[spline]);
                st.queue[0] = (uint32_t)strtoul(f[which ? slh : sfh], 0, 16);
                unsigned bit = (unsigned)(strtoul(f[which ? slv : sfv], 0, 16) >> 28) & 1u;
                int want = source_gpu_command_ready(&st);
                char detail[200];
                snprintf(detail, sizeof detail, "phase %s count %s head %s: logged %u, model %d",
                         f[sphase], f[scount], f[which ? slh : sfh], bit, want);
                record(tally("GPUSTAT.28", 1), (int)bit == want, detail);
            }
        }
    }
    int failed = 0;
    for (int i = 0; i < ntallies; ++i) {
        Tally *t = &tallies[i];
        int counts = t->fitted || strict;
        printf("%-28s %12llu checked %10llu mismatched%s%s%s\n", t->name, t->checked, t->bad,
               t->fitted ? "" : "  [not fitted]", t->bad ? "  first: " : "", t->bad ? t->first_bad : "");
        if (counts && t->bad) failed = 1;
    }
    if (!ntallies) { fprintf(stderr, "no rows\n"); return 2; }
    puts(failed ? "FAIL" : "PASS");
    return failed;
}
