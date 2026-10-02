import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class OverlayWidescreenCallbacks(unittest.TestCase):
    def test_native_module_forwards_screen_bounds_and_entry_hooks(self):
        root = Path(__file__).resolve().parents[2]
        gcc = shutil.which('gcc') or ('C:/msys64/mingw64/bin/gcc.exe' if os.name == 'nt' else None)
        if not gcc or not Path(gcc).exists():
            self.skipTest('C compiler unavailable')
        source = '''
#define PSX_OVERLAY_DLL_BUILD 1
#define PSX_OVERLAY_EXPORT
#include "overlay_dispatch_preamble.c.inc"
#include <assert.h>
static CPUState *seen_cpu;
static uint32_t seen_address, cycles;
static int32_t bound(int32_t x) { return x * 2; }
static int margin120(void) { return 120; }
static void advance(uint32_t n) { cycles += n; }
static int entry(CPUState *cpu, uint32_t address) {
    assert(cycles == 17); seen_cpu = cpu; seen_address = address;
    return 1;
}
int main(void) {
    CPUState cpu = {0};
    OverlayCallbacks callbacks = {0};
    callbacks.ws_screen_x_bound = bound;
    callbacks.mod_function_entry = entry;
    callbacks.advance_cycles = advance;
    overlay_init(&callbacks);
    assert(overlay_abi() == PSX_OVERLAY_ABI_TAG);
    assert(psx_ws_screen_x_bound(-256) == -512);
    assert(psx_ws_screen_x_bound(256) == 512);
    psx_advance_cycles(17);
    assert(psx_mod_function_entry(&cpu, 0x80045770) == 1);
    assert(seen_cpu == &cpu && seen_address == 0x80045770);
    callbacks.ws_screen_x_bound = 0; callbacks.mod_function_entry = 0;
    overlay_init(&callbacks);
    assert(psx_ws_screen_x_bound(-256) == -256);
    assert(psx_mod_function_entry(&cpu, 0) == 0);
    assert(seen_address == 0x80045770);
    /* bgez_sites / clip_edge_x_load_sites in overlay code: identity with no
     * margin callback (4:3 host), widened by the live margin with one. */
    assert(psx_ws_cull_bgez(0u) == 1 && psx_ws_cull_bgez((uint32_t)-1) == 0);
    assert(psx_ws_clip_edge_x(0u, 0x140u) == 0u);
    assert(psx_ws_clip_edge_x(0x140u, 0x140u) == 0x140u);
    /* screen_x_sites on lui / bltz: a screen X in the high half. */
    assert(psx_ws_cull_lui_hi(0x0200u) == (0x0200u << 16));
    assert(psx_ws_cull_bltz_hi((uint32_t)-1 << 16) == 1 && psx_ws_cull_bltz_hi(0u) == 0);
    callbacks.ws_x_margin = margin120;
    overlay_init(&callbacks);
    assert(psx_ws_cull_bgez((uint32_t)-120) == 1);
    assert(psx_ws_cull_bgez((uint32_t)-121) == 0);
    assert(psx_ws_clip_edge_x(0u, 0x140u) == (uint32_t)-120);
    assert(psx_ws_clip_edge_x(0x140u, 0x140u) == 0x140u + 120u);
    assert(psx_ws_clip_edge_x(98u, 0x140u) == 98u);     /* interior viewport edge */
    assert(psx_ws_clip_edge_x(222u, 0x140u) == 222u);
    assert(psx_ws_cull_lui_hi(0x0200u) == ((0x0200u + 120u) << 16));
    assert(psx_ws_cull_bltz_hi((uint32_t)-120 << 16) == 0);
    assert(psx_ws_cull_bltz_hi((uint32_t)-121 << 16) == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path/'test.c').write_text(source, encoding='utf-8', newline='\n')
            executable = path / ('test.exe' if os.name == 'nt' else 'test')
            subprocess.run([gcc, '-std=c11', '-DPSX_ENABLE_BLOCK_CYCLES=1',
                            '-DPSX_NO_DEBUG_TOOLS', '-I'+str(root/'runtime/include'),
                            str(path/'test.c'), '-o', str(executable), '-lm'], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
