/* PS1B-520: the dirty-RAM interpreter decides a branch on a [widescreen.cull]
 * branch-site list (bltz_sites, branch_keep_sites, nclip_keep_sites,
 * nclip_exact_sites) as the generated code decides it.
 *
 * Compiled side: the condition the recompiler emits for that site, compiled as
 * generated code compiles it (recompiler/tests/ws_branch_site_cases.h; its text
 * is checked against the generator by recompiler_patch_test).
 * Interpreted side: the production interpreter (dirty_ram_interp.c,
 * exec_one_fetched_inner) with the lists passed to the runtime as main.cpp
 * passes game.toml's lists.
 *
 * Measurement: for each case, margin and register value, the interpreter's
 * decision (the `taken` it hands to the delivery helper) against the compiled
 * condition's value. Mismatches are counted per list; the fixture fails on any.
 * At margin 40 every listed case must change at least one raw decision (so a
 * list that does not reach the interpreter shows), at margin 0 (4:3) none may.
 * On a runtime with the counter, ws_branch_sites' hits and changed counts must
 * equal the decisions made at the site and those that differ from raw.
 *
 * The production interpreter and widescreen code (gpu.c) are linked unchanged;
 * only the delivery helper is a recorder, so the delay slot never runs.
 * gte_nclip_precise_bltz is a fixture function: gte.cpp is not linked, and both
 * sides call the same one. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "gpu.h"

int ws_branch_sites_interpret(CPUState *, uint32_t, uint32_t);
void ws_branch_sites_enable_source(void);
int ws_case_count(void);
const char *ws_case_name(int);
unsigned ws_case_lists(int);
uint32_t ws_case_word(int);
const char *ws_case_cond(int);
const char *ws_case_tag(int);
int ws_case_compiled(int, const CPUState *);
int ws_branch_configure(unsigned, uint32_t);
int ws_branch_stats(int, uint64_t *, uint64_t *);
extern int g_psx_slice_irq_handoff, g_psx_slice_slot_take;

enum { MARGIN = 40, NCLIP_EXACT_EDGE = -20, KINDS = 4 };
enum { SITE_PC = 0x80002000u };
static const char *const kind_names[KINDS] = {
    "bltz_sites", "branch_keep_sites", "nclip_keep_sites", "nclip_exact_sites"
};
static const int32_t values[] = {-1000, -41, -40, -30, -5, -1, 0, 5};

/* The exact NCLIP test of gte.cpp, replaced by a fixed rule that differs from
 * the raw sign and from psx_ws_cull_bltz, so the fixture sees which one a path
 * calls. */
int gte_nclip_precise_bltz(int32_t native_mac0) {
    return native_mac0 < NCLIP_EXACT_EDGE;
}

static uint8_t ram[0x200000];
static unsigned calls;
static uint32_t eligible_pc;
static struct Delivery {
    uint32_t slot, target, instruction;
    int taken;
} delivery;

/* The common delivery seam, observed. */
uint64_t g_irq_deliver_count;
uint32_t i_stat = 1, i_mask = 1;
int g_psx_irq_delivering;
uint32_t g_dirty_ram_code_gen; /* memory.c is not linked */
int psx_check_interrupts_delay_slot(CPUState *cpu, uint32_t slot,
                                    uint32_t target, int taken, uint32_t insn) {
    (void)cpu;
    ++calls;
    delivery = (struct Delivery){slot, target, insn, taken};
    ++g_irq_deliver_count;
    return 1;
}
int psx_get_in_exception(void) { return 0; }
int psx_interrupt_cooldown_active(void) { return 0; }
int psx_irq_opcode_eligible(uint32_t pc) { return pc == eligible_pc; }
uint8_t *memory_get_ram_ptr(void) { return ram; }
static void put(uint32_t pc, uint32_t word) {
    uint32_t p = pc & 0x1FFFFFFFu;
    for (unsigned i = 0; i < 4; ++i) ram[p + i] = (uint8_t)(word >> (8 * i));
}
uint64_t psx_cycle_count, psx_next_service_cycle = UINT64_MAX;
int psx_in_device_service, g_event_step_conservative;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit = 1000;
uint32_t *g_psx_cyc_local_acc;
int g_psx_cyc_bb_defer;
uint64_t psx_get_cycle_count(void) { return psx_cycle_count + g_psx_cyc_batch; }

static CPUState state_at(int32_t rs, int32_t rt) {
    CPUState cpu = {0};
    cpu.pc = SITE_PC;
    cpu.cop0[12] = 0x401;
    cpu.gpr[8] = (uint32_t)rs;
    cpu.gpr[9] = (uint32_t)rt;
    cpu.ld_which_t = 32;
    cpu.read_fudge = 32;
    return cpu;
}

/* The raw MIPS condition of the branch word, to count changed decisions. */
static int raw_taken(uint32_t word, int32_t rs, int32_t rt) {
    switch (word >> 26) {
    case 0x01: return ((word >> 16) & 1u) ? rs >= 0 : rs < 0;
    case 0x04: return rs == rt;
    case 0x05: return rs != rt;
    case 0x06: return rs <= 0;
    case 0x07: return rs > 0;
    }
    assert(0);
    return 0;
}

