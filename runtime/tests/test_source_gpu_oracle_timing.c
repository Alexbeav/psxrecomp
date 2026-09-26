/* PS1B-182: replay oracle GPU logs and require the projection to start
 * commands exactly when the oracle did.
 *
 * The cost replay (test_source_gpu_oracle_replay.c) checks what each command
 * costs. This one checks scheduling: every logged event is applied to the
 * model after resynchronising the model's budget to the event's logged
 * pre-state, and the post-state must match exactly:
 *   W  GP0 word: FIFO count, phase and budget after the write;
 *   C  service event: budget after credit and after processing; a second
 *      service at the same cycle (own tick and DMA tick coincide) starts
 *      nothing, as the oracle logs no second event there;
 *   G  GP1 write: FIFO count, phase and budget after;
 *   R  GPUREAD: C0h phase and remaining words after.
 * The model keeps its own FIFO contents from the logged words; budgets are
 * resynchronised from each event's logged pre-state, so renderer (sink)
 * charges need no modelling here.
 * Input: a PS1B-182 log in TSV form on stdin. Exit non-zero on a mismatch. */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 32
typedef struct { char ev; int n; char names[MAXF][32]; } Header;
static Header headers[8];
static int nheaders;
static int column(const Header *h, const char *name)
{
    for (int i = 0; i < h->n; ++i) if (!strcmp(h->names[i], name)) return i;
    fprintf(stderr, "log has no column %s for event %c\n", name, h->ev);
    exit(2);
}

/* Rows are buffered and replayed in cycle order. A service event (C) is
 * written to the log when its run ends, after the rows it caused; it is keyed
 * by its last cycle and replayed before the other rows of that cycle. */
typedef struct { unsigned long long cycle; unsigned long long seq; int rank; char *text; } Row;
static Row *rows;
static size_t nrows, caprows;
static unsigned long long seq;

