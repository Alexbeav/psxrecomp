"""Widescreen engages on every guest frame, headless or not (PS1B-385).

The per-frame present routine, sdl_vblank_present_body in runtime/src/main.cpp,
engages widescreen at game entry (g_ws_engaged, refresh_widescreen_projection)
and then returns early for presentation skips: headless, TCP turbo, load and
FMV present skips. When the engage block sat below the headless return, a
headless run with a wide aspect stayed at ws.mode 0 and x_margin 0, so no
widescreen change could be checked headless and the headless game ran a
different GTE projection than the windowed build.

This guard pins the order: the headless early-out (`if (g_headless) {`, which
returns) comes after the engage block. Restart and quit exits are not checked. Optional argument: another main.cpp to check.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
SIGNATURE = 'static NetplayVblankEpilogue sdl_vblank_present_body(void) {'
ENGAGE = 'if (!g_ws_engaged) {'


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
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'runtime/src/main.cpp'
    fn = body(path.read_text(encoding='utf-8', errors='replace'), SIGNATURE)
    assert ENGAGE in fn, 'sdl_vblank_present_body must engage widescreen at game entry'
    engage = fn.index(ENGAGE)
    headless = fn.find('if (g_headless) {')
    assert headless >= 0, 'sdl_vblank_present_body has no headless present early-out to check'
    assert headless > engage, (
        f'sdl_vblank_present_body returns for headless at offset {headless}, before it '
        f'engages widescreen at offset {engage}: headless runs never engage widescreen (PS1B-385)')
    print('ok: widescreen engages before the headless early-out')


if __name__ == '__main__':
    main()