static int tag_kind(const char *tag) {
    if (!strcmp(tag, "ws cull (left edge)")) return 0;
    if (!strcmp(tag, "ws branch keep")) return 1;
    if (!strcmp(tag, "ws nclip keep")) return 2;
    if (!strcmp(tag, "ws exact nclip")) return 3;
    assert(tag[0] == '\0');
    return -1;
}

static int interpreted(uint32_t word, int32_t rs, int32_t rt) {
    CPUState cpu = state_at(rs, rt);
    int link = (word >> 26) == 1u && ((word >> 16) & 0x10u);
    calls = 0;
    eligible_pc = SITE_PC + 4u;
    memset(&delivery, 0, sizeof delivery);
    assert(ws_branch_sites_interpret(&cpu, SITE_PC, word) == 1);
    assert(calls == 1);
    assert(delivery.slot == SITE_PC + 4u && delivery.instruction == 0u);
    assert(delivery.target == (delivery.taken ? SITE_PC + 4u + 2u * 4u : SITE_PC + 8u));
    assert(cpu.gpr[8] == (uint32_t)rs && cpu.gpr[9] == (uint32_t)rt);
    assert(cpu.gpr[31] == (link ? SITE_PC + 8u : 0u));
    return delivery.taken;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* keep the last line on an assert */
    ws_branch_sites_enable_source();
    g_psx_slice_irq_handoff = g_psx_slice_slot_take = 1;
    gpu_ws_set_auto_hooks(0, 0);
    put(SITE_PC + 4u, 0u); /* nop in the delay slot */

    unsigned mismatches[KINDS + 1] = {0}, decisions[KINDS + 1] = {0};
    unsigned total_mismatches = 0;
    int counters = -1;
    const int n = ws_case_count();
    for (int i = 0; i < n; ++i) {
        const uint32_t word = ws_case_word(i);
        const int kind = tag_kind(ws_case_tag(i));
        const int slot = kind < 0 ? KINDS : kind;
        put(SITE_PC, word);
        static const int margins[] = {MARGIN, 0};
        for (unsigned m = 0; m < 2; ++m) {
            counters = ws_branch_configure(ws_case_lists(i), SITE_PC);
            gpu_ws_set_margin_override(margins[m]);
            unsigned evaluations = 0, flips = 0, bad = 0;
            for (unsigned v = 0; v < sizeof values / sizeof values[0]; ++v) {
                const int32_t rts[2] = {values[v], 7};
                for (unsigned r = 0; r < 2; ++r) {
                    const int32_t rs = values[v], rt = rts[r];
                    CPUState cpu = state_at(rs, rt);
                    const int compiled = ws_case_compiled(i, &cpu);
                    const int interp = interpreted(word, rs, rt);
                    const int raw = raw_taken(word, rs, rt);
                    ++evaluations;
                    if (compiled != raw) ++flips;
                    if (interp != compiled) {
                        ++bad;
                        printf("  MISMATCH %s margin=%d rs=%d rt=%d raw=%d interp=%d compiled=%d\n",
                               ws_case_name(i), margins[m], (int)rs, (int)rt,
                               raw, interp, compiled);
                    }
                }
            }
            uint64_t hits[KINDS] = {0}, changed[KINDS] = {0};
            for (int k = 0; k < KINDS; ++k) ws_branch_stats(k, &hits[k], &changed[k]);
            printf("case=%s lists=0x%X word=%08X margin=%d decided_by=%s evaluations=%u "
                   "changed_vs_raw=%u mismatches=%u counter_hits=%llu counter_changed=%llu | %s%s%s%s\n",
                   ws_case_name(i), ws_case_lists(i), word, margins[m],
                   kind < 0 ? "raw" : kind_names[kind], evaluations, flips, bad,
                   (unsigned long long)(kind < 0 ? 0 : hits[kind]),
                   (unsigned long long)(kind < 0 ? 0 : changed[kind]),
                   ws_case_cond(i), ws_case_tag(i)[0] ? " /* " : "",
                   ws_case_tag(i), ws_case_tag(i)[0] ? " */" : "");
            mismatches[slot] += bad;
            decisions[slot] += evaluations;
            total_mismatches += bad;
            /* A listed case must matter when widened and be identity at 4:3. */
            if (kind >= 0 && margins[m] > 0) assert(flips > 0);
            if (margins[m] == 0) assert(flips == 0);
            if (counters == 1) {
                for (int k = 0; k < KINDS; ++k) {
                    assert(hits[k] == (k == kind ? evaluations : 0u));
                    assert(changed[k] == (k == kind ? flips : 0u));
                }
            }
        }
    }
    for (int k = 0; k <= KINDS; ++k)
        printf("LIST %s: decisions=%u interp_vs_compiled_mismatches=%u\n",
               k < KINDS ? kind_names[k] : "(raw)", decisions[k], mismatches[k]);
    printf("lists passed to the runtime: %s\n", counters == 1 ? "yes" : "no (no setter)");
    if (total_mismatches) {
        printf("FAIL: %u interpreted decisions differ from the generated code\n",
               total_mismatches);
        return 1;
    }
    printf("PASS: %d cases x 2 margins x %u values; the interpreter decides every listed "
           "site as the generated code does\n",
           n, (unsigned)(2 * sizeof values / sizeof values[0]));
    return 0;
}
