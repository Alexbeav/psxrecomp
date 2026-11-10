#!/usr/bin/env python3
"""The include directory handed to tcc holds every file a BIOS module includes.

tcc 0.9.27 does not skip a UTF-8 BOM, so compile_overlays._bom_free_incdir()
gives it a BOM-stripped copy of each include directory instead of the real one.
tools/bios_module_build.py writes a glue.c that includes two .c.inc fragments
from runtime/include by name. If the copy leaves them out, the tcc build of
every BIOS module stops at "include file ... not found" and the runtime falls
back to the bundled BIOS.

Needs no compiler and no built target.

    python3 recompiler/tests/test_tcc_module_include_dir.py
"""

import ast
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TOOLS = os.path.join(ROOT, 'tools')
RUNTIME_INCLUDE = os.path.join(ROOT, 'runtime', 'include')
BOM = b'\xef\xbb\xbf'


def load_bom_free_incdir():
    """compile_overlays._bom_free_incdir and its memo, without importing the
    whole tool: the test then runs on any Python 3 and needs none of the
    tool's other imports."""
    path = os.path.join(TOOLS, 'compile_overlays.py')
    with open(path, encoding='utf-8') as f:
        tree = ast.parse(f.read(), filename=path)
    keep = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == '_bom_free_incdir':
            keep.append(node)
        elif isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == '_TCC_BOMFREE_INC' for t in node.targets):
            keep.append(node)
    assert len(keep) == 2, 'compile_overlays.py no longer defines _bom_free_incdir and its memo'
    scope = {'os': os, 'tempfile': tempfile}
    exec(compile(ast.Module(body=keep, type_ignores=[]), path, 'exec'), scope)
    return scope['_bom_free_incdir']


def module_glue_includes():
    """Quoted include names bios_module_build.py writes into a module."""
    with open(os.path.join(TOOLS, 'bios_module_build.py'), encoding='utf-8') as f:
        text = f.read()
    return sorted(set(re.findall(r'#include \\?"([A-Za-z0-9_./]+)\\?"', text)))


def main():
    bom_free_incdir = load_bom_free_incdir()
    made = []
    try:
        # A synthetic directory: headers and fragments are copied without
        # their BOM, anything else is left out.
        src = tempfile.mkdtemp(prefix='tcc_inc_src_')
        made.append(src)
        files = {
            'a.h': BOM + b'#define A 1\n',
            'b.c.inc': BOM + b'static int b;\n',
            'c.inc': b'static int c;\n',
            'notes.txt': b'not a C input\n',
        }
        for name, data in files.items():
            with open(os.path.join(src, name), 'wb') as f:
                f.write(data)
        out = bom_free_incdir(src)
        made.append(out)
        assert sorted(os.listdir(out)) == ['a.h', 'b.c.inc', 'c.inc'], sorted(os.listdir(out))
        for name in ('a.h', 'b.c.inc', 'c.inc'):
            with open(os.path.join(out, name), 'rb') as f:
                data = f.read()
            assert not data.startswith(BOM), f'{name} kept its BOM'
            want = files[name][3:] if files[name].startswith(BOM) else files[name]
            assert data == want, f'{name} content changed'

        # The real directory: everything a module's glue.c includes resolves.
        wanted = module_glue_includes()
        assert 'overlay_dispatch_preamble.c.inc' in wanted, wanted
        assert 'psx_bios_module_glue.c.inc' in wanted, wanted
        real = bom_free_incdir(RUNTIME_INCLUDE)
        made.append(real)
        missing = [n for n in wanted if not os.path.isfile(os.path.join(real, n))]
        assert not missing, f'tcc include copy lacks: {missing}'
        print(f'OK: tcc include copy holds {len(wanted)} module includes: {", ".join(wanted)}')
        return 0
    finally:
        for d in made:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
