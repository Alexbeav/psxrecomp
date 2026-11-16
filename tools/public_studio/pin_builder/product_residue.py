"""What a product folder holds before its test starts, and how to take out what the starts wrote.

A build starts the product it has just made, and a start writes into the
folder it runs from: memory cards, disc digests, the mod state, reports,
captures, shard sources, native cache units. Found 2026-10-02: the Linux
archive was packed after its starts and carried their memory cards and
mods/state.toml to whoever unpacked it.

snapshot() is taken before the starts and clean() after them. clean() removes
every file the starts added, except the native cache pairs: those are kept on
purpose, because a player with no compiler gets compiled overlay code only
from a shipped cache. A file that was there before and is different or gone
afterwards is reported, not repaired: the caller stops.

Standard library only, Python 3.9: linux_build ships this file to the Unix
build host and runs it with the system Python.
"""
import hashlib
import os
import re

# A compiled overlay unit and its range table, as the runtime names them.
NATIVE_PAIR = re.compile(r"[0-9A-Fa-f]{8}_[0-9A-Fa-f]{8}\.(so|dylib|dll|ranges)$")
CACHE = "cache"
# The player's disc and BIOS copies: large, and a start only reads them. Their
# size is recorded, not their hash.
SIZE_ONLY = ("inputs",)


def _mark(path, relative):
    if relative.split("/", 1)[0] in SIZE_ONLY:
        return "size:%d" % os.path.getsize(path)
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _walk(product):
    """(files, folders) of `product` as '/'-separated relative paths. Links are listed, never followed."""
    files, folders = [], []
    for current, names, leaves in os.walk(product):
        base = os.path.relpath(current, product)
        prefix = "" if base == "." else base.replace(os.sep, "/") + "/"
        for name in list(names):
            if os.path.islink(os.path.join(current, name)):
                names.remove(name)
                files.append(prefix + name)
            else:
                folders.append(prefix + name)
        files.extend(prefix + name for name in leaves)
    return sorted(files), sorted(folders)


def snapshot(product):
    """What `product` holds now: {"files": {path: mark}, "folders": [path]}."""
    product = str(product)
    files, folders = _walk(product)
    marks = {}
    for relative in files:
        path = os.path.join(product, *relative.split("/"))
        marks[relative] = "link" if os.path.islink(path) else _mark(path, relative)
    return {"files": marks, "folders": folders}


def drop_bytecode(product):
    """Remove every __pycache__ folder of `product`; return how many were removed.

    Call it before snapshot(). The bundled interpreter writes compiled Python
    files beside its sources when it runs. The build host runs it while it
    stages the product, so a few such files exist before the test starts, and
    the starts rewrite them: the first real build with clean() stopped on three
    "changed" files of that kind (Galerians, 2026-10-02). They are a cache of
    the machine that made them. A product ships none; the player's interpreter
    writes its own.
    """
    removed = 0
    for current, names, _leaves in os.walk(str(product)):
        for name in list(names):
            if name == "__pycache__" and not os.path.islink(os.path.join(current, name)):
                folder = os.path.join(current, name)
                for inner, _dirs, files in os.walk(folder, topdown=False):
                    for leaf in files:
                        os.remove(os.path.join(inner, leaf))
                    os.rmdir(inner)
                names.remove(name)
                removed += 1
    return removed


def is_native_pair(relative):
    parts = relative.split("/")
    return parts[0] == CACHE and len(parts) > 1 and bool(NATIVE_PAIR.match(parts[-1]))


def changed(product, before):
    """Files that were in `product` at `before` and are different or gone now, as "path (changed)" / "path (gone)"."""
    product = str(product)
    present = set(_walk(product)[0])
    found = []
    for relative, mark in sorted(before["files"].items()):
        path = os.path.join(product, *relative.split("/"))
        if relative not in present:
            found.append(relative + " (gone)")
        elif mark != ("link" if os.path.islink(path) else _mark(path, relative)):
            found.append(relative + " (changed)")
    return found


def clean(product, before):
    """Take out of `product` what was written since `before`; return what was done.

      removed     files the starts added and the product does not keep
      kept_cache  native cache pairs the starts added (kept on purpose)
      changed     files that were there before and are different or gone now
    """
    product = str(product)
    files, _folders = _walk(product)
    removed, kept = [], []
    for relative in files:
        if relative in before["files"]:
            continue
        if is_native_pair(relative):
            kept.append(relative)
            continue
        os.remove(os.path.join(product, *relative.split("/")))
        removed.append(relative)
    different = changed(product, before)
    # Folders the starts made and that are empty now, deepest first.
    known = set(before["folders"])
    for relative in sorted(_walk(product)[1], key=lambda name: name.count("/"), reverse=True):
        path = os.path.join(product, *relative.split("/"))
        if relative not in known and not os.listdir(path):
            os.rmdir(path)
    return {"removed": removed, "kept_cache": kept, "changed": different}


def unexpected(product, before):
    """Files of `product` that are neither in `before` nor a native cache pair: [] after a clean()."""
    files, _folders = _walk(str(product))
    return [relative for relative in files if relative not in before["files"] and not is_native_pair(relative)]
