#!/usr/bin/env python3
"""Keep psxrecomp_cli.py's import closure importable on Python 3.9.

RHEL/Rocky 9, Debian 11 and macOS 13/14 still ship 3.9 as the system python3.
The CLI imports release_stage -> compile_overlays to stage the overlay
toolchain, and a failed import there is swallowed as a warning: the product
builds, but overlays silently run interpreted (athena-rocky, T115). So the
floor is checked statically on whatever Python runs ctest:

  * every module reached from psxrecomp_cli.py, and from the scripts it spawns
    with sys.executable, parses as 3.9 grammar (no match statements, no except*);
  * PEP 604 unions (`X | None`) only appear where 3.9 never evaluates them,
    i.e. in annotations of a module with `from __future__ import annotations`;
  * no 3.10+ stdlib calls from a short list of ones that have bitten before.

It also imports compile_overlays with tomllib and tomli hidden, which a 3.9
host without tomli looks like: the import must not exit the CLI.

Set PSXRECOMP_FLOOR_PYTHON to a 3.9 interpreter to also run the imports under it.
"""

from __future__ import annotations

import ast
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FLOOR = (3, 9)
SEARCH = (ROOT, ROOT / "tools")

TYPE_NAMES = {
    "None", "str", "int", "float", "bytes", "bool", "list", "dict", "tuple",
    "set", "frozenset", "type", "object", "Path", "Any", "Optional", "Union",
    "List", "Dict", "Tuple", "Set", "Callable", "Iterable", "Iterator",
    "Sequence", "Mapping",
}
# 3.10+ APIs: attribute / bare-name uses and call keywords.
NEW_ATTRS = {"bit_count", "pairwise", "hardlink_to", "file_digest", "UTC",
             "TypeAlias", "ParamSpec", "Self", "StrEnum"}
NEW_KWARGS = {"zip": {"strict"}, "write_text": {"newline"},
              "read_text": {"newline"}, "dataclass": {"slots", "kw_only"}}


def resolve(name: str) -> Path | None:
    for base in SEARCH:
        for cand in (base / (name.replace(".", "/") + ".py"),
                     base / name.replace(".", "/") / "__init__.py"):
            if cand.is_file():
                return cand
    return None


def import_closure(entry: Path) -> dict[Path, str]:
    seen: dict[Path, str] = {}
    todo = [entry]
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        src = path.read_text(encoding="utf-8")
        seen[path] = src
        # Function-local imports count: stage_overlay_toolchain_for_product
        # imports release_stage inside the function body.
        for node in ast.walk(ast.parse(src)):
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
                names = [node.module]
            else:
                continue
            for name in names:
                found = resolve(name)
                if found is not None and found not in seen:
                    todo.append(found)
    return seen


def is_type_operand(node: ast.AST, classes: set[str]) -> bool:
    if isinstance(node, ast.Constant):
        return node.value is None
    if isinstance(node, ast.Name):
        return node.id in TYPE_NAMES or node.id in classes
    if isinstance(node, ast.Attribute):
        return node.attr in TYPE_NAMES
    if isinstance(node, ast.Subscript):
        return is_type_operand(node.value, classes)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr):
        return is_type_operand(node.left, classes) or is_type_operand(node.right, classes)
    return False


