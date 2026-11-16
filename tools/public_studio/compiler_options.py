"""Translate compiler choices into the selected core CLI's real flags."""
import re


def arguments(optimization='optimized', lto=True, *, source=False):
    if optimization not in {'fast', 'optimized'}:
        raise ValueError('Unknown compiler optimization')
    level='-O1' if optimization=='fast' else '-O3'
    result=[f'--cmake-extra=-DCMAKE_C_FLAGS_RELEASE={level} -DNDEBUG',
            f'--cmake-extra=-DCMAKE_CXX_FLAGS_RELEASE={level} -DNDEBUG']
    if source:
        result.append('--cmake-extra=-DPSX_RUNTIME_IPO=' + ('ON' if lto else 'OFF'))
    elif not lto:
        result.append('--no-lto')
    return result


def verify(build_dir, optimization, lto):
    ninja=(build_dir/'build.ninja').read_text(encoding='utf-8')
    blocks=re.findall(r'(?m)^build [^\n]*psx-runtime[^\n]*\n(?:  [^\n]*\n)*',ninja)
    compiler=[];linker=[]
    for block in blocks:
        flags=re.findall(r'(?m)^  FLAGS = (.*)$',block)
        if not flags:continue
        if '_COMPILER__' in block.splitlines()[0]:compiler+=flags
        elif '_EXECUTABLE_LINKER__' in block.splitlines()[0]:linker+=flags
    if not compiler or not linker:
        raise RuntimeError('Cannot verify requested compiler options in the selected core build')
    level='-O1' if optimization=='fast' else '-O3'
    for flags in compiler:
        levels=re.findall(r'(?<!\S)(-O[0-3sgz])(?=\s|$)',flags)
        if not levels or levels[-1]!=level:
            raise RuntimeError('The selected core did not apply the requested compiler optimization')
    actual_lto=any('-flto' in flags for flags in compiler+linker)
    if actual_lto != lto:
        raise RuntimeError('The selected core did not apply the requested runtime LTO choice')
    return {'optimization':optimization,'lto':lto,'runtime_compile_rules':len(compiler),'runtime_link_rules':len(linker)}
