#ifndef PSX_CYCLE_FREEZE_H
#define PSX_CYCLE_FREEZE_H

/* Render-pass time freeze (render_pass.c, docs/RENDER_PASSES.md).
 *
 * Runtime-only: generated and overlay code never sees this header. It is kept
 * out of psx_cycles.h on purpose, because psx_cycles.h is part of the
 * overlay codegen hash (runtime/codegen_hash_sources.cmake) and any change to
 * it invalidates every title's overlay cache and savestates. The freeze is
 * enforced in psx_cycles.c's out-of-line service paths, which generated code
 * already reaches through psx_cycles.h.
 *
 * While g_psx_render_pass_active is set, guest cycles are still counted (GTE
 * and mult/div deadlines keep working) but devices are never serviced, no
 * VBlank or device event fires and no interrupt is delivered; end() puts
 * every clock value back exactly, so the pass consumed no guest time. A pass
 * that runs longer than `watchdog_cycles` calls `overrun` (which must not
 * return into the guest; render_pass.c longjmps out of the pass). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_psx_render_pass_active;
typedef struct PsxCycleFreeze {
    uint64_t cycle_count;
    uint64_t next_service;
    uint64_t fast_limit;
    uint32_t batch;
    uint32_t batch_limit;
    uint32_t *local_acc;
    uint32_t local_acc_value;
    int      in_device_service;
} PsxCycleFreeze;
int  psx_cycle_freeze_begin(PsxCycleFreeze *save, uint64_t watchdog_cycles,
                            void (*overrun)(void));
void psx_cycle_freeze_end(const PsxCycleFreeze *save);

#ifdef __cplusplus
}
#endif

#endif
