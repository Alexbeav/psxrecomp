/* Overlay ABI layout pin (ABI v27, PS1B-331 / PS1B-412).
 *
 * The fork and upstream both called their ABI "25" while the two structs
 * differed from slot 45 on, so a shard that passed the version gate on the
 * other side read the wrong slot. v26 is upstream's v25 layout with the fork's
 * slots appended. This test keeps it that way:
 *
 *   - every slot of upstream's v25 OverlayCallbacks has the same offset and the
 *     same type in this tree's struct. Upstream's side is its own header,
 *     fixtures/overlay_api_upstream_v25.h, a byte-for-byte copy of
 *     runtime/include/overlay_api.h at RetroPortingToolKit/psxrecomp 3505f2a01
 *     (git blob 676510362e8d; test_overlay_abi_v26_guards.py checks the copy);
 *   - every slot of the v26 struct has a pinned index, and the list covers the
 *     whole struct, so a slot cannot be added, removed or moved without
 *     editing this file;
 *   - the fork's CPUState additions are a tail after the last field upstream's
 *     layout has.
 *
 * When upstream changes its layout: replace the fixture with upstream's new
 * header, take its layout verbatim in overlay_api.h, move the fork slots after
 * it, and set PSX_OVERLAY_ABI_VERSION to upstream's number plus one. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Upstream's header first, under other names, so both structs exist here. */
#define OverlayCallbacks UpstreamV25Callbacks
#define overlay_init     upstream_v25_overlay_init
#include "fixtures/overlay_api_upstream_v25.h"
#undef OverlayCallbacks
#undef overlay_init
enum {
    UPSTREAM_ABI_VERSION = PSX_OVERLAY_ABI_VERSION,
    UPSTREAM_CODEGEN_VER = PSX_OVERLAY_CODEGEN_VER
};
#undef OVERLAY_API_H
#undef PSX_OVERLAY_ABI_VERSION
#undef PSX_OVERLAY_CODEGEN_VER
#undef PSX_OVERLAY_CANDIDATE_CAP
#undef PSX_OVERLAY_FLAVOR_WIDESCREEN
#undef PSX_OVERLAY_FLAVOR_PGXP
#undef PSX_OVERLAY_ABI_TAG

#include "overlay_api.h"

/* X(slot index, member). One row per slot, in struct order. */
#define UPSTREAM_V25_SLOTS(X) \
    X( 0, dispatch_call)            X( 1, check_interrupts)        \
    X( 2, check_interrupts_at)      X( 3, advance_cycles)          \
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

/* Fork v26 slots stay fixed; v27 appends compiled-game BREAK at slot 49. */
#define FORK_V26_SLOTS(X) \
    X(47, cpu_step_boundary_enabled) X(48, cpu_step_boundary) \
    X(49, psx_game_break)

#define COUNT_SLOT(index, member) + 1
enum {
    UPSTREAM_SLOT_COUNT = 0 UPSTREAM_V25_SLOTS(COUNT_SLOT),
    FORK_SLOT_COUNT     = 0 FORK_V26_SLOTS(COUNT_SLOT)
};

