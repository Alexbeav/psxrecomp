/* Admission matrix for savestate_admission_decide(). Each row is one
 * invariant from the consolidation report §1.10 table; the test is the
 * behavioural specification the downstream re-land is gated on. */
#include "savestate_admission.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static SavestateAdmissionInputs base_safe(void)
{
    SavestateAdmissionInputs in;
    memset(&in, 0, sizeof in);
    in.hle_enabled = 0;
    in.boundary_active = 0;
    in.capture_boundary_ok = 0;
    in.context_ok = 1;
    in.snapshot_safe = 1;
    in.resume_pc_ok = 1;
    in.resume_pc = 0x80012340u;
    in.pc = 0x80012340u;
    in.cpu_pc = 0x80012340u;
    return in;
}

static void expect(const char* name, const SavestateAdmissionInputs* in,
                   SavestateAdmission want, const char* want_reason)
{
    SavestateAdmission got = savestate_admission_decide(in);
    const char* reason = savestate_admission_reason(in);
    if (got != want || (want_reason && strcmp(reason, want_reason) != 0)) {
        printf("FAIL %-40s got=%d reason=%s want=%d/%s\n", name, (int)got,
               reason, (int)want, want_reason ? want_reason : "*");
        g_fail++;
    } else {
        printf("ok   %-40s %d %s\n", name, (int)got, reason);
    }
}

int main(void)
{
    SavestateAdmissionInputs in;

    in = base_safe();
    expect("plain safe context admits", &in, SAVESTATE_ADMIT, "ok");

    /* --- downstream: HLE scheduler boundary --- */
    in = base_safe(); in.hle_enabled = 1; in.boundary_active = 0; in.capture_boundary_ok = 1;
    expect("HLE dispatch + capture boundary -> unwind", &in,
           SAVESTATE_UNWIND_TO_SCHEDULER, "waiting_flat_scheduler");

    in = base_safe(); in.hle_enabled = 1; in.boundary_active = 0; in.capture_boundary_ok = 0;
    expect("HLE dispatch, no boundary -> defer", &in, SAVESTATE_DEFER,
           "waiting_flat_scheduler");

    in = base_safe(); in.hle_enabled = 1; in.boundary_active = 1;
    expect("HLE at flat scheduler boundary admits", &in, SAVESTATE_ADMIT, "ok");

    /* unwind must win over every other rejection: it is the path that makes
     * the context serializable at all */
    in = base_safe(); in.hle_enabled = 1; in.capture_boundary_ok = 1;
    in.context_ok = 0; in.snapshot_safe = 0;
    expect("unwind precedes context/pump rejections", &in,
           SAVESTATE_UNWIND_TO_SCHEDULER, NULL);

    /* --- downstream: register context --- */
    in = base_safe(); in.context_ok = 0;
    expect("invalid flat/snapshot context defers", &in, SAVESTATE_DEFER,
           "unsafe_guest_context");

    /* --- upstream: dirty-RAM pump site --- */
    in = base_safe(); in.snapshot_safe = 0;
    expect("dirty pump site defers", &in, SAVESTATE_DEFER, "dirty_pump_site");

    /* --- upstream: hint == 0 handling --- */
    in = base_safe(); in.resume_pc = 0u; in.pc = 0x80020000u; in.cpu_pc = 0x80020000u;
    expect("hint 0, pc == cpu->pc, safe -> admit", &in, SAVESTATE_ADMIT, "ok");

    in = base_safe(); in.resume_pc = 0u; in.pc = 0x00020000u; in.cpu_pc = 0x80020000u;
    expect("hint 0, pc == cpu->pc modulo segment", &in, SAVESTATE_ADMIT, "ok");

    in = base_safe(); in.resume_pc = 0u; in.pc = 0x80020000u; in.cpu_pc = 0x80020000u;
    in.snapshot_safe = 0;
    expect("hint 0 at pump site defers even if pc matches", &in, SAVESTATE_DEFER,
           "dirty_pump_site");

    in = base_safe(); in.resume_pc = 0u; in.pc = 0x80020000u; in.cpu_pc = 0x80020100u;
    expect("hint 0, stale fallback pc != cpu->pc defers", &in, SAVESTATE_DEFER,
           "no_hint_stale_fallback");

    in = base_safe(); in.resume_pc = 0u; in.pc = 0x80020000u; in.cpu_pc = 0u;
    expect("hint 0 with no CPUState defers", &in, SAVESTATE_DEFER,
           "no_hint_stale_fallback");

    /* --- both: resolved PC not dispatchable --- */
    in = base_safe(); in.resume_pc_ok = 0;
    expect("non-dispatchable resume pc defers", &in, SAVESTATE_DEFER,
           "resume_pc_not_dispatchable");

    /* hint present but pc differs from cpu->pc is fine when everything else is ok
     * (the resolver may legitimately substitute a block leader) */
    in = base_safe(); in.pc = 0x80012344u; in.cpu_pc = 0x80012340u;
    expect("hinted pc may differ from cpu->pc", &in, SAVESTATE_ADMIT, "ok");

    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("savestate admission matrix: all rows pass\n");
    return 0;
}