static int cmp_row(const void *a, const void *b)
{
    const Row *x = a, *y = b;
    if (x->cycle != y->cycle) return x->cycle < y->cycle ? -1 : 1;
    if (x->rank != y->rank) return x->rank - y->rank;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

typedef struct { const char *name; unsigned long long checked, bad; char first[240]; } Tally;
static Tally tallies[16];
static int ntallies;
static void record(const char *name, int ok, const char *detail)
{
    Tally *t = NULL;
    for (int i = 0; i < ntallies; ++i) if (!strcmp(tallies[i].name, name)) t = &tallies[i];
    if (!t) { t = &tallies[ntallies++]; *t = (Tally){ name, 0, 0, "" }; }
    t->checked++;
    if (!ok && !t->bad++) snprintf(t->first, sizeof t->first, "%s", detail);
}

static SourceGPUCommandProjection s;
static int wc[11], cc[8], gc[12], dc[6], rc[5];
static long long pending_sink;

static void split(char *line, char **f, int *n)
{
    *n = 0;
    char *p = line;
    f[(*n)++] = p;
    while ((p = strchr(p, '\t')) && *n < MAXF) { *p++ = 0; f[(*n)++] = p; }
    char *e = f[*n - 1];
    e[strcspn(e, "\r\n")] = 0;
}

static void replay(Row *r)
{
    char *f[MAXF];
    int n;
    split(r->text + 2, f, &n);
    char detail[240];
    switch (r->text[0]) {
    case 'D':
        /* Remember the sink charge and draw state for the dispatch it follows. */
        pending_sink = atoll(f[dc[0]]);
        s.skip_field = (unsigned)atoi(f[dc[1]]);
        s.field_valid = (unsigned)atoi(f[dc[2]]);
        break;
    case 'W': {
        uint32_t word = (uint32_t)strtoul(f[wc[1]], 0, 16);
        s.budget = atoi(f[wc[4]]);
        int count_ok = s.count == (uint32_t)atoi(f[wc[2]]) && s.phase == (uint32_t)atoi(f[wc[3]]);
        snprintf(detail, sizeof detail, "cycle %s word %s: model count %u phase %u, logged %s %s",
                 f[0], f[wc[1]], s.count, s.phase, f[wc[2]], f[wc[3]]);
        record("W pre-state (FIFO/phase)", count_ok, detail);
        source_gpu_command_write(&s, word);
        int ok = s.count == (uint32_t)atoi(f[wc[5]]) && s.phase == (uint32_t)atoi(f[wc[6]]) &&
                 s.budget == atoi(f[wc[7]]);
        snprintf(detail, sizeof detail, "cycle %s word %s: model count %u phase %u budget %d, logged %s %s %s",
                 f[0], f[wc[1]], s.count, s.phase, s.budget, f[wc[5]], f[wc[6]], f[wc[7]]);
        record("W post-state", ok, detail);
        s.dispatch.kind = SOURCE_GPU_DISPATCH_NONE;
        break;
    }
    case 'C': {
        unsigned long long last = strtoull(f[cc[1]], 0, 10), elapsed = strtoull(f[cc[3]], 0, 10);
        int events = atoi(f[cc[2]]);
        s.budget = atoi(f[cc[4]]);
        s.last_update = last - elapsed;
        uint32_t before = s.count;
        source_gpu_command_update(&s, last);
        int after_process = s.budget;
        int ok = after_process == atoi(f[cc[6]]);
        snprintf(detail, sizeof detail, "cycle %llu (%d events, elapsed %llu): model %d, logged %s (count %u->%u)",
                 last, events, elapsed, after_process, f[cc[6]], before, s.count);
        record(events == 1 ? "C service (single)" : "C service (credit-only run)", ok, detail);
        /* A second service call at the same cycle must start nothing. It is
         * checked on a copy so the replay continues from the logged timeline. */
        SourceGPUCommandProjection again = s;
        again.dispatch.kind = SOURCE_GPU_DISPATCH_NONE;
        source_gpu_command_update(&again, last);
        snprintf(detail, sizeof detail, "cycle %llu: repeated service started kind %u", last, again.dispatch.kind);
        record("C same-cycle repeat starts nothing", !again.dispatch.kind && again.count == s.count && again.phase == s.phase, detail);
        s.dispatch.kind = SOURCE_GPU_DISPATCH_NONE;
        break;
    }
    case 'G': {
        s.budget = atoi(f[gc[1]]);
        source_gpu_command_gp1(&s, (uint32_t)strtoul(f[gc[0]], 0, 16));
        int ok = s.count == (uint32_t)atoi(f[gc[8]]) && s.phase == (uint32_t)atoi(f[gc[7]]) &&
                 s.budget == atoi(f[gc[6]]);
        snprintf(detail, sizeof detail, "cycle %s word %s: model count %u phase %u budget %d", f[0], f[gc[0]],
                 s.count, s.phase, s.budget);
        record("G post-state", ok, detail);
        break;
    }
    case 'R': {
        s.budget = atoi(f[rc[4]]);
        source_gpu_command_read(&s);
        int ok = s.phase == (uint32_t)atoi(f[rc[2]]) && s.transfer_words == (uint32_t)atoi(f[rc[3]]);
        snprintf(detail, sizeof detail, "cycle %s: model phase %u words %u, logged %s %s", f[0], s.phase,
                 s.transfer_words, f[rc[2]], f[rc[3]]);
        record("R post-state", ok, detail);
        break;
    }
    default:
        break;
    }
}

static void flush(unsigned long long below)
{
    size_t k = 0;
    while (k < nrows && rows[k].cycle < below) ++k;
    if (!k) return;
    qsort(rows, nrows, sizeof(Row), cmp_row);
    k = 0;
    while (k < nrows && rows[k].cycle < below) {
        replay(&rows[k]);
        free(rows[k].text);
        ++k;
    }
    memmove(rows, rows + k, (nrows - k) * sizeof(Row));
    nrows -= k;
}

int main(void)
{
    static char line[1 << 16];
    source_gpu_command_cold(&s);
    s.field_valid = 1;
    unsigned long long max_cycle = 0;
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "#fields\t", 8)) {
            Header *h = &headers[nheaders++];
            char *p = line + 8;
            h->ev = *p;
            p = strchr(p, '\t');
            while (p) {
                ++p;
                char *q = strchr(p, '\t');
                size_t len = q ? (size_t)(q - p) : strcspn(p, "\r\n");
                memcpy(h->names[h->n], p, len);
                h->names[h->n++][len] = 0;
                p = q;
            }
            if (h->ev == 'W') {
                const char *nm[8] = { "word", "count_before", "phase_before", "budget_before",
                                      "count_after", "phase_after", "budget_after", "cycle" };
                for (int i = 0; i < 7; ++i) wc[i + 1] = column(h, nm[i]);
            } else if (h->ev == 'C') {
                const char *nm[6] = { "last_cycle", "events", "elapsed_total", "budget_before_first",
                                      "budget_after_credit_last", "budget_after_process_last" };
                for (int i = 0; i < 6; ++i) cc[i + 1] = column(h, nm[i]);
            } else if (h->ev == 'G') {
                const char *nm[9] = { "word", "budget_before", "phase_before", "count_before", "display_mode_before",
                                      "dma_direction_before", "budget_after", "phase_after", "count_after" };
                for (int i = 0; i < 9; ++i) gc[i] = column(h, nm[i]);
            } else if (h->ev == 'R') {
                const char *nm[5] = { "phase_before", "transfer_words_before", "phase_after", "transfer_words_after", "budget" };
                for (int i = 0; i < 5; ++i) rc[i] = column(h, nm[i]);
            } else if (h->ev == 'D') {
                dc[0] = column(h, "sink_charge"); dc[1] = column(h, "skip_field"); dc[2] = column(h, "field_valid");
            }
            continue;
        }
        char ev = line[0];
        if (ev != 'W' && ev != 'C' && ev != 'G' && ev != 'D' && ev != 'R') continue;
        const char *tab = strchr(line, '\t');
        if (!tab) continue;
        unsigned long long cycle = strtoull(tab + 1, 0, 10);
        int rank = 1;
        if (ev == 'C') {                 /* key a service run by its last cycle */
            const char *t2 = strchr(tab + 1, '\t');
            cycle = strtoull(t2 + 1, 0, 10);
            rank = 0;
        }
        if (nrows == caprows) { caprows = caprows ? caprows * 2 : 4096; rows = realloc(rows, caprows * sizeof(Row)); }
        rows[nrows++] = (Row){ cycle, seq++, rank, strdup(line) };
        if (cycle > max_cycle) max_cycle = cycle;
        if (nrows >= 200000) flush(max_cycle > 4000000 ? max_cycle - 4000000 : 0);
    }
    flush(~0ull);
    int failed = 0;
    for (int i = 0; i < ntallies; ++i) {
        Tally *t = &tallies[i];
        printf("%-36s %12llu checked %10llu mismatched%s%s\n", t->name, t->checked, t->bad,
               t->bad ? "  first: " : "", t->bad ? t->first : "");
        failed |= t->bad != 0;
    }
    puts(failed ? "FAIL" : "PASS");
    return failed;
}
