"""PS1B-520: the dirty-RAM interpreter decides a branch on a [widescreen.cull]
branch-site list (bltz_sites, branch_keep_sites, nclip_keep_sites,
nclip_exact_sites) as the generated code decides it.

The compiled side is the generator's condition text from
recompiler/tests/ws_branch_site_cases.h (checked against the generator by
recompiler_patch_test). --src-root links another runtime tree's interpreter: a
tree without dirty_ram_set_ws_branch_sites (the stage before PS1B-520) gets no
lists, so its interpreter decides every listed site raw and the fixture fails.
"""
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
    cases = here.parent.parent / 'recompiler' / 'tests' / 'ws_branch_site_cases.h'
    if not cases.is_file():
        raise SystemExit('missing ' + str(cases))
    interp_h = (source / 'include' / 'dirty_ram_interp.h').read_text(encoding='utf-8',
                                                              errors='replace')
    has_setter = 'dirty_ram_set_ws_branch_sites' in interp_h
    defines = ('WS_FIXTURE_HAVE_BRANCH_SITES',) if has_setter else ()
    levels = args.opt or ['O0', 'O2']
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        modules = []
        for name, marker, path in (
            ('interp_ws_branch_sites_interp.c', '@DIRTY_RAM_SOURCE@',
             source / 'src' / 'dirty_ram_interp.c'),
            ('interp_ws_branch_sites_source.c', '@SOURCE_RUNTIME@',
             source / 'src' / 'source_gpu_runtime.c'),
            ('interp_ws_branch_sites_cases.c', '@CASES_HEADER@', cases),
        ):
            wrapper = work / name
            wrapper.write_text((here / (name + '.in')).read_text(encoding='utf-8').replace(
                marker, path.as_posix()), encoding='utf-8')
            modules.append(str(wrapper))
        modules.append('gpu.c')  # production widescreen margin and cull helpers
        for level in levels:
            output = work / level
            output.mkdir()
            build_and_run(args.cc, here, source, '-' + level, output, modules,
                          'test_interp_ws_branch_sites.c', defines)
            # The helper shows the fixture's lines only on a failure; print the
            # measured values of a passing run as well.
            exe = output / ('sectionwire' + level + ('.exe' if os.name == 'nt' else ''))
            if exe.exists():
                env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PSX_')}
                shown = subprocess.run([str(exe)], env=env, capture_output=True,
                                       encoding='utf-8', errors='replace', timeout=60)
                print('-' + level + ':\n' + shown.stdout, end='')
    print('PASS: the interpreter decides each [widescreen.cull] branch-site list as the '
          'generated code does (' + '/'.join(levels) + ')')


if __name__ == '__main__':
    main()
