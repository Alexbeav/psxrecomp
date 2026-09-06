#include "savestate_admission.h"

int savestate_admission_pc_matches_cpu(uint32_t pc, uint32_t cpu_pc)
{
    return cpu_pc != 0u && (((cpu_pc ^ pc) & 0x1FFFFFFFu) == 0u);
}

static int needs_scheduler_boundary(const SavestateAdmissionInputs* in)
{
    return in->hle_enabled && !in->boundary_active;
}

SavestateAdmission savestate_admission_decide(const SavestateAdmissionInputs* in)
{
    /* Downstream: inside a long HLE dispatch with an explicit block-leader
     * interrupt boundary -> unwind the host continuation and let the flat
     * scheduler serialize at this exact guest PC. */
    if (needs_scheduler_boundary(in) && in->capture_boundary_ok)
        return SAVESTATE_UNWIND_TO_SCHEDULER;

    /* Any of these means CPUState is not (yet) the serializable guest context. */
    if (needs_scheduler_boundary(in))                       return SAVESTATE_DEFER;
    if (!in->context_ok)                                    return SAVESTATE_DEFER;
    if (!in->snapshot_safe)                                 return SAVESTATE_DEFER;
    if (in->resume_pc == 0u &&
        !savestate_admission_pc_matches_cpu(in->pc, in->cpu_pc))
                                                            return SAVESTATE_DEFER;
    if (!in->resume_pc_ok)                                  return SAVESTATE_DEFER;
    return SAVESTATE_ADMIT;
}

const char* savestate_admission_reason(const SavestateAdmissionInputs* in)
{
    if (needs_scheduler_boundary(in)) return "waiting_flat_scheduler";
    if (!in->context_ok)              return "unsafe_guest_context";
    if (!in->snapshot_safe)           return "dirty_pump_site";
    if (in->resume_pc == 0u &&
        !savestate_admission_pc_matches_cpu(in->pc, in->cpu_pc))
                                      return "no_hint_stale_fallback";
    if (!in->resume_pc_ok)            return "resume_pc_not_dispatchable";
    return "ok";
}
