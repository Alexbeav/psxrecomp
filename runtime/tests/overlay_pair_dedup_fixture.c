#include "overlay_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

#ifndef TEST_PAIR_ID
#define TEST_PAIR_ID UINT64_C(0x1020304050607080)
#endif

#ifndef TEST_MARKER
#define TEST_MARKER 0xC001CAFEu
#endif

#ifndef TEST_INSTANCE
#define TEST_INSTANCE 0
#endif

static int s_init_count;
static int s_flush_count;
static int s_call_count;

static void trace(const char *event) {
    const char *path = getenv("PSX_PAIR_TEST_TRACE");
    if (!path || !*path) return;
    FILE *out = fopen(path, "ab");
    if (!out) return;
    fprintf(out, "%s %d\n", event, TEST_INSTANCE);
    fclose(out);
}

TEST_EXPORT int overlay_abi(void) { return PSX_OVERLAY_ABI_TAG; }
TEST_EXPORT uint64_t overlay_pair_id(void) { return TEST_PAIR_ID; }

TEST_EXPORT void overlay_init(const OverlayCallbacks *callbacks) {
    (void)callbacks;
    s_init_count++;
    trace("init");
}

TEST_EXPORT void overlay_flush_cycles(void) {
    s_flush_count++;
    trace("flush");
}

TEST_EXPORT int test_init_count(void) { return s_init_count; }
TEST_EXPORT int test_flush_count(void) { return s_flush_count; }
TEST_EXPORT int test_call_count(void) { return s_call_count; }

/* A shard's exports are named by the full VA of the segment it was compiled
 * for (docs/SEGMENT_AWARE_CODE.md §5.1, §5.7): KSEG0 by default, KUSEG or
 * KSEG1 when the segment tests build it that way. */
#if defined(TEST_SEGMENT_KUSEG)
#define ENTRY(lo) func_0001##lo
#elif defined(TEST_SEGMENT_KSEG1)
#define ENTRY(lo) func_A001##lo
#else
#define ENTRY(lo) func_8001##lo
#endif

TEST_EXPORT void ENTRY(0000)(CPUState *cpu) {
    s_call_count++;
    cpu->gpr[2] = TEST_MARKER;
    cpu->gpr[6] = TEST_INSTANCE;   /* which shard ran */
    cpu->gpr[7] = cpu->pc;         /* the PC it was entered for (CPS routing) */
    trace("call");
}

#ifndef TEST_PARTIAL_EXPORTS
TEST_EXPORT void ENTRY(0004)(CPUState *cpu) { cpu->gpr[3] = TEST_MARKER; }
TEST_EXPORT void ENTRY(0008)(CPUState *cpu) { cpu->gpr[4] = TEST_MARKER; }
TEST_EXPORT void ENTRY(000C)(CPUState *cpu) { cpu->gpr[5] = TEST_MARKER; }
#endif
