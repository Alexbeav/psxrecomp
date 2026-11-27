/* PS1B-601: GP0 transfer mirrors in the source command projection.
 *
 * psx-spx "GPU Command Summary" decodes the transfer commands on the top three
 * bits only: 80h..9Fh are VRAM-to-VRAM copy, A0h..BFh CPU-to-VRAM and
 * C0h..DFh VRAM-to-CPU. Beetle's GP0 table (gpu.cpp) fills each 32-entry row
 * with the same handler. The projection must model a mirror exactly as its
 * base opcode: same packet length, ready threshold, prefetch, cost, data phase
 * and owed words. */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
static void check(int okay, const char *what, unsigned op)
{
    checks++;
    if (!okay) { failures++; fprintf(stderr, "FAIL %02Xh: %s\n", op, what); }
}

static void credited(SourceGPUCommandProjection *s)
{
    source_gpu_command_cold(s);
    source_gpu_command_update(s, 100000);
}

/* Run one packet through a fresh projection and return it. */
static SourceGPUCommandProjection run(unsigned op, const uint32_t *params, unsigned n, int *ok)
{
    SourceGPUCommandProjection s;
    credited(&s);
    *ok = source_gpu_command_write(&s, (uint32_t)op << 24);
    for (unsigned i = 0; i < n; ++i) *ok &= source_gpu_command_write(&s, params[i]) != 0;
    return s;
}

static void same(unsigned base, unsigned op, const uint32_t *params, unsigned n)
{
    int ok_base, ok_op;
    SourceGPUCommandProjection a = run(base, params, n, &ok_base);
    SourceGPUCommandProjection b = run(op, params, n, &ok_op);
    uint32_t head_a = (uint32_t)base << 24, head_b = (uint32_t)op << 24;
    check(source_gpu_command_known(op), "known", op);
    check(source_gpu_command_length(head_b) == source_gpu_command_length(head_a), "packet length", op);
    check(source_gpu_t_ready_below(head_b) == source_gpu_t_ready_below(head_a), "ready threshold", op);
    check(source_gpu_command_prefetch(head_b) == source_gpu_command_prefetch(head_a), "prefetch", op);
    check(ok_base && ok_op, "packet accepted", op);
    check(b.error == a.error && !b.error, "no error", op);
    check(b.phase == a.phase, "data phase", op);
    check(b.transfer_words == a.transfer_words, "transfer words", op);
    check(b.budget == a.budget, "cost", op);
    check(b.count == a.count, "queue depth", op);

    /* Owed words while queued behind negative credit. */
    SourceGPUCommandProjection qa, qb;
    credited(&qa); credited(&qb);
    qa.budget = qb.budget = -100000;
    source_gpu_command_write(&qa, head_a);
    source_gpu_command_write(&qb, head_b);
    for (unsigned i = 0; i < 2 && i < n; ++i) {
        source_gpu_command_write(&qa, params[i]);
        source_gpu_command_write(&qb, params[i]);
    }
    check(source_gpu_command_owed(&qb) == source_gpu_command_owed(&qa), "owed words", op);
}

int main(void)
{
    static const uint32_t copy[] = { 0x00000000u, 0x00100010u, 0x00040008u };
    static const uint32_t xfer[] = { 0x00000000u, 0x00020002u };
    static const unsigned bases[] = { 0x80u, 0xA0u, 0xC0u };
    for (unsigned b = 0; b < 3; ++b) {
        unsigned base = bases[b];
        const uint32_t *params = base == 0x80u ? copy : xfer;
        unsigned n = base == 0x80u ? 3u : 2u;
        const unsigned mirrors[] = { base + 1u, base + 0x0Fu, base + 0x1Fu };
        for (unsigned m = 0; m < 3; ++m) same(base, mirrors[m], params, n);
    }
    if (failures) {
        fprintf(stderr, "source_gpu_transfer_mirrors: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("source_gpu_transfer_mirrors: %u checks passed\n", checks);
    return 0;
}
