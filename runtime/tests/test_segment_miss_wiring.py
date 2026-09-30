"""Segment misses are recorded at dispatch and reach every surface.

docs/SEGMENT_AWARE_CODE.md §5.5 (§10 decision 2): a PC in the EXE's text whose
physical word has a compiled row only in another segment is a segment miss. It
runs interpreted through dirty_ram_dispatch_inner's clean-text-miss path and is
recorded, so the run names each (segment, entry) that needs a seed.

The classification and the ring are unit-tested (test_psx_segment_miss.c,
psx_segment_miss_note included). What a unit test cannot see is the wiring
inside the runtime, which only runs in a full build: deleting the dispatch
hook, the TCP command or a report field would leave every other test green
and make segment misses silent again. This guard pins that wiring and the
`segment_misses` summary row shape.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
INTERP = (ROOT / 'runtime/src/dirty_ram_interp.c').read_text(encoding='utf-8')
DEBUG = (ROOT / 'runtime/src/debug_server.c').read_text(encoding='utf-8')
CRASH = (ROOT / 'runtime/src/crash_trace.c').read_text(encoding='utf-8')
CMAKE = (ROOT / 'runtime/runtime.cmake').read_text(encoding='utf-8')


def body(source, signature):
    start = source.index(signature)
    depth, pos = 0, source.index('{', start)
    while True:
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if depth == 0:
                return source[start:pos]
        pos += 1


def main():
    assert 'runtime/src/psx_segment_miss.c' in CMAKE, 'the runtime does not link psx_segment_miss.c'

    # The hook: every clean game-text miss goes to psx_segment_miss_note with
    # the full PC, inside the game-dispatch block that sets the flag.
    inner = body(INTERP, 'static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, '
                         'uint32_t stop_addr) {')
    block = inner[inner.index('#ifdef PSX_HAS_GAME_DISPATCH'):]
    block = block[:block.index('#endif\n\n    /* B-2')]
    assert re.search(r'clean_game_text_miss = psx_game_address_in_text\(addr\) \? 1 : 0;', block), \
        'a compiled-dispatch miss in game text no longer sets clean_game_text_miss'
    hook = re.search(r'if \(clean_game_text_miss\)\s*\(void\)psx_segment_miss_note\(addr, '
                     r'psx_game_is_function_entry,\s*cpu->gpr\[31\], cpu->gpr\[29\],', block)
    assert hook, 'dirty_ram_dispatch_inner no longer notes segment misses on a clean-text miss'
    assert block.index('psx_dispatch_game_compiled(cpu, addr)') < hook.start(), \
        'the segment-miss note must follow the compiled-dispatch attempt'

    # TCP: the command, and the counters on dispatch_stats and ping.
    assert re.search(r'\{\s*"segment_misses",\s*handle_segment_misses\s*\}', DEBUG), \
        'TCP segment_misses is not registered'
    seg = body(DEBUG, 'static void handle_segment_misses(int id, const char *json)')
    # The summary row shape (the seed tools read `pc`).
    assert '\\"summary\\":[' in seg and \
        '{\\"pc\\":\\"0x%08X\\",\\"home\\":\\"0x%08X\\",\\"count\\":%llu}' in seg, \
        'segment_misses summary rows changed shape'
    stats = body(DEBUG, 'static void handle_dispatch_stats(int id, const char *json)')
    assert 'segment_miss_total' in stats and 'psx_segment_miss_total()' in stats
    assert 'segment_miss_unique' in stats and 'psx_segment_miss_unique()' in stats
    ping = body(DEBUG, 'static void handle_ping(int id, const char *json)')
    assert 'segment_miss_total' in ping and 'psx_segment_miss_total()' in ping, \
        'ping no longer surfaces segment misses'

    # Exit report (psx_last_run_report.json).
    dump = body(CRASH, 'void psx_crash_trace_dump(const char *reason, void *seh_info) {')
    assert '\\"segment_misses\\"' in dump and 'psx_segment_miss_summary(' in dump, \
        'the exit report lost its segment_misses section'
    print('segment-miss wiring guards passed')


if __name__ == '__main__':
    main()
