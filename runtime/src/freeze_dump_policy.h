#ifndef PSXRECOMP_FREEZE_DUMP_POLICY_H
#define PSXRECOMP_FREEZE_DUMP_POLICY_H

#include <stddef.h>
#include <stdint.h>

/* Full freeze dumps contain several large trace rings. Keep automatic
 * watchdog output bounded while leaving deliberate fatal dumps independent. */
#define FREEZE_DUMP_ORDINARY_SLOTS 1u
#define FREEZE_DUMP_HARD_SLOTS 1u
/* A slot that was spent is filled again after a long healthy interval, this
 * many times. Without it the slow frames and stalls of a startup spend both
 * slots, and a wedge an hour later leaves no dump. */
#define FREEZE_DUMP_SLOT_REFILLS 1u
/* Healthy samples in a row before a spent slot is filled again: 60 s at the
 * heartbeat's 100 ms. */
#define FREEZE_DUMP_REFILL_HEALTHY_TICKS 600u
/* The most automatic dumps one process can write. */
#define FREEZE_DUMP_AUTO_LIMIT \
    ((FREEZE_DUMP_ORDINARY_SLOTS + FREEZE_DUMP_HARD_SLOTS) * \
     (1u + FREEZE_DUMP_SLOT_REFILLS))
#define FREEZE_DUMP_MAX_FAILED_ATTEMPTS 3u
/* Policy hysteresis, independent of the detector's sample window. The total
 * time before rearm can therefore be longer than this tick count alone. */
#define FREEZE_DUMP_REARM_HEALTHY_TICKS 20u

typedef struct FreezeDumpPolicy {
    uint32_t automatic_dumps;
    uint32_t failed_dumps;
    uint32_t suppressed_events;
    uint32_t healthy_ticks;
    uint32_t wedge_kind;
    uint32_t pending_kind;
    uint32_t ordinary_failed_attempts;
    uint32_t hard_failed_attempts;
    int ordinary_slot_consumed;
    int hard_slot_consumed;
    int in_wedge;
    uint32_t healthy_run_ticks;  /* healthy samples since the last wedge sample */
    uint32_t ordinary_refills;
    uint32_t hard_refills;
} FreezeDumpPolicy;

/* Observe one watchdog sample. Returns nonzero when the caller must write an
 * automatic full dump. A new event requires a sustained healthy interval.
 * A spent slot is filled again after FREEZE_DUMP_REFILL_HEALTHY_TICKS healthy
 * samples in a row, FREEZE_DUMP_SLOT_REFILLS times.
 * Fatal state bypasses this automatic policy and does not change its budget. */
int freeze_dump_policy_observe(FreezeDumpPolicy *policy, uint32_t wedge_kind,
                               int fatal_active);

/* Record whether the authorized full dump reached disk. A slot permits a
 * bounded number of failed attempts before the policy consumes that slot. */
void freeze_dump_policy_record_result(FreezeDumpPolicy *policy, int written);

/* Format a unique dump path. The sequence prevents same-second truncation. */
int freeze_dump_format_path(char *out, size_t cap, const char *backend,
                            long long wall, uint32_t sequence);

#endif /* PSXRECOMP_FREEZE_DUMP_POLICY_H */
