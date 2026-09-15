"""Compile/load the real overlay shim and check observer batch ownership."""
from pathlib import Path
import ctypes
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GCC = shutil.which('gcc')
assert GCC, 'overlay boundary test requires GCC on PATH'
SOURCE = r'''
#include <assert.h>
#include "cpu_state.h"
#include "psx_icache.h"
#include "overlay_dispatch_preamble.c.inc"
static int installed, replay, calls, advances;
static uint64_t clock_value;
static uint32_t last_address;
static CPUState *last_cpu;
static int enabled(int include_replay) {
    return installed && (include_replay || !replay);
}
static void advance(uint32_t cycles) { clock_value += cycles; advances++; }
static void boundary(CPUState *cpu, uint32_t address) {
    assert(enabled(0));
    assert(s_pending_cycles == 0);
    last_cpu = cpu; last_address = address; calls++;
}
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int test_boundary(void) {
    CPUState cpu = {0};
    OverlayCallbacks cbs = {0};
    cbs.advance_cycles = advance;
    cbs.cpu_step_boundary_enabled = enabled;
    cbs.cpu_step_boundary = boundary;
    overlay_init(&cbs);
    assert(overlay_abi() == PSX_OVERLAY_ABI_TAG);
    for (int state = 0; state < 4; state++) {
        installed = state & 1; replay = (state >> 1) & 1;
        calls = advances = 0; clock_value = 0;
        assert(!!g_psx_cpu_step_boundary_callback == !!installed);
        psx_advance_cycles(7);
        psx_cpu_step_boundary(&cpu, 0x80010004);
        int active = installed && !replay;
        assert(calls == active && advances == active);
        assert(clock_value == (active ? 7 : 0));
        if (active) assert(last_cpu == &cpu && last_address == 0x80010004);
        overlay_flush_cycles();
        assert(clock_value == 7 && advances == 1);
    }
    /* Live installation changes must be queried afresh for cached followers. */
    installed = replay = calls = advances = 0; clock_value = 0;
    psx_advance_cycles(3);
    psx_cpu_step_boundary(&cpu, 0x80010008);
    installed = 1;
    psx_advance_cycles(5);
    psx_cpu_step_boundary(&cpu, 0x8001000C);
    assert(clock_value == 8 && calls == 1 && advances == 1);
    assert(last_address == 0x8001000C);
    installed = 0;
    psx_advance_cycles(2);
    psx_cpu_step_boundary(&cpu, 0x80010010);
    assert(clock_value == 8 && calls == 1);
    overlay_flush_cycles();
    assert(clock_value == 10 && advances == 2);
    /* A host with no observer callbacks retains ordinary batch behavior. */
    cbs.cpu_step_boundary_enabled = 0; cbs.cpu_step_boundary = 0;
    overlay_init(&cbs);
    assert(!g_psx_cpu_step_boundary_callback);
    psx_advance_cycles(9);
    psx_cpu_step_boundary(&cpu, 0x80010014);
    assert(clock_value == 10 && calls == 1);
    overlay_flush_cycles();
    assert(clock_value == 19 && advances == 3);
    return 6;
}
'''

for opt in ('0', '2'):
    with tempfile.TemporaryDirectory() as td:
        p = Path(td)
        (p/'boundary.c').write_text(SOURCE, encoding='utf-8')
        dll = p/('boundary.dll' if os.name == 'nt' else 'boundary.so')
        command = [GCC, '-shared', '-fPIC', '-O'+opt,
                   '-DPSX_OVERLAY_DLL_BUILD', '-DPSX_ENABLE_BLOCK_CYCLES=1',
                   '-DPSX_NO_DEBUG_TOOLS', '-I'+str(ROOT/'runtime/include'),
                   str(p/'boundary.c'), '-o', str(dll), '-lm']
        subprocess.run(command, check=True, capture_output=True)
        lib = ctypes.CDLL(str(dll))
        assert lib.test_boundary() == 6
        if os.name == 'nt':
            import _ctypes
            _ctypes.FreeLibrary(lib._handle)
        del lib
print('12 overlay boundary cases passed at O0/O2')
