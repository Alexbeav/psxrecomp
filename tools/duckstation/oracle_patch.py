#!/usr/bin/env python3
"""The oracle patch, kept without any line of the upstream source.

psxrecomp_oracle.patch adds our debug server to a pinned DuckStation checkout.
A normal unified diff carries context lines and removed lines, and those are
upstream's source. This repository must not hold them. So the patch is stored
with zero context lines:

  * it holds only the lines we add, and the line numbers where they go;
  * hunk headers carry no function name;
  * a change that removes or rewrites an upstream line is not in the patch. It
    is a "line edit" in pin.json: a path, a line number, the text to delete
    from that line, and the SHA-256 of the line before and after. The edit is
    made here, by line number, and only when the hash matches.

A zero-context patch applies by line number alone, so the base must be exact.
The patch's own "index" lines name the upstream blob of each file it changes;
apply() checks them against the checkout before it touches anything.

Commands:
  oracle_patch.py apply  <checkout>   apply the line edits and the patch (safe to repeat)
  oracle_patch.py status <checkout>   print "applied" or "not applied"
  oracle_patch.py regen  <checkout>   rebuild the patch from a patched checkout
  oracle_patch.py check               verify the stored patch holds no upstream line
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
PIN_PATH = HERE / "pin.json"

HUNK_RX = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
INDEX_RX = re.compile(r"^index ([0-9a-f]+)\.\.([0-9a-f]+)")


class PatchError(Exception):
    pass


def load_pin() -> Dict[str, Any]:
    return json.loads(PIN_PATH.read_text(encoding="utf-8"))


def patch_path(pin: Dict[str, Any]) -> Path:
    return HERE / pin["patch"]


def line_hash(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def git(checkout: Path, *args: str) -> Tuple[int, str]:
    proc = subprocess.run(["git", "-C", str(checkout), *args], capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    return proc.returncode, (proc.stdout + proc.stderr).strip()


# ---- the stored form ---------------------------------------------------------

def zero_context(diff_text: str) -> Tuple[str, List[Dict[str, Any]]]:
    """Rewrite a unified diff so that it holds added lines only.

    Returns the new text and the removals it had to leave out: one entry per
    removed line, with its path and its line number in the old file. Added
    lines that directly follow a removed line are its replacement; they are
    left out too. Each entry needs a line edit in pin.json. A file whose only
    change is such a rewrite drops out of the patch.
    """
    out: List[str] = []
    removed: List[Dict[str, Any]] = []
    header: List[str] = []
    hunks: List[str] = []
    path: Optional[str] = None
    old = new = 0
    run: List[str] = []
    run_old = run_new = 0
    rewriting = False

    def flush_run() -> None:
        nonlocal run
        if run:
            hunks.append(f"@@ -{run_old},0 +{run_new},{len(run)} @@")
            hunks.extend(run)
            run = []

    def flush_file() -> None:
        nonlocal header, hunks
        flush_run()
        if header and hunks:
            out.extend(header)
            out.extend(hunks)
        header, hunks = [], []

    in_hunk = False
    for line in diff_text.split("\n"):
        if line.startswith("diff --git "):
            flush_file()
            header = [line]
            path = None
            in_hunk = False
            continue
        match = HUNK_RX.match(line)
        if match:
            flush_run()
            rewriting = False
            old, new = int(match.group(1)), int(match.group(3))
            in_hunk = True
            continue
        if not in_hunk:
            if line.startswith("+++ "):
                path = None if line[4:] == "/dev/null" else line[4:][2:]
            if header and line:
                header.append(line)
            continue
        if line.startswith("+"):
            if rewriting:
                removed[-1]["replaced"] = True
            else:
                if not run:
                    run_old, run_new = max(old - 1, 0), new
                run.append(line)
            new += 1
        elif line.startswith("-"):
            flush_run()
            removed.append({"path": path, "line": old, "replaced": False})
            rewriting = True
            old += 1
        elif line.startswith(" "):
            flush_run()
            rewriting = False
            old += 1
            new += 1
        elif line.startswith("\\"):
            if run:
                run.append(line)
        else:
            flush_run()
            rewriting = False
            in_hunk = False
    flush_file()
    return "\n".join(out) + "\n", removed


def problems(patch_text: str) -> List[str]:
    """Why a patch text is not in the stored form. Empty when it is."""
    found: List[str] = []
    in_hunk = False
    for number, line in enumerate(patch_text.split("\n"), 1):
        if line.startswith("diff --git "):
            in_hunk = False
            continue
        match = HUNK_RX.match(line)
        if match:
            in_hunk = True
            if not line.rstrip().endswith("@@"):
                found.append(f"line {number}: text after the hunk header")
            if int(match.group(2) or "1") != 0:
                found.append(f"line {number}: the hunk takes lines from the old file")
            continue
        if not in_hunk or not line:
            continue
        if line.startswith("+") or line.startswith("\\"):
            continue
        kind = "context" if line.startswith(" ") else "removed" if line.startswith("-") else "unknown"
        found.append(f"line {number}: a {kind} line")
    return found


def changed_files(patch_text: str) -> List[Tuple[str, str]]:
    """(path, upstream blob id) of each existing file the patch changes."""
    files: List[Tuple[str, str]] = []
    path: Optional[str] = None
    new_file = False
    blob: Optional[str] = None
    for line in patch_text.split("\n"):
        if line.startswith("diff --git "):
            path, new_file, blob = line.split(" b/", 1)[1], False, None
        elif line.startswith("new file mode"):
            new_file = True
        elif INDEX_RX.match(line) and path:
            blob = INDEX_RX.match(line).group(1)
            if not new_file:
                files.append((path, blob))
    return files


# ---- line edits --------------------------------------------------------------

def edit_state(checkout: Path, edit: Dict[str, Any]) -> str:
    """'before', 'after' or 'other' for one line edit."""
    target = checkout / edit["path"]
    if not target.is_file():
        return "other"
    lines = target.read_text(encoding="utf-8").split("\n")
    index = int(edit["line"]) - 1
    if index >= len(lines):
        return "other"
    digest = line_hash(lines[index].rstrip("\r"))
    if digest == edit["sha256_before"]:
        return "before"
    if digest == edit["sha256_after"]:
        return "after"
    return "other"


def apply_edit(checkout: Path, edit: Dict[str, Any]) -> None:
    target = checkout / edit["path"]
    raw = target.read_bytes().decode("utf-8")
    lines = raw.split("\n")
    index = int(edit["line"]) - 1
    tail = "\r" if lines[index].endswith("\r") else ""
    body = lines[index].rstrip("\r")
    if edit["remove_text"] not in body:
        raise PatchError(f"{edit['path']} line {edit['line']}: the text to remove is not there")
    body = body.replace(edit["remove_text"], "", 1)
    if line_hash(body) != edit["sha256_after"]:
        raise PatchError(f"{edit['path']} line {edit['line']}: the edited line is not the expected one")
    lines[index] = body + tail
    # A file that git rewrites under core.autocrlf=true comes out with CRLF line
    # endings, and that is what applying a patch to this file used to do. Write
    # the same, so the patched tree is byte for byte what it was before.
    _, autocrlf = git(checkout, "config", "--get", "core.autocrlf")
    if autocrlf.strip().lower() == "true" and "\r\n" not in raw:
        lines = [line + "\r" for line in lines[:-1]] + lines[-1:]
    target.write_bytes("\n".join(lines).encode("utf-8"))


# ---- apply and status ----------------------------------------------------------

def is_applied(checkout: Path, pin: Dict[str, Any]) -> bool:
    rc, _ = git(checkout, "apply", "--reverse", "--check", "--unidiff-zero", str(patch_path(pin)))
    if rc != 0:
        return False
    return all(edit_state(checkout, edit) == "after" for edit in pin.get("line_edits", []))


def apply(checkout: Path, pin: Dict[str, Any], verify_base: bool = True) -> str:
    """Apply the line edits and the patch. Returns 'applied' or 'already applied'."""
    patch = patch_path(pin)
    if not patch.is_file():
        raise PatchError(f"missing oracle patch: {patch}")
    text = patch.read_text(encoding="utf-8")
    bad = problems(text)
    if bad:
        raise PatchError("the oracle patch is not in zero-context form: " + "; ".join(bad[:5]))
    if is_applied(checkout, pin):
        return "already applied"

    reset_hint = "Reset the checkout to the pinned base and run setup again."
    if verify_base:
        for path, blob in changed_files(text):
            rc, head = git(checkout, "rev-parse", f"HEAD:{path}")
            if rc != 0 or not head.startswith(blob):
                raise PatchError(f"{path} is not the pinned upstream file (expected blob {blob}). "
                                 "The pinned base moved, or the patch was made against another one.")
            rc, _ = git(checkout, "diff", "--quiet", "HEAD", "--", path)
            if rc != 0:
                raise PatchError(f"{path} is changed in the checkout but the patch is not applied. " + reset_hint)
    for edit in pin.get("line_edits", []):
        if verify_base and edit.get("upstream_blob"):
            rc, head = git(checkout, "rev-parse", f"HEAD:{edit['path']}")
            if rc != 0 or not head.startswith(edit["upstream_blob"]):
                raise PatchError(f"{edit['path']} is not the pinned upstream file "
                                 f"(expected blob {edit['upstream_blob']}).")
        if edit_state(checkout, edit) == "other":
            raise PatchError(f"{edit['path']} line {edit['line']} is neither the upstream line nor "
                             "the edited one. " + reset_hint)
    rc, out = git(checkout, "apply", "--check", "--unidiff-zero", str(patch))
    if rc != 0:
        raise PatchError(f"the oracle patch does not apply to this tree.\n  patch: {patch}\n  git says: {out}")

    for edit in pin.get("line_edits", []):
        if edit_state(checkout, edit) == "before":
            apply_edit(checkout, edit)
    rc, out = git(checkout, "apply", "--unidiff-zero", "--whitespace=nowarn", str(patch))
    if rc != 0:
        raise PatchError(f"git apply failed: {out}")
    return "applied"


def regen(checkout: Path, pin: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Rebuild the stored patch from a patched checkout of the pinned base."""
    rc, _ = git(checkout, "add", "--intent-to-add", "--all")
    rc, diff = git(checkout, "diff", "--no-color", pin["upstream_base"])
    if rc != 0:
        raise PatchError(f"git diff failed: {diff}")
    text, removed = zero_context(diff + "\n")
    patch_path(pin).write_text(text, encoding="utf-8", newline="\n")
    return removed


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", choices=["apply", "status", "regen", "check"])
    parser.add_argument("checkout", nargs="?", type=Path)
    args = parser.parse_args(argv)
    pin = load_pin()
    try:
        if args.command == "check":
            bad = problems(patch_path(pin).read_text(encoding="utf-8"))
            for item in bad:
                print(item)
            print("oracle patch: zero-context form" if not bad else f"oracle patch: {len(bad)} problems")
            return 1 if bad else 0
        if args.checkout is None:
            parser.error("this command needs the checkout path")
        if args.command == "apply":
            print(f"oracle patch: {apply(args.checkout, pin)}")
        elif args.command == "status":
            print("applied" if is_applied(args.checkout, pin) else "not applied")
        else:
            for item in regen(args.checkout, pin):
                print(f"needs a line edit in pin.json: {item['path']} line {item['line']}")
            print(f"wrote {patch_path(pin)}")
    except PatchError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
