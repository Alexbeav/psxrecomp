"""PS1B-519: the compiled IRQ hand-off consumes generated BLTZ decisions."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import contextlib
from source_fixture_link import build_and_run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--opt', choices=('O0', 'O2'), action='append',
                        help='run one optimisation level only')
    parser.add_argument('--src-root', type=Path,
                        help='runtime tree whose sources to link (a control tree)')
    parser.add_argument('--work-dir', type=Path, help='retain executable/debug objects for diagnosis')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = args.src_root.resolve() if args.src_root else here.parent
    levels = args.opt or ['O0', 'O2']
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    with (contextlib.nullcontext(str(args.work_dir)) if args.work_dir
          else tempfile.TemporaryDirectory()) as temporary:
        work = Path(temporary)
        modules = []
        for name, marker, production in (
            ('irq_handoff_ws_cull_interp.c', '@DIRTY_RAM_SOURCE@', 'dirty_ram_interp.c'),
            ('irq_handoff_ws_cull_compiled.c', '@COMPILED_SOURCE@', 'source_gpu_runtime.c'),
        ):
            wrapper = work / name
            text = (here / (name + '.in')).read_text(encoding='utf-8').replace(
                marker, (source / 'src' / production).as_posix())
            if name == 'irq_handoff_ws_cull_compiled.c':
                compiled_source = (source / 'src' / production).read_text(
                    encoding='utf-8', errors='replace')
                latch = ('source_gpu_runtime_latch_bltz(slot_pc - 4u, generated_taken);'
                         if 'source_gpu_runtime_latch_bltz' in compiled_source
                         else '(void)generated_taken; /* baseline: generated decision not handed off */')
                text = text.replace('@GENERATED_LATCH@', latch)
                reset = ('compiled_bltz_reset();'
                         if 's_compiled_bltz_valid' in compiled_source
                         else '/* baseline has no decision token to reset */')
                text = text.replace('@TOKEN_RESET@', reset)
                direct_latch = ('source_gpu_runtime_latch_bltz(branch_pc, taken);'
                                if 'source_gpu_runtime_latch_bltz' in compiled_source
                                else '(void)branch_pc; (void)taken;')
                text = text.replace('@DIRECT_LATCH@', direct_latch)
            wrapper.write_text(text, encoding='utf-8')
            modules.append(str(wrapper))
        modules.extend(('gpu.c', 'psx_ram_geometry.c'))  # real cull and retail RAM geometry
        for level in levels:
            output = work / level
            output.mkdir()
            build_and_run(args.cc, here, source, '-' + level, output, modules,
                          'test_irq_handoff_ws_cull.c',
                          extra_compile_flags=('-g',) if args.work_dir else ())
            # The helper shows the fixture's lines only on a failure; print the
            # measured values of a passing run as well.
            exe = output / ('sectionwire' + level + ('.exe' if os.name == 'nt' else ''))
            if exe.exists():
                env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PSX_')}
                shown = subprocess.run([str(exe)], env=env, capture_output=True,
                                       encoding='utf-8', errors='replace', timeout=60)
                print('-' + level + ':\n' + shown.stdout, end='')
    print('PASS: compiled hand-off consumes generated BLTZ decisions ('
          + '/'.join(levels) + ')')


if __name__ == '__main__':
    main()
