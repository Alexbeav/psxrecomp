"""PS1B-510: the compiled IRQ hand-off decides a widescreen funnel BLTZ as the
interpreter does (Cause.BT and register 6 follow psx_ws_cull_bltz)."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from source_fixture_link import build_and_run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--opt', choices=('O0', 'O2'), action='append',
                        help='run one optimisation level only')
    parser.add_argument('--src-root', type=Path,
                        help='runtime tree whose sources to link (a control tree)')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = args.src_root.resolve() if args.src_root else here.parent
    levels = args.opt or ['O0', 'O2']
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        modules = []
        for name, marker, production in (
            ('irq_handoff_ws_cull_interp.c', '@DIRTY_RAM_SOURCE@', 'dirty_ram_interp.c'),
            ('irq_handoff_ws_cull_compiled.c', '@COMPILED_SOURCE@', 'source_gpu_runtime.c'),
        ):
            wrapper = work / name
            wrapper.write_text((here / (name + '.in')).read_text(encoding='utf-8').replace(
                marker, (source / 'src' / production).as_posix()), encoding='utf-8')
            modules.append(str(wrapper))
        modules.append('gpu.c')  # production widescreen cull, unchanged
        for level in levels:
            output = work / level
            output.mkdir()
            build_and_run(args.cc, here, source, '-' + level, output, modules,
                          'test_irq_handoff_ws_cull.c')
            # The helper shows the fixture's lines only on a failure; print the
            # measured values of a passing run as well.
            exe = output / ('sectionwire' + level + ('.exe' if os.name == 'nt' else ''))
            if exe.exists():
                env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PSX_')}
                shown = subprocess.run([str(exe)], env=env, capture_output=True,
                                       encoding='utf-8', errors='replace', timeout=60)
                print('-' + level + ':\n' + shown.stdout, end='')
    print('PASS: compiled hand-off decides a widescreen funnel BLTZ as the interpreter does ('
          + '/'.join(levels) + ')')


if __name__ == '__main__':
    main()
