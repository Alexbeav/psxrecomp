/* func_override.c — replace a guest function with hand-written C.
 * See runtime/include/func_override.h.
 *
 * CLAUDE.md rule 3 (no printf, no logs): this module prints nothing. State
 * is exposed through the accessors and surfaced by the `func_override` TCP
 * command. Registration errors are returned as codes for the caller to
 * report.
 */

#include "func_override.h"

#include <string.h>

#include "cpu_state.h"

extern uint32_t psx_read_word(uint32_t addr);

/* Set by us, read at the top of every psx_dispatch_impl (emitted by
 * recompiler/src/full_function_emitter.cpp) and at the interpreter's
 * JAL/JALR call-resolution tiers (dirty_ram_interp.c). NULL until install,
 * so a build with no overrides dispatches exactly as before. */
int (*g_psx_func_override_hook)(CPUState *cpu, uint32_t phys) = NULL;

typedef struct {
    char           id[FO_MAX_ID];
    uint32_t       phys;         /* normalised: KSEG bits masked off */
    FuncOverrideFn fn;
    uint64_t       calls;
    uint64_t       guard_misses;
    uint32_t       guard[FO_MAX_GUARD_WORDS];
    int            n_guard;      /* 0 = unguarded */
} Entry;

static Entry s_entries[FO_MAX_OVERRIDES];
static int   s_count = 0;

/* call_original support: the address whose hook is currently executing
 * (for redial) and a one-shot bypass armed by func_override_call_original.
 * Dispatch is single-threaded; a nested override (an override whose
 * guest_call reaches another overridden function) saves/restores around
 * the inner consult, so plain statics are correct here. */
static uint32_t s_active_phys = 0;
static uint32_t s_bypass_phys = 0;

static uint32_t normalise(uint32_t addr) { return addr & 0x1FFFFFFFu; }

static Entry *find(uint32_t phys)
{
    for (int i = 0; i < s_count; i++)
        if (s_entries[i].phys == phys)
            return &s_entries[i];
    return NULL;
}

static int add_common(const char *id, uint32_t addr, FuncOverrideFn fn,
                      const uint32_t *guard, int n_guard)
{
    if (!fn)                         return FO_ERR_ARGS;
    if (n_guard < 0 || n_guard > FO_MAX_GUARD_WORDS) return FO_ERR_ARGS;
    if (n_guard > 0 && !guard)       return FO_ERR_ARGS;
    if (s_count >= FO_MAX_OVERRIDES) return FO_ERR_FULL;

    const uint32_t phys = normalise(addr);
    /* Two overrides on one address is always a mistake, and a silent
     * last-wins would be untraceable. Refuse it. */
    if (find(phys)) return FO_ERR_DUPLICATE;

    Entry *e = &s_entries[s_count];
    memset(e, 0, sizeof(*e));
    if (id) {
        strncpy(e->id, id, FO_MAX_ID - 1);
        e->id[FO_MAX_ID - 1] = '\0';
    }
    e->phys = phys;
    e->fn   = fn;
    for (int i = 0; i < n_guard; i++) e->guard[i] = guard[i];
    e->n_guard = n_guard;
    s_count++;
    return FO_OK;
}

int func_override_add(const char *id, uint32_t addr, FuncOverrideFn fn)
{
    return add_common(id, addr, fn, NULL, 0);
}

int func_override_add_guarded(const char *id, uint32_t addr, FuncOverrideFn fn,
                              const uint32_t *expected_words, int n_words)
{
    if (n_words < 1) return FO_ERR_ARGS;
    return add_common(id, addr, fn, expected_words, n_words);
}

/* The hook. Runs on EVERY dispatch, so the common path — nothing registered
 * for this address — must stay cheap. A linear scan over a handful of
 * entries beats a hash for realistic counts and keeps the code honest;
 * revisit if a game ever registers hundreds. */
static int hook(CPUState *cpu, uint32_t phys)
{
    for (int i = 0; i < s_count; i++) {
        Entry *e = &s_entries[i];
        if (e->phys != phys) continue;
        /* One-shot bypass: func_override_call_original re-dispatched this
         * address; let the original backend take it exactly once. */
        if (s_bypass_phys == phys) {
            s_bypass_phys = 0;
            return 0;
        }
        /* Residency guard: wrong bytes at the address (overlay swapped,
         * page reused) means the function this override targets is not
         * resident — decline, never corrupt. */
        if (e->n_guard) {
            const uint32_t base = 0x80000000u | phys;
            int miss = 0;
            for (int w = 0; w < e->n_guard; w++)
                if (psx_read_word(base + (uint32_t)(w * 4)) != e->guard[w]) {
                    miss = 1;
                    break;
                }
            if (miss) {
                e->guard_misses++;
                return 0;
            }
        }
        /* Count the CONSULT, not just the handled case: an override that
         * declines still proves the address was reached through a hooked
         * path, which is exactly what a diagnostic probe needs (calls == 0
         * means "this call never crosses a hook", the bypass class of bug
         * that hid the interp local-chain gap). */
        e->calls++;
        {
            const uint32_t saved_active = s_active_phys;
            s_active_phys = phys;
            const int handled = e->fn(cpu) ? 1 : 0;
            s_active_phys = saved_active;
            return handled;
        }
    }
    return 0;
}

void func_override_guest_call(CPUState *cpu, uint32_t target, uint32_t site_ra)
{
    /* Spoof $ra to the original call site so callees, fntrace and crash
     * forensics see authentic values, then restore. psx_dispatch_call runs
     * the callee to completion (pc == site_ra with the caller's $sp). */
    extern void psx_dispatch_call(CPUState *cpu, uint32_t addr,
                                  uint32_t return_addr);
    const uint32_t saved_ra = cpu->gpr[31];
    cpu->gpr[31] = site_ra;
    psx_dispatch_call(cpu, target, site_ra);
    cpu->gpr[31] = saved_ra;
}

void func_override_call_original(CPUState *cpu)
{
    if (!s_active_phys) return;   /* not inside an override — nothing to do */
    const uint32_t phys  = s_active_phys;
    const uint32_t saved = s_bypass_phys;
    s_bypass_phys = phys;
    /* KSEG0 form: game text dispatches with the 0x8000_0000 view; the hook
     * itself keys on the normalised address either way. Return to the
     * override's caller frame: the original must observe the same $ra the
     * override was entered with. */
    func_override_guest_call(cpu, 0x80000000u | phys, cpu->gpr[31]);
    s_bypass_phys = saved;
}

void func_override_install(void)
{
    /* Leave the hook NULL when empty: dispatch then matches a build without
     * the tier exactly, which keeps "overrides cost nothing" literally
     * true. */
    g_psx_func_override_hook = (s_count > 0) ? hook : NULL;
}

int func_override_count(void) { return s_count; }

int func_override_get(int index, char *id_out, uint32_t *addr_out,
                      uint64_t *calls_out)
{
    return func_override_get_ex(index, id_out, addr_out, calls_out, NULL,
                                NULL);
}

int func_override_get_ex(int index, char *id_out, uint32_t *addr_out,
                         uint64_t *calls_out, uint64_t *guard_misses_out,
                         int *guarded_out)
{
    if (index < 0 || index >= s_count) return 0;
    const Entry *e = &s_entries[index];
    if (id_out)           { memcpy(id_out, e->id, FO_MAX_ID); }
    if (addr_out)         *addr_out         = e->phys;
    if (calls_out)        *calls_out        = e->calls;
    if (guard_misses_out) *guard_misses_out = e->guard_misses;
    if (guarded_out)      *guarded_out      = e->n_guard;
    return 1;
}
