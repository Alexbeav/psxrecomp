#!/usr/bin/env python3
"""A set whose discs boot different programs, set up as one folder (PS1B-333).

Resident Evil 2's two discs boot different programs. One build links one
program, so such a set is one build per program. The player still gets one
folder: one executable per program, one `saves` folder, one `settings.toml`.

A set package is a root that holds:

    set.toml                the set: its discs, its programs
    programs/<program>/     one unchanged single-program project per program
    psxrecomp/ recomp-ui/   one framework tree and one launcher tree
    CMakeLists.txt, codegen_setup.c/.h    the set's setup host (init-host)

`psxrecomp_cli.py generate` and `rebuild` take the set root when their config
has a `[set]` table. They run the ordinary single-program step once per program
and then call `join_products` here. This module holds everything that is about
the set and nothing that builds: the set file, the disc check, the folder links
that let each program see the shared framework, the join, the start scripts
and the host project.

The join has the rules of Workbench Studio's private set build, so a set that
setup installs on the player's machine and a set that Studio builds have the
same layout. Studio calls `join` from the pinned tree for that reason.

Runs on Python 3.9 (the CLI's floor): no tomllib, no match statements.

    python program_set.py check     --set set.toml
    python program_set.py folders   --set set.toml
    python program_set.py init-host --root <set root>
    python program_set.py join      --set set.toml --program leon=<folder>
                                    --program claire=<folder> --out <folder>
                                    [--set-disc N=<path>]... [--record <file>]
    python program_set.py kit-check <kit folder> <kit folder> [--json]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

HERE = Path(__file__).resolve().parent
LAYOUT = HERE / "new_project_layout"
TEMPLATES = LAYOUT / "templates" / "set_host"

SET_FILE = "set.toml"
SET_MARKER = "program_set.generated"
INSTALL_RECORD = "program-set-install.json"
SHARED_TREES = ("psxrecomp", "recomp-ui")
DOWNLOAD_CACHE = ".cache/overlay-toolchain"   # where release_stage.py keeps the tcc and Python archives
HOST_FILES = ("CMakeLists.txt", "codegen_setup.c", "codegen_setup.h")
HOST_STAMP = "Written by psxrecomp/tools/program_set.py init-host"

# What a program's folder contributes to the set's folder. A program's folder
# is either a product Studio already cleaned, or the build folder setup made on
# the player's machine, which also holds CMake's leftovers. Only these names
# are taken, so both give the same result.
PAYLOAD_FILES = ("game_options.toml", "bios.cfg", "overlay_codegen_hash.h", "psx_game_version.txt")
PAYLOAD_DIRS = ("assets", "mods", "overlay_toolchain", "licenses", "cache", "inputs")
# Never taken: player state under mods/, and tcc's example programs (five .c
# files nothing reads; a product holds no source files).
LEFT_OUT = ("mods/state.toml", "mods/state.toml.tmp", "mods/installed", "overlay_toolchain/tcc/examples")
# Same path in every program's folder, different bytes for a known, harmless
# reason. The first program's copy is kept and every hash is recorded.
#   overlay_toolchain/psxrecomp-game*   link timestamps differ per build
#   mods/README.md                      names the program
KNOWN_VARIANTS = ("overlay_toolchain/psxrecomp-game.exe", "overlay_toolchain/psxrecomp-game",
                  "mods/README.md")
# Each program's folder carries these for itself. They are never copied across:
# the join writes the per-program config, and Studio writes its own provenance.
PER_PROGRAM = ("game.toml", "BUILDINFO.json", "SHA256SUMS.txt", "disc.cfg", "settings.toml")
# Line-oriented caches every program appends to: the union is kept.
LINE_UNIONS = ("saves/disc_digests.tsv",)


class SetError(Exception):
    """A plain sentence for the player or the operator; nothing was half-done."""


class WrongDisc(SetError):
    """A located disc is not the disc its position in the set calls for.

    `number` is that position, 1-based (0 when it is not known), so that the
    caller can say which file was refused.
    """

    def __init__(self, message: str, number: int = 0) -> None:
        super().__init__(message)
        self.number = number


# ---- set.toml ---------------------------------------------------------------


def _strip_comment(line: str) -> str:
    out, quote, i = [], "", 0
    while i < len(line):
        c = line[i]
        if quote:
            out.append(c)
            if c == "\\" and quote == '"' and i + 1 < len(line):
                out.append(line[i + 1])
                i += 1
            elif c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
            out.append(c)
        elif c == "#":
            break
        else:
            out.append(c)
        i += 1
    return "".join(out).strip()


def _scalar(token: str) -> Any:
    token = token.strip()
    if len(token) >= 2 and token[0] == '"' and token[-1] == '"':
        return json.loads(token)  # TOML basic strings use JSON's escapes
    if len(token) >= 2 and token[0] == "'" and token[-1] == "'":
        return token[1:-1]
    if token in ("true", "false"):
        return token == "true"
    try:
        return int(token, 0)
    except ValueError:
        raise SetError(f"set.toml: cannot read the value {token!r}") from None


def _array_items(body: str) -> List[Any]:
    items, cur, quote, i = [], [], "", 0
    while i < len(body):
        c = body[i]
        if quote:
            cur.append(c)
            if c == "\\" and quote == '"' and i + 1 < len(body):
                cur.append(body[i + 1])
                i += 1
            elif c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c == ",":
            if "".join(cur).strip():
                items.append(_scalar("".join(cur)))
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        items.append(_scalar("".join(cur)))
    return items


def _array_closed(text: str) -> bool:
    quote, i = "", 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\" and quote == '"':
                i += 1
            elif c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
        elif c == "]":
            return True
        i += 1
    return False


def parse_toml_subset(text: str) -> Dict[str, Dict[str, Any]]:
    """Tables, strings, integers, booleans and flat arrays: what set.toml and a
    game.toml's [game] table use. Table names keep their dots (`program.leon`)."""
    tables: Dict[str, Dict[str, Any]] = {"": {}}
    cur, pending_key, pending = "", None, ""
    for raw in text.splitlines():
        line = _strip_comment(raw)
        if pending_key is not None:
            pending += " " + line
            if _array_closed(pending):
                tables[cur][pending_key] = _array_items(pending[1:pending.rindex("]")])
                pending_key, pending = None, ""
            continue
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = line.strip("[]").strip()
            tables.setdefault(cur, {})
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if value.startswith("["):
            if _array_closed(value):
                tables[cur][key] = _array_items(value[1:value.rindex("]")])
            else:
                pending_key, pending = key, value
            continue
        try:
            tables[cur][key] = _scalar(value)
        except SetError:
            tables[cur][key] = value  # a kind of value this reader does not need
    return tables


