"""Observe actual interpreter/compiled delivery arguments at the same guest slot."""
import argparse
import importlib.util
from pathlib import Path
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--opt', choices=('O0', 'O2'), action='append',
                        help='select one optimization for an independent parent control')
    parser.add_argument('--src-root', type=Path,
                        help='runtime sources to compare (allows an exact-parent control)')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = args.src_root.resolve() if args.src_root else here.parent
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        # Existing helper has bare text=True calls. Use an owned UTF-8 copy so
        # Windows compiler messages do not depend on the active ANSI codepage.
        helper = work / 'source_fixture_link.py'
        helper.write_text((here / helper.name).read_text(encoding='utf-8').replace(
            'text=True', "text=True, encoding='utf-8', errors='replace'"), encoding='utf-8')
        spec = importlib.util.spec_from_file_location('irq_fixture_link', helper)
        link = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(link)
        modules = []
        for name, marker, production in (
            ('irq_handoff_interp.c', '@DIRTY_RAM_SOURCE@', 'dirty_ram_interp.c'),
            ('irq_handoff_compiled.c', '@COMPILED_SOURCE@', 'source_gpu_runtime.c'),
        ):
            wrapper = work / name
            wrapper.write_text((here / (name + '.in')).read_text(encoding='utf-8').replace(
                marker, (source / 'src' / production).as_posix()), encoding='utf-8')
            modules.append(str(wrapper))
        for selected in args.opt or ('O0', 'O2'):
            option = '-' + selected
            output = work / option[1:]
            output.mkdir()
            link.build_and_run(args.cc, here, source, option, output, modules,
                               'test_irq_handoff_not_taken.c')
    print('PASS: production interpreter/compiled hand-over consistency (' +
          '/'.join(args.opt or ('O0', 'O2')) + ')')


if __name__ == '__main__':
    main()
