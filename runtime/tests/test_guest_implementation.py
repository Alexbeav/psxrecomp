"""Compile/run real CMake bindings, including the retained reference bodies."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--generator', default='Ninja')
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    framework = Path(__file__).resolve().parents[2]
    args.work_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='binding-', dir=args.work_root)).resolve()
    assert root.is_relative_to(args.work_root.resolve())

    def run(command, ok=True):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        if (result.returncode == 0) != ok:
            raise AssertionError(result.stdout)
        return result.stdout

    def case(name, implementation, *, duplicate=False, missing=False, second=False):
        source = root / name
        source.mkdir()
        (source / 'cpu.h').write_text('typedef struct { int value; } CPUState;\n')
        generated = '#include "cpu.h"\nvoid func_80000100(CPUState* cpu) { cpu->value = 41; }\n'
        generated += 'void func_80000200(CPUState* cpu) { cpu->value = 42; }\n'
        (source / 'original.c').write_text(generated)
        (source / 'dispatch.c').write_text('''#include "cpu.h"
#include <stdio.h>
void func_80000100(CPUState*);
int main(void) {
    CPUState cpu = {0}; func_80000100(&cpu);
    printf("value=%d tag=%u identity=%s\\n", cpu.value,
        PSX_SAVESTATE_IMPL_TAG, PSX_BUILD_IMPLEMENTATIONS);
    return 0;
}
''')
        native = '''#include "cpu.h"
void reference_func_80000100(CPUState*);
void func_80000100(CPUState* cpu) { reference_func_80000100(cpu); cpu->value += 100; }
'''
        if second:
            native += '''void reference_func_80000200(CPUState*);
void func_80000200(CPUState* cpu) { reference_func_80000200(cpu); cpu->value += 100; }
'''
        (source / 'native.c').write_text(native)
        sources = '${CMAKE_CURRENT_SOURCE_DIR}/original.c'
        if duplicate:
            (source / 'duplicate.c').write_text(generated)
            sources += ' ${CMAKE_CURRENT_SOURCE_DIR}/duplicate.c'
        symbol = 'func_missing' if missing else 'func_80000100'
        calls = f'''psxrecomp_select_guest_implementation(
    NAME demo IMPLEMENTATION {implementation}
    TARGETS base pgxp GENERATED_SOURCES {sources}
    HLE_SOURCES "${{CMAKE_CURRENT_SOURCE_DIR}}/native.c"
    SYMBOLS {symbol} REFERENCE_PREFIX reference_ SNAPSHOT_TAG 0x54474801)
'''
        if second:
            calls += f'''psxrecomp_select_guest_implementation(
    NAME other IMPLEMENTATION HLE TARGETS base pgxp
    GENERATED_SOURCES {sources} HLE_SOURCES "${{CMAKE_CURRENT_SOURCE_DIR}}/native.c"
    SYMBOLS func_80000200 REFERENCE_PREFIX reference_ SNAPSHOT_TAG 0x00010000)
'''
        cmake = f'''cmake_minimum_required(VERSION 3.20)
project(binding C)
include("{framework.as_posix()}/runtime/guest_implementation.cmake")
foreach(target base pgxp)
    add_executable(${{target}} original.c dispatch.c)
    target_include_directories(${{target}} PRIVATE "{framework.as_posix()}/runtime/include")
endforeach()
{calls}'''
        (source / 'CMakeLists.txt').write_text(cmake)
        build = source / 'build'
        configured = run([args.cmake, '-S', str(source), '-B', str(build),
                          '-G', args.generator, f'-DCMAKE_C_COMPILER={args.compiler}'],
                         ok=not (duplicate or missing))
        if duplicate or missing:
            assert 'expected one definition' in configured, configured
            return
        run([args.cmake, '--build', str(build), '--config', 'Release'])
        value = 141 if implementation == 'HLE' else 41
        tag = 0x54474801 if implementation == 'HLE' else 0
        identity = f'demo={implementation}'
        if second:
            tag ^= 0x10000
            identity += ',other=HLE'
        for target in ('base', 'pgxp'):
            exe = build / (target + ('.exe' if os.name == 'nt' else ''))
            if not exe.exists():
                exe = build / 'Release' / exe.name
            output = run([str(exe)])
            assert f'value={value} tag={tag} identity={identity}' in output, output
            assert f'guest implementation: demo={implementation}' in output, output

    case('lle', 'LLE')
    case('hle', 'HLE')
    case('multiple-families', 'HLE', second=True)
    case('missing-symbol', 'HLE', missing=True)
    case('duplicate-definition', 'HLE', duplicate=True)
    print('PASS: HLE/LLE dispatch, reference retention, variant identity, composed tags and invalid bindings')


if __name__ == '__main__':
    main()
