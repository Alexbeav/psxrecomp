/* Save-state admission decision, extracted from savestate_poll() so it can be
 * exercised without the runtime (see tests/test_savestate_admission.c).
 *
 * Two orthogonal admission models meet here:
 *   - upstream (mstan #230/#237): reject dirty-RAM pump-site contexts
 *     (snapshot_safe) and allow a missing hint only when the resolved PC came
 *     straight from cpu->pc (pc_matches_cpu);
 *   - downstream (HLE scheduler): a title inside one long CPS/native dispatch
 *     must be unwound to the flat scheduler before CPUState is serializable
 *     (needs_scheduler_boundary / capture_boundary_ok), and the register
 *     context must be the guest's (context_ok).
 * The decision is pure so the matrix in the test is the specification. */
#ifndef PSX_SAVESTATE_ADMISSION_H
#define PSX_SAVESTATE_ADMISSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SavestateAdmissionInputs {
    int hle_enabled;          /* psx_hle_scheduler_enabled() */
    int boundary_active;      /* psx_scheduler_snapshot_boundary_active() */
    int capture_boundary_ok;  /* savestate_active_capture_boundary_ok(cpu, pc) */
    int context_ok;           /* flat/snapshot context check for (pc, sp) */
    int snapshot_safe;        /* psx_irq_resume_context_snapshot_safe() */
    int resume_pc_ok;         /* savestate_resume_pc_ok(pc) */
    uint32_t resume_pc;       /* hint published by the caller (0 = none) */
    uint32_t pc;              /* resolved resume PC */
    uint32_t cpu_pc;          /* cpu->pc (0 when no CPUState) */
} SavestateAdmissionInputs;

typedef enum SavestateAdmission {
    SAVESTATE_ADMIT = 0,               /* serialize now */
    SAVESTATE_UNWIND_TO_SCHEDULER = 1, /* longjmp to the flat scheduler, retry there */
    SAVESTATE_DEFER = 2                /* not safe yet; bounded retry */
} SavestateAdmission;

/* True when a missing hint is still safe: the resolved PC is cpu->pc itself. */
int savestate_admission_pc_matches_cpu(uint32_t pc, uint32_t cpu_pc);

SavestateAdmission savestate_admission_decide(const SavestateAdmissionInputs* in);

/* Short reason string for diagnostics ("waiting_flat_scheduler", ...). */
const char* savestate_admission_reason(const SavestateAdmissionInputs* in);

#ifdef __cplusplus
}
#endif
#endif