def floor_violations(src: str, label: str) -> list[str]:
    try:
        tree = ast.parse(src, feature_version=FLOOR)
    except SyntaxError as exc:
        return [f"{label}:{exc.lineno}: not Python {FLOOR[0]}.{FLOOR[1]} syntax: {exc.msg}"]

    lazy = any(isinstance(n, ast.ImportFrom) and n.module == "__future__"
               and any(a.name == "annotations" for a in n.names) for n in tree.body)
    classes = {n.name for n in ast.walk(tree) if isinstance(n, ast.ClassDef)}

    annotation_nodes: set[int] = set()
    for node in ast.walk(tree):
        anns: list[ast.AST] = []
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            a = node.args
            anns += [arg.annotation for arg in
                     a.posonlyargs + a.args + a.kwonlyargs + [a.vararg, a.kwarg]
                     if arg is not None and arg.annotation is not None]
            if node.returns is not None:
                anns.append(node.returns)
        elif isinstance(node, ast.AnnAssign):
            anns.append(node.annotation)
        for ann in anns:
            annotation_nodes.update(id(m) for m in ast.walk(ann))

    out = []
    for node in ast.walk(tree):
        line = getattr(node, "lineno", 0)
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr):
            if id(node) in annotation_nodes:
                if not lazy:
                    out.append(f"{label}:{line}: PEP 604 annotation `{ast.unparse(node)}` "
                               "is evaluated on 3.9; add `from __future__ import annotations`")
            elif is_type_operand(node.left, classes) or is_type_operand(node.right, classes):
                out.append(f"{label}:{line}: runtime PEP 604 union `{ast.unparse(node)}` "
                           "fails on 3.9; use a tuple or typing.Optional/Union")
        elif isinstance(node, ast.Attribute) and node.attr in NEW_ATTRS:
            out.append(f"{label}:{line}: `.{node.attr}` needs Python 3.10+")
        elif isinstance(node, ast.alias) and node.name in NEW_ATTRS:
            out.append(f"{label}: imports `{node.name}`, which needs Python 3.10+")
        elif isinstance(node, ast.Call):
            func = node.func
            name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", None)
            bad = NEW_KWARGS.get(name, set()) & {k.arg for k in node.keywords}
            if bad:
                out.append(f"{label}:{line}: `{name}({', '.join(sorted(bad))}=...)` "
                           "needs Python 3.10+")
    return out


def check_detector() -> None:
    """The checker must still catch what broke athena-rocky, and not flag 3.9-safe code."""
    bad = {
        "eager_annotation": "def f(x: str | None) -> None: pass\n",
        "runtime_union": "from __future__ import annotations\nok = isinstance(1, int | str)\n",
        "alias_union": "from __future__ import annotations\nAlias = dict[str, int] | None\n",
        "match": "match x:\n    case 1: pass\n",
        "zip_strict": "zip(a, b, strict=True)\n",
        "write_text_newline": "p.write_text('x', newline='\\n')\n",
    }
    for label, src in bad.items():
        assert floor_violations(src, label), f"detector missed {label}"
    good = ("from __future__ import annotations\n"
            "import re\n"
            "class C: pass\n"
            "def f(x: str | None, y: C | None = None) -> list[int] | None: pass\n"
            "flags = re.I | re.M\n"
            "mask = 1 | 2\n")
    assert floor_violations(good, "good") == [], floor_violations(good, "good")


IMPORT_PROBE = r"""
import importlib.abc, sys
class Hide(importlib.abc.MetaPathFinder):
    def find_spec(self, name, path=None, target=None):
        if name in ("tomllib", "tomli"):
            raise ImportError("hidden: " + name)
        return None
sys.meta_path.insert(0, Hide())
sys.path.insert(0, sys.argv[1])
import compile_overlays, release_stage
assert compile_overlays.tomllib is None
print("imported")
"""


def check_imports(python: str) -> None:
    proc = subprocess.run(
        [python, "-c", IMPORT_PROBE, str(ROOT / "tools")],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    assert proc.returncode == 0 and "imported" in proc.stdout, (
        f"{python}: importing release_stage without tomllib/tomli failed "
        f"(rc={proc.returncode})\n{proc.stdout}{proc.stderr}")


def main() -> None:
    check_detector()

    # prepare_disc.py is not imported but spawned with sys.executable, so it
    # runs on the same host interpreter and shares the floor.
    closure: dict[Path, str] = {}
    for entry in ("psxrecomp_cli.py", "tools/prepare_disc.py"):
        closure.update(import_closure(ROOT / entry))
    names = {p.name for p in closure}
    for required in ("release_stage.py", "compile_overlays.py", "toolchain_pack.py",
                     "prepare_disc.py", "disc_companion.py"):
        assert required in names, f"import closure lost {required}: {sorted(names)}"

    violations = []
    for path, src in sorted(closure.items()):
        violations += floor_violations(src, path.relative_to(ROOT).as_posix())
    assert not violations, "Python 3.9 floor violations:\n  " + "\n  ".join(violations)

    check_imports(sys.executable)
    floor_python = os.environ.get("PSXRECOMP_FLOOR_PYTHON")
    if floor_python:
        check_imports(floor_python)

    print(f"cli python floor test: PASS ({len(closure)} modules)")


if __name__ == "__main__":
    main()