/* Every member is one pointer, so slot N sits at N * sizeof(void *). */
#define PIN_FORK_SLOT(index, member)                                         \
    _Static_assert(offsetof(OverlayCallbacks, member) ==                     \
                       (size_t)(index) * sizeof(void *),                     \
                   "v26 slot moved: " #member);                              \
    _Static_assert(sizeof(((OverlayCallbacks *)0)->member) == sizeof(void *),\
                   "v26 slot is not one pointer: " #member);

#if defined(__GNUC__) || defined(__clang__)
#define SAME_SLOT_TYPE(member)                                               \
    _Static_assert(__builtin_types_compatible_p(                             \
                       __typeof__(((UpstreamV25Callbacks *)0)->member),      \
                       __typeof__(((OverlayCallbacks *)0)->member)),         \
                   "slot type differs from upstream v25: " #member);
#else
#define SAME_SLOT_TYPE(member)
#endif

#define PIN_UPSTREAM_SLOT(index, member)                                     \
    PIN_FORK_SLOT(index, member)                                             \
    _Static_assert(offsetof(UpstreamV25Callbacks, member) ==                 \
                       offsetof(OverlayCallbacks, member),                   \
                   "slot offset differs from upstream v25: " #member);       \
    SAME_SLOT_TYPE(member)

UPSTREAM_V25_SLOTS(PIN_UPSTREAM_SLOT)
FORK_V26_SLOTS(PIN_FORK_SLOT)

/* The lists cover both structs completely: no unlisted slot on either side. */
_Static_assert(sizeof(UpstreamV25Callbacks) ==
                   UPSTREAM_SLOT_COUNT * sizeof(void *),
               "upstream v25 has a slot this test does not list");
_Static_assert(sizeof(OverlayCallbacks) ==
                   (UPSTREAM_SLOT_COUNT + FORK_SLOT_COUNT) * sizeof(void *),
               "v27 has a slot this test does not list");
_Static_assert(UPSTREAM_SLOT_COUNT == 47 && FORK_SLOT_COUNT == 3,
               "slot counts changed");

/* The numbers. Upstream's header is v25 / codegen 13. The fork is two above
 * upstream's ABI, and its codegen number is not upstream's. */
_Static_assert(UPSTREAM_ABI_VERSION == 25, "fixture is not upstream v25");
_Static_assert(UPSTREAM_CODEGEN_VER == 13, "fixture is not upstream cg13");
_Static_assert(PSX_OVERLAY_ABI_VERSION == 27, "fork ABI version");
_Static_assert(PSX_OVERLAY_ABI_VERSION == UPSTREAM_ABI_VERSION + 2,
               "fork v27 includes BREAK after v26");
_Static_assert(PSX_OVERLAY_CODEGEN_VER == 16, "fork codegen version");
_Static_assert(PSX_OVERLAY_CODEGEN_VER != UPSTREAM_CODEGEN_VER,
               "fork codegen version must not reuse upstream's number");

/* x86-64 offsets of the slots that clashed, spelled out. */
_Static_assert(sizeof(void *) != 8 ||
                   (offsetof(OverlayCallbacks, mod_function_entry) == 0x168 &&
                    offsetof(OverlayCallbacks, last_store_pc) == 0x170 &&
                    offsetof(OverlayCallbacks, cpu_step_boundary_enabled) == 0x178 &&
                    offsetof(OverlayCallbacks, cpu_step_boundary) == 0x180 &&
                    offsetof(OverlayCallbacks, psx_game_break) == 0x188 &&
                    sizeof(OverlayCallbacks) == 0x190 &&
                    sizeof(UpstreamV25Callbacks) == 0x178),
               "x86-64 tail offsets");

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

#define PRINT_SLOT(index, member)                                            \
    printf("  slot %2d  +0x%03X  %s\n", index,                               \
           (unsigned)offsetof(OverlayCallbacks, member), #member);

int main(void) {
    /* Everything above is checked at compile time. Print what was examined,
     * so a log shows the layout this build pinned. */
    printf("overlay ABI v%d (tag 0x%X), codegen %d; upstream fixture v%d, "
           "codegen %d\n",
           PSX_OVERLAY_ABI_VERSION, (unsigned)PSX_OVERLAY_ABI_TAG,
           PSX_OVERLAY_CODEGEN_VER, UPSTREAM_ABI_VERSION,
           UPSTREAM_CODEGEN_VER);
    printf("upstream v25 slots (offset and type equal on both sides): %d\n",
           UPSTREAM_SLOT_COUNT);
    UPSTREAM_V25_SLOTS(PRINT_SLOT)
    printf("fork v26 slots: %d\n", FORK_SLOT_COUNT);
    FORK_V26_SLOTS(PRINT_SLOT)
    printf("sizeof OverlayCallbacks: upstream v25 0x%X, v26 0x%X\n",
           (unsigned)sizeof(UpstreamV25Callbacks),
           (unsigned)sizeof(OverlayCallbacks));
    printf("CPUState: upstream fields end at +0x%X, fork load-value tail "
           "+0x%X..+0x%X, sizeof 0x%X\n",
           (unsigned)(offsetof(CPUState, ld_absorb) + sizeof(uint32_t)),
           (unsigned)offsetof(CPUState, load_value_rt),
           (unsigned)(offsetof(CPUState, load_value_age) + sizeof(uint32_t)),
           (unsigned)sizeof(CPUState));
    printf("PASS: overlay ABI layout\n");
    return 0;
}
