/* Shard fixture for test_overlay_abi_gate_runtime.py.
 *
 * Built four ways:
 *   default                  against this tree's overlay_api.h (ABI v27);
 *   -DTEST_UPSTREAM_V25      against upstream's v25 header, byte for byte
 *                            (fixtures/overlay_api_upstream_v25.h);
 *   -DTEST_STALE_ABI=25      this tree's header, but exporting the tag a
 *                            pin G2 shard exports.
 *   -DTEST_STALE_ABI=26      the former fork tag without BREAK forwarding.
 *
 * The loader must run the first and must never call into the other two: both
 * say "25", and neither lays the callback table out as this host does. Every
 * entry point writes a trace line, so "never called" is observable. */
#ifdef TEST_UPSTREAM_V25
#include "fixtures/overlay_api_upstream_v25.h"
#else
#include "overlay_api.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

#define TEST_PAIR_ID UINT64_C(0x1020304050607080)
#define TEST_MARKER  0xC001CAFEu
#define TEST_STORE_PC 0x800657FCu

static OverlayCallbacks s_cbs;

static void trace(const char *event) {
    const char *path = getenv("PSX_PAIR_TEST_TRACE");
    if (!path || !*path) return;
    FILE *out = fopen(path, "ab");
    if (!out) return;
    fprintf(out, "%s\n", event);
    fclose(out);
}

TEST_EXPORT int overlay_abi(void) {
#ifdef TEST_STALE_ABI
    return (TEST_STALE_ABI & 0xFFFF) | ((PSX_OVERLAY_FLAVOR & 0xFFFF) << 16);
#else
    return PSX_OVERLAY_ABI_TAG;
#endif
}
TEST_EXPORT uint64_t overlay_pair_id(void) { return TEST_PAIR_ID; }

TEST_EXPORT void overlay_init(const OverlayCallbacks *callbacks) {
    s_cbs = *callbacks;
#if !defined(TEST_UPSTREAM_V25)
    if (!s_cbs.psx_game_break) abort(); /* required v27 slot is wired */
#endif
    trace("init");
}

TEST_EXPORT void overlay_flush_cycles(void) {}

TEST_EXPORT void func_80010000(CPUState *cpu) {
    trace("call");
    /* Upstream's slots (v24, v25). */
    if (s_cbs.last_store_pc) *s_cbs.last_store_pc = TEST_STORE_PC;
    cpu->gpr[3] = s_cbs.mod_function_entry
        ? (uint32_t)s_cbs.mod_function_entry(cpu, 0x80010000u) + 0x100u
        : 0u;
#if !defined(TEST_UPSTREAM_V25)
    /* The fork's slots (v26). */
    if (s_cbs.cpu_step_boundary_enabled && s_cbs.cpu_step_boundary_enabled(0) &&
        s_cbs.cpu_step_boundary)
        s_cbs.cpu_step_boundary(cpu, 0x80010000u);
#endif
    cpu->gpr[2] = TEST_MARKER;
}

TEST_EXPORT void func_80010004(CPUState *cpu) { cpu->gpr[4] = TEST_MARKER; }