def is_set_config(path: Path) -> bool:
    """True when the file declares a set: it has a `[set]` table."""
    try:
        text = Path(path).read_text(encoding="utf-8-sig")
    except OSError:
        return False
    return any(_strip_comment(line) == "[set]" for line in text.splitlines())


def _strings(value: Any, what: str) -> List[str]:
    if not isinstance(value, list) or not value or not all(isinstance(v, str) and v for v in value):
        raise SetError(f"set.toml: {what} must be a list of names")
    return list(value)


def load_set(path: Path) -> Dict[str, Any]:
    """Read and check set.toml. Raises SetError with a plain sentence."""
    path = Path(path).resolve()
    try:
        tables = parse_toml_subset(path.read_text(encoding="utf-8-sig"))
    except OSError as exc:
        raise SetError(f"cannot read {path}: {exc}") from None
    head = tables.get("set")
    if head is None:
        raise SetError(f"{path.name} has no [set] table")
    for key in ("name", "title", "exe_name"):
        if not isinstance(head.get(key), str) or not head[key]:
            raise SetError(f"set.toml: [set] {key} is missing")
    serials = _strings(head.get("serials"), "[set] serials")
    names = _strings(head.get("programs"), "[set] programs")
    if len(names) < 2 or len(set(names)) != len(names):
        raise SetError("set.toml: a set has two or more programs, each named once")
    game = tables.get("game") or {}
    discs = game.get("discs")
    if not isinstance(discs, list) or len(discs) != len(serials):
        raise SetError("set.toml: [game] discs must hold one entry per serial of the set, in set order")
    if game.get("disc_serials") != serials:
        raise SetError("set.toml: [game] disc_serials must equal [set] serials")
    programs, owned = [], {}
    for name in names:
        table = tables.get(f"program.{name}")
        if table is None:
            raise SetError(f"set.toml: [program.{name}] is missing")
        for key in ("folder", "exe_name", "shortcut"):
            if not isinstance(table.get(key), str) or not table[key]:
                raise SetError(f"set.toml: [program.{name}] {key} is missing")
        folder = table["folder"].replace("\\", "/").strip("/")
        if not folder or folder.startswith("..") or "/../" in f"/{folder}/" or Path(folder).is_absolute():
            raise SetError(f"set.toml: [program.{name}] folder must be a folder inside the set")
        positions = table.get("positions")
        own = _strings(table.get("serials"), f"[program.{name}] serials")
        if (not isinstance(positions, list) or len(positions) != len(own)
                or not all(isinstance(p, int) and 1 <= p <= len(serials) for p in positions)):
            raise SetError(f"set.toml: [program.{name}] positions must name the program's discs, 1-based")
        for position, serial in zip(positions, own):
            if serials[position - 1] != serial:
                raise SetError(f"set.toml: [program.{name}] says disc {position} is {serial}, "
                               f"but the set says {serials[position - 1]}")
            if position in owned:
                raise SetError(f"set.toml: disc {position} belongs to both {owned[position]} and {name}")
            owned[position] = name
        programs.append({"program": name, "folder": folder, "exe_name": table["exe_name"],
                         "shortcut": table["shortcut"], "serials": own, "positions": list(positions)})
    if len(owned) != len(serials):
        missing = [str(n) for n in range(1, len(serials) + 1) if n not in owned]
        raise SetError("set.toml: no program boots disc " + ", ".join(missing))
    if len({p["exe_name"] for p in programs} | {head["exe_name"]}) != len(programs) + 1:
        raise SetError("set.toml: the set and each program need different exe names")
    return {"path": path, "root": path.parent, "name": head["name"], "title": head["title"],
            "exe_name": head["exe_name"], "bios_stem": str(head.get("bios_stem") or ""),
            "serials": serials, "discs": [str(d) for d in discs], "programs": programs}


def program_of_disc(spec: Dict[str, Any], position: int) -> Dict[str, Any]:
    return next(p for p in spec["programs"] if position in p["positions"])


# ---- the discs of the set ---------------------------------------------------


def parse_set_disc_args(values: Optional[List[str]]) -> Dict[int, str]:
    """`--set-disc N=path`, N is the 1-based position in the set."""
    out: Dict[int, str] = {}
    for value in values or []:
        number, sep, path = value.partition("=")
        if not sep or not number.strip().isdigit() or not path.strip():
            raise SetError(f"--set-disc takes N=PATH, not {value!r}")
        out[int(number)] = path.strip()
    return out


def read_disc_cfg(root: Path) -> List[str]:
    """The wizard's record of the located discs: line N is disc N."""
    try:
        return [line.strip() for line in (Path(root) / "disc.cfg").read_text(encoding="utf-8-sig").splitlines()]
    except OSError:
        return []


