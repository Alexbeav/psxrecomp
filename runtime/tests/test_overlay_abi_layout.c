/* Composed overlay ABI v36/cg18: preserve the complete upstream v25
 * prefix and pin the complete modern tail. Original fixture identity:
 * RetroPortingToolKit/psxrecomp 3505f2a01, blob676510362e8d.
 * Slot3 was renamed from advance_cycles to cpu_charge with identical type.
 * This is ABI/layout proof, not cross-product snapshot or runtime proof. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define OverlayCallbacks UpstreamV25Callbacks
#define overlay_init upstream_v25_overlay_init
#define advance_cycles cpu_charge
#include "fixtures/overlay_api_upstream_v25.h"
#undef advance_cycles
#undef OverlayCallbacks
#undef overlay_init
enum { UPSTREAM_ABI_VERSION = PSX_OVERLAY_ABI_VERSION,
       UPSTREAM_CODEGEN_VER = PSX_OVERLAY_CODEGEN_VER };
#undef OVERLAY_API_H
#undef PSX_OVERLAY_ABI_VERSION
#undef PSX_OVERLAY_CODEGEN_VER
#undef PSX_OVERLAY_CANDIDATE_CAP
#undef PSX_OVERLAY_FLAVOR_WIDESCREEN
#undef PSX_OVERLAY_FLAVOR_PGXP
#undef PSX_OVERLAY_ABI_TAG
#include "overlay_api.h"
#define UPSTREAM_V25_SLOTS(X) \
    X( 0, dispatch_call)            X( 1, check_interrupts)        \
    X( 2, check_interrupts_at)      X( 3, cpu_charge)          \
    X( 4, gte_execute)              X( 5, psx_syscall)             \
    X( 6, psx_unknown_dispatch)     X( 7, log_call_entry)          \
    X( 8, psx_restore_state_escape) X( 9, call_bail_flag)          \
    X(10, bail_first)               X(11, bail_resolved)           \
    X(12, ws_backdrop_x)            X(13, ws_x_margin)             \
    X(14, ws_sprite_tag)            X(15, ws_backdrop_value)       \
    X(16, psx_native_bad_entry)     X(17, cyc_load_word)           \
    X(18, cyc_load_half)            X(19, cyc_load_byte)           \
    X(20, cyc_lwc2_read)            X(21, icache_fetch)            \
    X(22, muldiv_set)               X(23, muldiv_stall)            \
    X(24, mult_latency_s)           X(25, mult_latency_u)          \
    X(26, gte_stall)                X(27, gte_read)                \
    X(28, slice_block)              X(29, gte_read_data)           \
    X(30, gte_read_ctrl)            X(31, gte_write_data)          \
    X(32, gte_write_ctrl)           X(33, rfe_mark_escape)         \
    X(34, ws_player_x_bound)        X(35, gte_precision_store_word)\
    X(36, ws_depth_bound)           X(37, ws_plane_nx)             \
    X(38, ws_xclip_bound)           X(39, ws_cull_keep_result)     \
    X(40, ws_aspect_cone_result)    X(41, ws_angle_widen)          \
    X(42, game_text_native_ok)      X(43, pgxp)                    \
    X(44, ws_screen_x_bound)        X(45, mod_function_entry)      \
    X(46, last_store_pc)

#define COMPOSED_V36_SLOTS(X) \
    X(47, ws_masked_reject) \
    X(48, ws_nclip_branch) \
    X(49, ws_cull_scale) \
    X(50, nclip_exact_sign) \
    X(51, mod_instruction) \
    X(52, mod_memory_address) \
    X(53, mod_immediate_select) \
    X(54, mod_try_function_replacement) \
    X(55, cpu_step_boundary_enabled) \
    X(56, cpu_step_boundary)
#define COUNT_SLOT(index, member) +1
enum { UPSTREAM_SLOT_COUNT = 0 UPSTREAM_V25_SLOTS(COUNT_SLOT),
       COMPOSED_SLOT_COUNT = 0 COMPOSED_V36_SLOTS(COUNT_SLOT) };
#define PIN_SLOT(index, member) \
 _Static_assert(offsetof(OverlayCallbacks, member) == (size_t)(index)*sizeof(void*), "slot moved: " #member); \
 _Static_assert(sizeof(((OverlayCallbacks*)0)->member) == sizeof(void*), "slot width: " #member);
#define PIN_PREFIX(index, member) \
 PIN_SLOT(index, member) \
 _Static_assert(offsetof(UpstreamV25Callbacks, member) == offsetof(OverlayCallbacks, member), "upstream offset: " #member); \
 _Static_assert(__builtin_types_compatible_p(__typeof__(((UpstreamV25Callbacks*)0)->member), __typeof__(((OverlayCallbacks*)0)->member)), "upstream type: " #member);
UPSTREAM_V25_SLOTS(PIN_PREFIX)
COMPOSED_V36_SLOTS(PIN_SLOT)
_Static_assert(UPSTREAM_SLOT_COUNT == 47 && COMPOSED_SLOT_COUNT == 10, "complete slot inventory");
_Static_assert(sizeof(UpstreamV25Callbacks) == 47*sizeof(void*), "fixture has an unlisted field");
#ifdef PSX_SOURCE_BUS_BOUNDARY
PIN_SLOT(57, source_bus_boundary)
PIN_SLOT(58, psx_game_break)
PIN_SLOT(59, source_latch_bltz)
_Static_assert(sizeof(OverlayCallbacks) == 60*sizeof(void*), "unlisted source-flavor field");
_Static_assert((PSX_OVERLAY_EFFECTIVE_FLAVOR & 4) != 0, "source callback must change the ABI tag");
#else
PIN_SLOT(57, psx_game_break)
PIN_SLOT(58, source_latch_bltz)
_Static_assert(sizeof(OverlayCallbacks) == 59*sizeof(void*), "unlisted base field");
#endif
_Static_assert(__builtin_types_compatible_p(__typeof__(((OverlayCallbacks*)0)->cpu_step_boundary_enabled), int (*)(int)), "observer enabled type");
_Static_assert(__builtin_types_compatible_p(__typeof__(((OverlayCallbacks*)0)->cpu_step_boundary), void (*)(CPUState*, uint32_t)), "observer step type");
_Static_assert(UPSTREAM_ABI_VERSION == 25 && UPSTREAM_CODEGEN_VER == 13, "original fixture identity");
_Static_assert(PSX_OVERLAY_ABI_VERSION == 37 && PSX_OVERLAY_CODEGEN_VER == 19, "composed product identity");
_Static_assert(PSX_OVERLAY_ABI_VERSION != UPSTREAM_ABI_VERSION && PSX_OVERLAY_CODEGEN_VER != UPSTREAM_CODEGEN_VER, "stale caches must not match");
/* CPUState is part of the same contract: shards read and write it directly.
 * Upstream's layout ends at ld_absorb. The fork's load-value pipeline is three
 * uint32_t after it and nothing else. */
_Static_assert(offsetof(CPUState, load_value_rt) ==
                   offsetof(CPUState, ld_absorb) + sizeof(uint32_t),
               "fork CPUState tail must start right after upstream's fields");
_Static_assert(offsetof(CPUState, load_value) ==
                   offsetof(CPUState, load_value_rt) + sizeof(uint32_t),
               "load_value moved");
_Static_assert(offsetof(CPUState, load_value_age) ==
                   offsetof(CPUState, load_value) + sizeof(uint32_t),
               "load_value_age moved");
_Static_assert(sizeof(CPUState) ==
                   ((offsetof(CPUState, load_value_age) + sizeof(uint32_t) +
                     _Alignof(CPUState) - 1) / _Alignof(CPUState)) *
                       _Alignof(CPUState),
               "CPUState has a field after the fork load-value tail");

int main(void) { puts("PASS: composed v36 ABI slots/types and CPUState tail"); return 0; }