def read_marker(root: Path, name: str = SET_MARKER) -> Dict[str, Any]:
    """What the last complete generate recorded, or {} when there is none."""
    try:
        data = json.loads((Path(root) / "generated" / name).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def located_discs(spec: Dict[str, Any], given: Optional[Dict[int, str]] = None) -> List[Optional[Path]]:
    """Where each disc of the set is, by set position; None for one not located.

    In this order: the command line, set.toml (the wizard writes the located
    set there), the wizard's disc.cfg, and what the last generate recorded. A
    path that is not a file is not a disc.
    """
    root = Path(spec["root"])
    cfg = read_disc_cfg(root)
    recorded = read_marker(root).get("discs")
    recorded = recorded if isinstance(recorded, list) else []
    found: List[Optional[Path]] = []
    for index, declared in enumerate(spec["discs"]):
        candidates = [(given or {}).get(index + 1), declared, cfg[index] if index < len(cfg) else None,
                      recorded[index] if index < len(recorded) else None]
        chosen = None
        for candidate in candidates:
            if not candidate:
                continue
            path = Path(candidate).expanduser()
            if not path.is_absolute():
                path = root / path
            if path.is_file():
                chosen = path.resolve()
                break
        found.append(chosen)
    return found


def probe_serial(path: Path) -> str:
    """The serial of the disc's boot executable, or "" when it has none."""
    for folder in (str(HERE), str(LAYOUT)):
        if folder not in sys.path:
            sys.path.insert(0, folder)
    import probe_disc  # noqa: E402
    return probe_disc.probe(Path(path), identity_only=True).serial or ""


def check_discs(spec: Dict[str, Any], discs: List[Optional[Path]],
                probe: Optional[Callable[[Path], str]] = None) -> None:
    """Stop unless every disc of the set is located and is the right disc.

    One build per program needs every program's disc, so a set cannot be set up
    from a part of it. The message names the disc the player has to find.
    """
    probe = probe or probe_serial
    for index, serial in enumerate(spec["serials"]):
        number = index + 1
        program = program_of_disc(spec, number)["program"]
        disc = discs[index] if index < len(discs) else None
        if disc is None:
            raise SetError(f"Disc {number} of {spec['title']} is not located. It is {serial}, the disc of the "
                           f"{program} program. Setup builds every program of this game, so it needs every "
                           "disc: select it, then run Generate again.")
        try:
            found = probe(disc)
        except Exception as exc:  # noqa: BLE001 - whatever the reader raises, the disc is not usable
            raise WrongDisc(f"Disc {number} of {spec['title']} cannot be read: {Path(disc).name} ({exc}). "
                            f"It must be {serial}, the disc of the {program} program.", number) from None
        if found != serial:
            raise WrongDisc(f"Disc {number} of {spec['title']} must be {serial}, the disc of the {program} "
                            f"program, but {Path(disc).name} is "
                            f"{found or 'not a disc this setup can identify'}.", number)


# ---- each program sees the shared framework ---------------------------------


def _same_folder(a: Path, b: Path) -> bool:
    try:
        return os.path.samefile(str(a), str(b))
    except OSError:
        return False


def _remove_link(link: Path) -> None:
    """Remove a folder link without touching what it points at."""
    try:
        os.unlink(str(link))          # a symlink
    except OSError:
        os.rmdir(str(link))           # a Windows junction is removed like an empty folder


def _is_link(path: Path) -> bool:
    if os.path.islink(str(path)):
        return True
    if os.name == "nt":
        try:
            return bool(os.lstat(str(path)).st_file_attributes & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT
        except (OSError, AttributeError):
            return False
    return False


def link_folder(link: Path, target: Path) -> str:
    """Make `link` reach the folder `target`. Returns how: "present", "own",
    "symlink", "junction" or "copy".

    A program's project expects `psxrecomp/` and `recomp-ui/` beside its
    CMakeLists.txt, as in a single package. In a set there is one of each at the
    root. A relative symlink survives a moved folder; a Windows junction holds
    an absolute path, so a stale one is replaced here on the next run. A drive
    that cannot hold links gets a copy.
    """
    link, target = Path(link), Path(target)
    if not target.is_dir():
        raise SetError(f"the set has no {target.name} folder at {target}")
    if _is_link(link):
        if _same_folder(link, target):
            return "present"
        _remove_link(link)            # points at an old place: the folder was moved
    elif link.exists():
        return "own"                  # the program brought its own tree; leave it
    link.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        try:
            import _winapi
            _winapi.CreateJunction(str(target), str(link))
            return "junction"
        except Exception:  # noqa: BLE001 - try the shell, then a copy
            done = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                                  capture_output=True, text=True, encoding="utf-8", errors="replace")
            if done.returncode == 0 and link.is_dir():
                return "junction"
    else:
        try:
            os.symlink(os.path.relpath(str(target), str(link.parent)), str(link), target_is_directory=True)
            return "symlink"
        except OSError:
            pass
    shutil.copytree(str(target), str(link), ignore=shutil.ignore_patterns(".git", "__pycache__"))
    return "copy"


def prepare_program_folder(spec: Dict[str, Any], program: Dict[str, Any]) -> Dict[str, str]:
    """Give one program's folder the set's framework and launcher trees."""
    root = Path(spec["root"])
    folder = root / program["folder"]
    if not (folder / "game.toml").is_file():
        raise SetError(f"the {program['program']} program has no game.toml in {folder}")
    made = {name: link_folder(folder / name, root / name) for name in SHARED_TREES}
    # One download cache for the overlay compiler archives: the first program's
    # build fetches them (or the packager's operator put them there), the
    # others reuse them.
    shared = root / DOWNLOAD_CACHE
    shared.mkdir(parents=True, exist_ok=True)
    made[DOWNLOAD_CACHE] = link_folder(folder / DOWNLOAD_CACHE, shared)
    return made


# ---- the join ---------------------------------------------------------------


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def find_executable(folder: Path, exe_name: str) -> Path:
    for name in (exe_name + ".exe", exe_name):
        if (Path(folder) / name).is_file():
            return Path(folder) / name
    raise SetError(f"{exe_name} was not built: it is not in {folder}")


def payload_files(folder: Path) -> List[str]:
    """The files of one program's folder that go into the set's folder, as
    sorted relative paths. The executable and the config are handled apart."""
    folder = Path(folder)
    found = [name for name in PAYLOAD_FILES if (folder / name).is_file()]
    found += [p.name for p in folder.glob("*.dll") if p.is_file()]
    for top in PAYLOAD_DIRS:
        base = folder / top
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            relative = path.relative_to(folder).as_posix()
            if path.is_file() and not any(relative == skip or relative.startswith(skip + "/") for skip in LEFT_OUT):
                found.append(relative)
    found += [name for name in LINE_UNIONS if (folder / name).is_file()]
    return sorted(set(found))


def not_taken(folder: Path, exe_file: str) -> List[str]:
    """Top-level names of a program's folder that the join neither takes nor
    rewrites. Empty for a product Studio cleaned; CMake's leftovers for a build
    folder. Studio stops on a non-empty list: a product file outside the rule
    would otherwise be dropped without a word."""
    taken = set(PAYLOAD_FILES) | set(PAYLOAD_DIRS) | set(PER_PROGRAM) | {exe_file, "saves"}
    return sorted(p.name for p in Path(folder).iterdir()
                  if p.name not in taken and not (p.is_file() and p.suffix.lower() == ".dll"))


def _surgeon(text: str):
    if str(LAYOUT) not in sys.path:
        sys.path.insert(0, str(LAYOUT))
    import update_disc_set  # noqa: E402
    return update_disc_set, update_disc_set.TomlSurgeon(text)


def program_config_text(text: str, set_discs: List[str], set_serials: List[str], positions: List[int]) -> str:
    """A program's game.toml turned into its config in the set's folder: the
    whole set in `discs` and `disc_serials`, the program's own discs in
    `program_discs`. Every other line is kept as it is."""
    tools, surgeon = _surgeon(text)
    lo, _ = surgeon._section_bounds("game")
    if lo < 0:
        raise SetError("the program's game.toml has no [game] table")
    surgeon.drop("game", "disc")
    surgeon.drop("game", "program_discs")
    surgeon.replace("game", "discs", tools.render_array("discs", list(set_discs)), anchor_after="exe")
    surgeon.replace("game", "disc_serials", tools.render_array("disc_serials", list(set_serials)),
                    anchor_after="discs")
    surgeon.replace("game", "program_discs",
                    ["program_discs = [" + ", ".join(str(int(p)) for p in positions) + "]"],
                    anchor_after="disc_serials")
    out = surgeon.text()
    game = parse_toml_subset(out).get("game") or {}
    if (game.get("discs") != list(set_discs) or game.get("disc_serials") != list(set_serials)
            or game.get("program_discs") != [int(p) for p in positions]):
        raise SetError("cannot write the set into the program's config; review its game.toml")
    return out


def own_disc_paths(folder: Path) -> Dict[str, str]:
    """serial -> path, from a product's own game.toml (a product Studio built
    already names the disc it was built from)."""
    config = Path(folder) / "game.toml"
    try:
        game = parse_toml_subset(config.read_text(encoding="utf-8-sig")).get("game") or {}
    except OSError:
        return {}
    paths = game.get("discs") or ([game["disc"]] if game.get("disc") else [])
    serials = game.get("disc_serials") or ([game["id"]] if game.get("id") and len(paths) == 1 else [])
    return {str(serial): str(path) for path, serial in zip(paths, serials)}


def shared_settings(text: str) -> str:
    """settings.toml for the set's folder: every table except [disc]. The
    programs share the file, and a remembered disc would be one program's."""
    out, skipping = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            skipping = stripped == "[disc]"
        if not skipping:
            out.append(line)
    return "\n".join(out) + "\n"


class Disagreement(SetError):
    """The programs carry one path with different bytes. `files` lists every
    such path: {"path", "sha256": {program: digest}}."""

    def __init__(self, files: List[Dict[str, Any]]) -> None:
        self.files = files
        named = "; ".join(f"{row['path']} ({' and '.join(row['sha256'])})" for row in files)
        what = named if len(files) == 1 else f"{len(files)} files: {named}"
        super().__init__(f"The programs of this set disagree about {what}. One folder can hold one copy. "
                         "Make the kits agree, or add the file to the known per-build list if the difference "
                         "is harmless.")


def disagreements(spec: Dict[str, Any], folders: Dict[str, Path]) -> List[Dict[str, Any]]:
    """Every path two or more programs carry with different bytes, except the
    known per-build files and the line-union files. Sorted by path; each row
    names every program that carries the path and its hash."""
    carriers: Dict[str, Dict[str, str]] = {}
    for program in spec["programs"]:
        name = program["program"]
        folder = Path(folders[name])
        for relative in payload_files(folder):
            if relative in LINE_UNIONS or relative in KNOWN_VARIANTS:
                continue
            carriers.setdefault(relative, {})[name] = sha256(folder / relative)
    return [{"path": relative, "sha256": hashes} for relative, hashes in sorted(carriers.items())
            if len(set(hashes.values())) > 1]


def join_products(spec: Dict[str, Any], products: Dict[str, Path], out: Path, *,
                  discs: Optional[List[Any]] = None,
                  log: Callable[[str], None] = lambda message: None) -> Dict[str, Any]:
    """Join the programs' folders into the set's folder `out`; return the record.

    `products` maps each program to its built folder. `discs`, by set position,
    is where the player's discs are; without it each product's own config says
    where its disc is. Files two programs carry must be byte-identical, except
    the known per-build ones.

    The first program's executable is written last, by rename. Setup starts the
    set only when that file exists, so a join that stops part-way never looks
    finished. `out` may already hold the player's saves and settings: nothing
    there is removed.
    """
    out = Path(out)
    missing = [p["program"] for p in spec["programs"] if p["program"] not in products]
    if missing:
        raise SetError("no built folder was given for: " + ", ".join(missing))
    folders = {name: Path(folder) for name, folder in products.items()}
    exes = {p["program"]: find_executable(folders[p["program"]], p["exe_name"]) for p in spec["programs"]}

    set_discs: List[Optional[str]] = [None] * len(spec["serials"])
    for index in range(len(set_discs)):
        if discs is not None and index < len(discs) and discs[index]:
            set_discs[index] = str(discs[index])
    for program in spec["programs"]:
        for serial, path in own_disc_paths(folders[program["program"]]).items():
            if serial in program["serials"]:
                index = spec["serials"].index(serial)
                if set_discs[index] is None:
                    set_discs[index] = path
    if any(path is None for path in set_discs):
        unnamed = [s for s, p in zip(spec["serials"], set_discs) if p is None]
        raise SetError("no disc path is known for: " + ", ".join(unnamed))

    # Every disagreement is found before anything is written, and all of them
    # are reported in one stop: a kit author who has to rebuild both programs
    # to see the next file pays one build per file.
    differing = disagreements(spec, folders)
    if differing:
        raise Disagreement(differing)

    out.mkdir(parents=True, exist_ok=True)
    first = spec["programs"][0]["program"]
    record: Dict[str, Any] = {"known_variants": {}, "line_unions": [], "shared_files": 0, "not_taken": {}}
    for program in spec["programs"]:
        name = program["program"]
        record["not_taken"][name] = not_taken(folders[name], exes[name].name)
    written: Dict[str, str] = {}      # relative path -> sha256, for files this join wrote
    for program in spec["programs"]:
        name, folder = program["program"], folders[program["program"]]
        for relative in payload_files(folder):
            source, target = folder / relative, out / relative
            if relative in LINE_UNIONS:
                lines = target.read_text(encoding="utf-8").splitlines() if target.is_file() else []
                extra = [line for line in source.read_text(encoding="utf-8").splitlines() if line not in lines]
                if extra or not target.is_file():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(("\n".join(lines + extra) + "\n").encode("utf-8"))
                # Counted as the private set build counts it: the first program's
                # copy is one shared file; a union is recorded once a second
                # program carries the file too.
                if relative not in written:
                    written[relative] = ""
                    record["shared_files"] += 1
                elif relative not in record["line_unions"]:
                    record["line_unions"].append(relative)
                continue
            digest = sha256(source)
            if relative not in written:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(str(source), str(target))
                written[relative] = digest
                record["shared_files"] += 1
                continue
            if written[relative] == digest:
                continue
            if relative in KNOWN_VARIANTS:
                entry = record["known_variants"].setdefault(
                    relative, {"kept": first, "sha256": {first: written[relative]}})
                entry["sha256"][name] = digest
                continue
            # Not reachable while the check above runs first; kept so a file
            # that changes during the join still stops it.
            raise Disagreement([{"path": relative, "sha256": {first: written[relative], name: digest}}])

    per_program = []
    for program in spec["programs"]:
        name, folder = program["program"], folders[program["program"]]
        config_source = folder / "game.toml"
        if not config_source.is_file():
            raise SetError(f"the {name} program's folder has no game.toml: {folder}")
        config = out / (program["exe_name"] + ".game.toml")
        config.write_bytes(program_config_text(config_source.read_text(encoding="utf-8-sig"),
                                               [str(p) for p in set_discs], spec["serials"],
                                               program["positions"]).encode("utf-8"))
        per_program.append({"program": name, "config": config.name, "serials": program["serials"],
                            "program_discs": program["positions"], "shortcut": program["shortcut"]})

    settings = folders[first] / "settings.toml"
    if settings.is_file() and not (out / "settings.toml").exists():
        (out / "settings.toml").write_bytes(
            shared_settings(settings.read_text(encoding="utf-8-sig")).encode("utf-8"))
    (out / "saves").mkdir(exist_ok=True)
    # A plain game.toml would be one program's config read by every program.
    stale = out / "game.toml"
    if stale.is_file():
        stale.unlink()

    for program in list(reversed(spec["programs"])):      # the first program's exe goes last
        source = exes[program["program"]]
        target = out / source.name
        partial = out / (source.name + ".installing")
        shutil.copy2(str(source), str(partial))
        os.replace(str(partial), str(target))
        entry = next(row for row in per_program if row["program"] == program["program"])
        entry["executable"] = target.name
        entry["executable_sha256"] = sha256(target)
        log(f"installed {target.name}")
    record.update(schema=1, set=spec["name"], title=spec["title"], folder=str(out),
                  serials=list(spec["serials"]), set_discs=[str(p) for p in set_discs],
                  per_program=per_program)
    return record


# ---- the kits of a set, before any build ------------------------------------

# What a kit itself puts into the folder its program is built in, and so into
# the set's folder. Everything else there comes from the framework, which the
# programs of a set share.
KIT_SHARED_FILES = {
    "game_options.toml": "the build copies it beside the executable",
    "VERSION": "it becomes psx_game_version.txt",
}
KIT_SHARED_FOLDERS = {
    "mods/preloaded/packages": "a mod package of this name is staged under mods/bundled by each program",
    "launcher_assets": "the launcher's artwork is staged under assets",
}
KIT_NOTICE_PREFIXES = ("LICENSE", "COPYING", "NOTICE", "THIRD_PARTY")
KIT_NOTICE_REASON = "a licence or notice file: a release folder carries it under licenses/kit"
KIT_SKIPPED_FOLDERS = (".git", "psxrecomp", "recomp-ui", "generated", "disc", "prepared_disc", "dist",
                       "saves", "cache", ".cache")


def kit_shared_reason(relative: str) -> str:
    """Why a kit file lands at one path for every program of a set; "" for a
    file each program keeps to itself (its recipe, seeds, sources)."""
    if relative in KIT_SHARED_FILES:
        return KIT_SHARED_FILES[relative]
    for folder, reason in KIT_SHARED_FOLDERS.items():
        if relative.startswith(folder + "/"):
            return reason
    if relative.rsplit("/", 1)[-1].upper().startswith(KIT_NOTICE_PREFIXES):
        return KIT_NOTICE_REASON
    return ""


def kit_files(kit: Path) -> List[str]:
    found = []
    for path in Path(kit).rglob("*"):
        parts = path.relative_to(kit).parts
        if any(part in KIT_SKIPPED_FOLDERS or part.startswith("build") for part in parts[:-1]):
            continue
        if path.is_file():
            found.append("/".join(parts))
    return sorted(found)


def compare_kits(kits: Dict[str, Path]) -> Dict[str, Any]:
    """The join's rule applied to the kits of a set, without a build.

    `shared` lists the files that reach the set's folder from the kits and
    differ between them: each one stops the join after both programs are built.
    `own` lists the other files that differ; each program keeps its own.

    This sees what the kits contribute. It cannot see a difference a build
    creates; the join's own check covers that.
    """
    carriers: Dict[str, Dict[str, str]] = {}
    for name, kit in kits.items():
        if not Path(kit).is_dir():
            raise SetError(f"not a kit folder: {kit}")
        for relative in kit_files(Path(kit)):
            carriers.setdefault(relative, {})[name] = sha256(Path(kit) / relative)
    shared, own = [], []
    for relative, hashes in sorted(carriers.items()):
        if len(set(hashes.values())) < 2:
            continue
        reason = kit_shared_reason(relative)
        if reason:
            shared.append({"path": relative, "why": reason, "sha256": hashes})
        else:
            own.append(relative)
    return {"kits": {name: str(kit) for name, kit in kits.items()}, "shared": shared, "own": own}


# ---- starting each program --------------------------------------------------


def script_name(shortcut: str) -> str:
    """A file name a player can read, from the program's shortcut name."""
    name = re.sub(r'[<>:"/\\|?*]', " ", shortcut)
    return re.sub(r"\s+", " ", name).strip(" .") or "Start"


def host_platform() -> str:
    if os.name == "nt":
        return "windows"
    return "macos" if sys.platform == "darwin" else "linux"


def write_start_scripts(spec: Dict[str, Any], record: Dict[str, Any], root: Path, out: Path,
                        platform: Optional[str] = None) -> List[str]:
    """One start script per program at the set's root, named as the player
    knows the program. They use paths relative to themselves, so the folder can
    be moved; a shortcut file would hold an absolute path. The executables stay
    directly runnable too.

    Windows gets `<name>.cmd`: `start` hands the game to Windows and the script
    ends, so no console window stays open. macOS gets `<name>.command`, which
    Finder runs on a double click. Linux gets `<name>.sh`.
    """
    platform = platform or host_platform()
    root, out = Path(root), Path(out)
    relative = os.path.relpath(str(out), str(root)).replace("\\", "/")
    names = []
    for row in record["per_program"]:
        title = row["shortcut"]
        if platform == "windows":
            path = root / (script_name(title) + ".cmd")
            folder = "%~dp0" + relative.replace("/", "\\")
            exe = folder + "\\" + row["executable"]
            text = ("@echo off\r\n"
                    f"rem Starts {title}. Setup wrote this file; it moves with the folder.\r\n"
                    f'if not exist "{exe}" goto missing\r\n'
                    f'start "" /D "{folder}" "{exe}"\r\n'
                    "exit /b 0\r\n"
                    ":missing\r\n"
                    f"echo {title} is not set up yet. Run setup in this folder first.\r\n"
                    "pause\r\n"
                    "exit /b 1\r\n")
            path.write_bytes(text.encode("utf-8"))
        else:
            path = root / (script_name(title) + (".command" if platform == "macos" else ".sh"))
            text = ("#!/bin/sh\n"
                    f"# Starts {title}. Setup wrote this file; it moves with the folder.\n"
                    'here=$(cd "$(dirname "$0")" && pwd)\n'
                    f'folder="$here/{relative}"\n'
                    f'exe="$folder/{row["executable"]}"\n'
                    'if [ ! -x "$exe" ]; then\n'
                    f'  echo "{title} is not set up yet. Run setup in this folder first." >&2\n'
                    "  exit 1\n"
                    "fi\n"
                    'cd "$folder" && exec "$exe" "$@"\n')
            path.write_bytes(text.encode("utf-8"))
            os.chmod(str(path), 0o755)
        names.append(path.name)
    return names


def write_install_record(root: Path, record: Dict[str, Any]) -> Path:
    path = Path(root) / INSTALL_RECORD
    path.write_bytes((json.dumps(record, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    return path


# ---- the set's generate and rebuild ----------------------------------------

NO_DIAGNOSTIC = ("Diagnostic mode is not available for a game whose discs are separate programs. "
                 "This package has no diagnostic build in this release; the normal build was not changed.")
NO_PGO = ("The optimised (PGO) rebuild is not available for a game whose discs are separate programs. "
          "The normal build was not changed.")


class _ProgramProgress:
    """One program's step, reported inside the set's step: its messages carry
    the program's name and its percentage fills the program's share."""

    def __init__(self, parent: Any, label: str, lo: float, hi: float, given_disc: bool = False) -> None:
        self.parent, self.label, self.lo, self.hi = parent, label, lo, hi
        # True when this program's disc is the file the set's caller gave as
        # `--disc`. The program's step sees its own disc as "the given one";
        # for the set that is true of one program at most.
        self.given_disc = given_disc
        self.json_progress = getattr(parent, "json_progress", False)
        self.last_result: Dict[str, Any] = {}

    def phase(self, name: str, *, pct: Optional[float] = None, message: Optional[str] = None,
              **fields: Any) -> None:
        if name == "done":
            return                      # the set says when the whole step is done
        scaled = None if pct is None else round(self.lo + (self.hi - self.lo) * float(pct), 4)
        self.parent.phase(name, pct=scaled, message=f"{self.label}: {message}" if message else None, **fields)

    def log(self, message: str, *, level: str = "info") -> None:
        self.parent.log(f"[{self.label}] {message}", level=level)

    def result(self, **fields: Any) -> None:
        self.last_result = dict(fields)

    def error(self, message: str, *, code: int = 1, **fields: Any) -> None:
        if "refused_given_disc" in fields:
            fields["refused_given_disc"] = bool(fields["refused_given_disc"]) and self.given_disc
        self.parent.error(f"{self.label}: {message}", code=code, **fields)

    def event(self, event: str, **fields: Any) -> None:
        self.parent.event(event, **fields)


def _program_args(args: argparse.Namespace, folder: Path, **changes: Any) -> argparse.Namespace:
    """The arguments of one program's step: the set's own, aimed at the program."""
    sub = argparse.Namespace(**vars(args))
    sub.config = str(folder / "game.toml")
    sub.project_root = str(folder)
    sub.set_disc = None
    for key, value in changes.items():
        setattr(sub, key, value)
    return sub


def generate_set(cli: Any, args: argparse.Namespace, progress: Any) -> int:
    """`psxrecomp_cli.py generate` for a set: check every disc, run the
    ordinary generate for each program, then write the set's marker.

    `cli` is the psxrecomp_cli module. Nothing is generated unless every disc
    of the set is located and is the right disc. The marker is removed first
    and written last, so setup counts as done only after every program is.
    """
    # Whether disc 1 of the set is the file given as `--disc`. A refusal says so
    # (`refused_given_disc`), because the setup window marks the selected file
    # as refused only then: a refusal of another disc of the set must not put a
    # cross on the disc the player is looking at (PS1B-415).
    disc_1_is_given = False
    try:
        spec = load_set(Path(args.config).expanduser())
        root = Path(spec["root"])
        given = parse_set_disc_args(getattr(args, "set_disc", None))
        from_disc_arg = bool(getattr(args, "disc", "")) and 1 not in given
        if from_disc_arg:
            given[1] = str(args.disc)   # the wizard passes the boot disc as --disc
        cli.activate_embedded_toolchain(root, progress)
        discs = located_discs(spec, given)
        if from_disc_arg and discs and discs[0] is not None:
            # located_discs takes the next source when the given path is not a file.
            as_given = Path(str(args.disc)).expanduser()
            if not as_given.is_absolute():
                as_given = root / as_given
            disc_1_is_given = as_given.is_file() and as_given.resolve() == discs[0]
        if any(disc is not None and disc.suffix.lower() == ".chd" for disc in discs):
            reader = cli.ensure_chd_reader(root, progress)
            if reader:
                os.environ.setdefault("PSXRECOMP_LIBCHDR", str(reader))
        progress.phase("verify", pct=0.02,
                       message=f"Checking the {len(spec['serials'])} discs of {spec['title']}")
        check_discs(spec, discs)
    except WrongDisc as error:
        progress.error(str(error), code=cli.EXIT_VERIFY, verify_failed=True,
                       refused_given_disc=disc_1_is_given and error.number == 1)
        return cli.EXIT_VERIFY
    except SetError as error:
        progress.error(str(error), code=cli.EXIT_USAGE)
        return cli.EXIT_USAGE

    marker = root / "generated" / (getattr(args, "gen_marker", "") or SET_MARKER)
    try:
        marker.unlink()
    except OSError:
        pass
    bios = (getattr(args, "bios", "") or "").strip()
    if bios and not Path(bios).expanduser().is_absolute():
        bios = str((root / bios).resolve())   # a program's step would resolve it under the program
    # The emitters are built once, from the set's root. A program reaches the
    # same build folder through its folder link; a second configure of that
    # folder under the link's path would be a different path to CMake.
    try:
        cli.ensure_framework(root, progress=progress)
        cli.ensure_emitters(root, progress,
                            download_toolchain=not bool(getattr(args, "no_toolchain_download", False)),
                            force=bool(getattr(args, "force_emitters", False)))
    except Exception as error:  # noqa: BLE001 - as the single-program generate reports it
        progress.error(str(error), code=cli.EXIT_ERROR)
        return cli.EXIT_ERROR
    count = len(spec["programs"])
    done = []
    for index, program in enumerate(spec["programs"]):
        folder = root / program["folder"]
        try:
            prepare_program_folder(spec, program)
        except (SetError, OSError) as error:
            progress.error(f"{program['program']}: {error}", code=cli.EXIT_ERROR)
            return cli.EXIT_ERROR
        child = _ProgramProgress(progress, f"{program['program']} ({index + 1} of {count})",
                                 0.05 + 0.9 * index / count, 0.05 + 0.9 * (index + 1) / count,
                                 given_disc=disc_1_is_given and program["positions"][0] == 1)
        code = cli.cmd_generate(_program_args(args, folder, disc=str(discs[program["positions"][0] - 1]),
                                              bios=bios, gen_marker="", force_emitters=False), child)
        if code != cli.EXIT_OK:
            return code
        done.append({"program": program["program"], "marker": child.last_result.get("marker"),
                     "disc": child.last_result.get("disc")})
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_bytes((json.dumps({"schema": 1, "set": spec["name"], "discs": [str(d) for d in discs],
                                    "programs": done}, indent=2) + "\n").encode("utf-8"))
    progress.phase("done", pct=1.0, message="Generate complete")
    progress.result(ok=True, out_dir=str(marker.parent), marker=str(marker), disc=str(discs[0]),
                    programs=done)
    return cli.EXIT_OK


def rebuild_set(cli: Any, args: argparse.Namespace, progress: Any) -> int:
    """`psxrecomp_cli.py rebuild` for a set: the ordinary rebuild for each
    program in its own folder, then the join into `--build-dir`.

    The first program's executable in `--build-dir` is what tells setup the
    set is installed. It is removed before the first build and comes back as
    the join's last step, so a stop anywhere leaves setup as what starts.
    """
    try:
        spec = load_set(Path(args.config).expanduser())
        root = Path(spec["root"])
        if getattr(args, "diagnostic_only", False) or (getattr(args, "diagnostic_dir", "") or "").strip():
            raise SetError(NO_DIAGNOSTIC)
        if getattr(args, "force_pgo", False):
            raise SetError(NO_PGO)
        if not read_marker(root):
            raise SetError(f"{spec['title']} has not been generated yet: run Generate first.")
        discs = located_discs(spec, parse_set_disc_args(getattr(args, "set_disc", None)))
        lost = [str(n + 1) for n, disc in enumerate(discs) if disc is None]
        if lost:
            raise SetError(f"Disc {', '.join(lost)} of {spec['title']} is not where it was at Generate. "
                           "Each program's settings name its disc: put the disc back, or run Generate again.")
    except SetError as error:
        progress.error(str(error), code=cli.EXIT_USAGE)
        return cli.EXIT_USAGE

    out = cli._resolve_under(root, args.build_dir)
    first = spec["programs"][0]
    for name in (first["exe_name"] + ".exe", first["exe_name"]):
        try:
            (out / name).unlink()
        except FileNotFoundError:
            pass
        except OSError as error:
            progress.error(f"{name} is in use ({error}). Close the game, then rebuild.", code=cli.EXIT_ERROR)
            return cli.EXIT_ERROR
    asked = {mode.strip() for mode in (getattr(args, "prune_after", "") or "").split(",") if mode.strip()}
    prune = "build-intermediates" if asked & {"build-intermediates", "all"} else ""
    count = len(spec["programs"])
    products: Dict[str, Path] = {}
    lto = None
    for index, program in enumerate(spec["programs"]):
        folder = root / program["folder"]
        try:
            prepare_program_folder(spec, program)
        except (SetError, OSError) as error:
            progress.error(f"{program['program']}: {error}", code=cli.EXIT_ERROR)
            return cli.EXIT_ERROR
        child = _ProgramProgress(progress, f"{program['program']} ({index + 1} of {count})",
                                 0.02 + 0.92 * index / count, 0.02 + 0.92 * (index + 1) / count)
        code = cli.cmd_rebuild(_program_args(
            args, folder, build_dir=str(folder / "build-release"), exe_basename=program["exe_name"], disc="",
            no_pgo=True, force_pgo=False, diagnostic_dir="", diagnostic_only=False, prune_after=prune,
            # the package's root: where its licenses/toolchain texts are
            package_root=str(root)), child)
        if code != cli.EXIT_OK:
            return code
        built = child.last_result.get("exe")
        products[program["program"]] = Path(built).parent if built else folder / "build-release"
        lto = child.last_result.get("lto") if lto is None else lto
    progress.phase("join", pct=0.96, message=f"Joining the programs into {out.name}")
    try:
        record = join_products(spec, products, out, discs=discs, log=progress.log)
        record["start_scripts"] = write_start_scripts(spec, record, root, out)
        installed = write_install_record(root, record)
    except (SetError, OSError) as error:
        progress.error(str(error), code=cli.EXIT_ERROR)
        return cli.EXIT_ERROR
    progress.phase("done", pct=1.0, message="Rebuild complete")
    progress.result(ok=True, exe=str(out / record["per_program"][0]["executable"]), pgo=False,
                    pgo_skipped="not available for a set of programs", lto=lto,
                    programs=[row["executable"] for row in record["per_program"]],
                    start_scripts=record["start_scripts"], install_record=str(installed))
    return cli.EXIT_OK


# ---- the set's setup host ---------------------------------------------------


def _c_identifier(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_") or "set"


def _c_string(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def init_host(root: Path, force: bool = False) -> List[str]:
    """Write the set's host project at the set's root, from set.toml.

    The host is the setup program: one per set. It is built from these three
    files and never holds game code. A file that this tool did not write is
    not replaced without --force.
    """
    root = Path(root).resolve()
    spec = load_set(root / SET_FILE)
    first = spec["programs"][0]
    stem = spec["bios_stem"]
    for key, value in (("[set] exe_name", spec["exe_name"]),
                       (f"[program.{first['program']}] exe_name", first["exe_name"]),
                       ("[set] bios_stem", stem or "x")):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
            raise SetError(f"set.toml: {key} = {value!r} cannot be used as a file or target name")
    # A set whose programs are bound to one retail BIOS says so with
    # [set] bios_stem; the host then asks for that BIOS, as each program's own
    # project does. Without the key the host keeps the framework's default.
    bios_block = "" if not stem else (
        f'set(PSXRECOMP_BIOS_STEMS "{stem}" CACHE STRING "Bound retail BIOS" FORCE)\n'
        f'set(PSXRECOMP_BIOS_STEM "{stem}" CACHE STRING "Accepted BIOS" FORCE)\n'
        f'set(PSXRECOMP_BIOS_PROFILE "${{PSXRECOMP_ROOT}}/bios/{stem}.toml" CACHE FILEPATH "Accepted BIOS" FORCE)')
    tokens = {
        "@HOST_STAMP@": HOST_STAMP,
        "@PROJECT_CMAKE_NAME@": _c_identifier(spec["exe_name"]).lower(),
        "@SET_TITLE@": _c_string(spec["title"]),
        "@SET_EXE_NAME@": spec["exe_name"],
        "@FIRST_EXE_NAME@": first["exe_name"],
        "@ENV_PREFIX@": _c_identifier(spec["exe_name"]).upper(),
        "@BIOS_CMAKE_BLOCK@": bios_block,
        "@SET_FILE@": SET_FILE,
        "@SET_MARKER@": SET_MARKER,
    }
    written = []
    for name in HOST_FILES:
        target = root / name
        if target.is_file() and not force and HOST_STAMP not in target.read_text(encoding="utf-8", errors="replace"):
            raise SetError(f"{target} exists and was not written by init-host; pass --force to replace it")
        text = (TEMPLATES / (name + ".in")).read_text(encoding="utf-8")
        for token, value in tokens.items():
            text = text.replace(token, value)
        left = re.findall(r"@[A-Z_]+@", text)
        if left:
            raise SetError(f"template {name}.in has unfilled tokens: {', '.join(sorted(set(left)))}")
        target.write_bytes(text.encode("utf-8"))
        written.append(name)
    return written


# ---- command line -----------------------------------------------------------


def _public(spec: Dict[str, Any]) -> Dict[str, Any]:
    return {key: (str(value) if isinstance(value, Path) else value) for key, value in spec.items()}


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="read set.toml and print the set as JSON")
    check.add_argument("--set", required=True, dest="set_file")
    folders = commands.add_parser("folders", help="print each program's folder, one per line (the packager)")
    folders.add_argument("--set", required=True, dest="set_file")
    host = commands.add_parser("init-host", help="write the set's host project at the set root")
    host.add_argument("--root", required=True)
    host.add_argument("--force", action="store_true")
    join = commands.add_parser("join", help="join the programs' built folders into one folder")
    join.add_argument("--set", required=True, dest="set_file")
    join.add_argument("--program", action="append", default=[], metavar="NAME=FOLDER")
    join.add_argument("--set-disc", action="append", default=[], metavar="N=PATH")
    join.add_argument("--out", required=True)
    join.add_argument("--record", default="", help="write the join's record here as JSON (default: stdout); "
                                                   "after a disagreement it holds the list of files")
    kits = commands.add_parser(
        "kit-check", help="before any build: list the files the kits of a set would disagree about in one folder")
    kits.add_argument("kit", nargs="+", metavar="[NAME=]FOLDER", help="each program's kit, two or more")
    kits.add_argument("--json", action="store_true", help="print the whole comparison as JSON")
    args = parser.parse_args(argv)
    try:
        if args.command == "kit-check":
            named: Dict[str, Path] = {}
            for value in args.kit:
                name, sep, folder = value.partition("=")
                # NAME=FOLDER, or a bare folder (a Windows drive letter is not a name)
                if not sep or len(name) < 2 or "/" in name or "\\" in name:
                    name, folder = Path(value).name, value
                if name in named:
                    raise SetError(f"two kits are named {name}; give NAME=FOLDER")
                named[name] = Path(folder)
            if len(named) < 2:
                raise SetError("kit-check takes the kits of a set: two or more folders")
            result = compare_kits(named)
            if args.json:
                print(json.dumps(result, indent=2, sort_keys=True))
            else:
                for row in result["shared"]:
                    print(f"differs: {row['path']} ({' and '.join(row['sha256'])}): {row['why']}")
                print(f"{len(result['shared'])} file(s) the programs would disagree about in one folder; "
                      f"{len(result['own'])} other differing file(s) stay with each program")
            return 1 if result["shared"] else 0
        if args.command == "check":
            print(json.dumps(_public(load_set(Path(args.set_file))), indent=2, sort_keys=True))
            return 0
        if args.command == "folders":
            for program in load_set(Path(args.set_file))["programs"]:
                print(program["folder"])
            return 0
        if args.command == "init-host":
            for name in init_host(Path(args.root), force=args.force):
                print(f"wrote {name}")
            return 0
        spec = load_set(Path(args.set_file))
        products = {}
        for value in args.program:
            name, sep, folder = value.partition("=")
            if not sep or not folder:
                raise SetError(f"--program takes NAME=FOLDER, not {value!r}")
            products[name.strip()] = Path(folder.strip())
        given = parse_set_disc_args(args.set_disc)
        discs = [given.get(n) for n in range(1, len(spec["serials"]) + 1)] if given else None
        try:
            record = join_products(spec, products, Path(args.out), discs=discs,
                                   log=lambda message: print(message, file=sys.stderr))
        except Disagreement as stop:
            if args.record:      # nothing was joined; the record says why, file by file
                Path(args.record).write_bytes(
                    (json.dumps({"disagreements": stop.files}, indent=2, sort_keys=True) + "\n").encode("utf-8"))
            raise
        text = json.dumps(record, indent=2, sort_keys=True) + "\n"
        if args.record:
            Path(args.record).write_bytes(text.encode("utf-8"))
        else:
            sys.stdout.write(text)
        return 0
    except SetError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
