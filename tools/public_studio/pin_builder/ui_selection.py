"""Bind a core pin to its bundled UI or one UI its Build declares compatible.

The selected PIN.json (the Build record) owns the core identity and, in
`ui_host_contract`, the identities a separate UI must present: host
ABI/provider, module ABI/provider/SHA-256 and file, shared SDL
version/SHA-256 and file, asset receipt SHA-256 and file. A UI record only
states what it is; it is compared field by field with that contract in
CONTRACT_ORDER, and the staged product bytes are hashed against it in
STAGED_ORDER. The first difference is refused. A Build without the contract
accepts only its bundled UI.

A selected module UI also names its prebuilt module and shared SDL3
(`artifacts`, see module_artifacts); the build links them and writes the
asset receipt (write_asset_receipt) before the staged bytes are checked.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re

CURRENT_STUDIO_RECORD = Path(
    "Z:/Share/Projects/Recompilations/PS1/Programme/WORKBENCH-STUDIO.json"
)
PIN_ROOT_ENV = "WORKBENCH_PIN_ROOT"
UI_ROOT_ENV = "WORKBENCH_UI_ROOT"
STUDIO_RECORD_ENV = "WORKBENCH_STUDIO_RECORD"
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")

# The order in which a selected UI is compared with the selected Build's
# ui_host_contract. The first field that differs is the one reported.
# (label, section, key, form); form "text" is a non-empty string, "sha256" 64 hex.
CONTRACT_ORDER = (
    ("host ABI", "host", "abi", "text"),
    ("host provider", "host", "provider", "text"),
    ("module ABI", "module", "abi", "text"),
    ("module provider", "module", "provider", "text"),
    ("module SHA-256", "module", "sha256", "sha256"),
    ("shared SDL version", "shared_sdl", "version", "text"),
    ("shared SDL SHA-256", "shared_sdl", "sha256", "sha256"),
    ("asset receipt SHA-256", "assets", "receipt_sha256", "sha256"),
)
UI_FIELDS = tuple((section, key, form) for _label, section, key, form in CONTRACT_ORDER)
# The Build also names the product-relative file that carries each staged
# identity, so the product path hashes the bytes the Build names.
STAGED_ORDER = (
    ("launcher module", "module", "file", "sha256"),
    ("shared SDL", "shared_sdl", "file", "sha256"),
    ("asset receipt", "assets", "receipt_file", "receipt_sha256"),
)
CONTRACT_FIELDS = UI_FIELDS + tuple((section, key, "file") for _l, section, key, _h in STAGED_ORDER)


class UiSelectionError(RuntimeError):
    pass


def _absolute(value, base):
    path = Path(value)
    return path if path.is_absolute() else (base / path).resolve()


def discover_registry(pin_root=None, ui_root=None, studio_record=None, environ=None):
    """Resolve supported current layouts; never fall back to retired psxrecomp."""
    environ = os.environ if environ is None else environ
    package = Path(__file__).resolve().parents[1] / "registry"
    record_path = Path(studio_record or environ.get(STUDIO_RECORD_ENV) or CURRENT_STUDIO_RECORD)
    candidates = []
    chosen_pin = Path(pin_root) if pin_root else None
    chosen_ui = Path(ui_root) if ui_root else None
    source = "arguments" if chosen_pin or chosen_ui else None
    if not chosen_pin and environ.get(PIN_ROOT_ENV):
        chosen_pin, source = Path(environ[PIN_ROOT_ENV]), "environment"
    if not chosen_ui and environ.get(UI_ROOT_ENV):
        chosen_ui, source = Path(environ[UI_ROOT_ENV]), "environment"
    record = None
    try:
        record = json.loads(record_path.read_text(encoding="utf-8-sig"))
        registries = record.get("registries") or {}
        if not chosen_pin and registries.get("pins"):
            chosen_pin = _absolute(registries["pins"], record_path.parent)
            source = "studio-record"
        if not chosen_ui and registries.get("uis"):
            chosen_ui = _absolute(registries["uis"], record_path.parent)
            source = "studio-record"
    except (OSError, ValueError, TypeError):
        pass
    candidates.append({"kind": "studio-record", "path": str(record_path),
                       "readable": record is not None})
    if not chosen_pin and (package / "pins").is_dir():
        chosen_pin, source = package / "pins", "package"
    if not chosen_ui and (package / "uis").is_dir():
        chosen_ui, source = package / "uis", "package"
    candidates.append({"kind": "package", "path": str(package),
                       "readable": package.is_dir()})
    return {"schema": 1, "source": source or "unbound",
            "pin_root": str(chosen_pin.resolve()) if chosen_pin else None,
            "ui_root": str(chosen_ui.resolve()) if chosen_ui else None,
            "studio_record": str(record_path), "candidates": candidates,
            "message": None if chosen_pin else
                (f"No current Workbench pin registry is bound. Set {PIN_ROOT_ENV}, pass --pin-root, "
                 f"or publish registries.pins in {record_path}. Retired Z:/Share/psxrecomp is not searched.")}


def _pin_ui_commit(pin):
    ref = pin.get("recomp_ui_ref")
    if isinstance(ref, dict):
        return ref.get("commit") or ref.get("ref")
    parts = str(ref or "").split()
    return parts[1] if len(parts) == 2 else (parts[0] if parts else None)


def _pin_ui_tree(pin):
    tree = pin.get("recomp_ui_tree")
    if tree:
        return str(tree)
    detail = str(pin.get("recomp_ui_ref_detail") or "")
    match = re.search(r"\btree\s+([0-9a-f]{40})\b", detail)
    return match.group(1) if match else None


def bundled_option(pin):
    return {"name": f"Bundled with {pin.get('name', 'selected build version')}",
            "mode": "bundled", "path": None, "compatible": True, "reason": None,
            "core": {"commit": pin.get("commit"), "tree": pin.get("tree")},
            "ui": {"commit": _pin_ui_commit(pin), "tree": _pin_ui_tree(pin)},
            "module": {"mode": "embedded", "abi": "legacy-embedded", "sha256": None},
            "shared_sdl": {"mode": "not-separate", "version": None, "sha256": None},
            "assets": {"mode": "bundled-ui-tree", "receipt_sha256": None},
            "binding": "pin-bundled-default"}


def load_ui_record(path):
    """Read a UI.json. It states what the UI is; it never states what a Build expects.

    A `compatibility.bindings` list in the record is ignored: the expected
    identities come only from the selected Build's `ui_host_contract`.
    """
    path = Path(path).resolve()
    record = json.loads(path.read_text(encoding="utf-8-sig"))
    ui = record.get("ui") or {}
    missing = []
    if record.get("schema") != 1 or record.get("kind") != "workbench-ui":
        missing.append("schema 1 kind workbench-ui")
    if not HEX40.fullmatch(str(ui.get("commit") or "")):
        missing.append("full UI commit")
    if not HEX40.fullmatch(str(ui.get("tree") or "")):
        missing.append("full UI tree")
    if record.get("status") == "incompatible":
        if not record.get("reason"):
            missing.append("incompatibility reason")
    else:
        missing += _missing(record, UI_FIELDS, "")
    if missing:
        raise UiSelectionError(f"UI record {path} lacks " + ", ".join(missing) + ".")
    return record, path


def _missing(value, fields, prefix):
    missing = []
    for section, key, form in fields:
        part = value.get(section)
        item = part.get(key) if isinstance(part, dict) else None
        ok = isinstance(item, str) and bool(item)
        if ok and form == "sha256":
            ok = bool(HEX64.fullmatch(item))
        elif ok and form == "file":
            # Product-relative: no absolute path and no step out of the product.
            ok = not Path(item).is_absolute() and ".." not in Path(item).parts
        if not ok:
            missing.append(f"{prefix}{section}.{key}")
    return missing


def build_contract(pin):
    """The selected Build's own expected identities for a separate UI, or a refusal reason."""
    contract = pin.get("ui_host_contract")
    if not isinstance(contract, dict):
        return None, (f"selected Build core {pin.get('commit')} declares no separate-UI contract "
                      "(ui_host_contract): it names no expected host, module, shared SDL or asset identities, "
                      "so only its bundled UI can be selected")
    missing = _missing(contract, CONTRACT_FIELDS, "ui_host_contract.")
    if missing:
        return None, f"selected Build core {pin.get('commit')} has an incomplete contract: lacks " + ", ".join(missing)
    return contract, None


def _first_mismatch(pin, record):
    """Compare the UI record with the Build-owned contract in CONTRACT_ORDER; first difference wins."""
    if record.get("status") == "incompatible":
        return record["reason"]
    contract, reason = build_contract(pin)
    if reason:
        return reason
    for label, section, key, _form in CONTRACT_ORDER:
        expected = contract[section][key]
        found = (record.get(section) or {}).get(key)
        if found != expected:
            return (f"{label} mismatch: selected Build {pin.get('commit')} expects {expected!r}, "
                    f"selected UI has {found!r}")
    return None


def evaluate(pin, record, path):
    ui = record["ui"]
    reason = _first_mismatch(pin, record)
    result = {"name": record.get("name") or ui["commit"][:9], "mode": "explicit",
              "path": str(path), "compatible": reason is None, "reason": reason,
              "core": {"commit": pin.get("commit"), "tree": pin.get("tree")},
              "ui": {"commit": ui["commit"], "tree": ui["tree"]},
              "module": record.get("module") or {"mode": "absent"},
              "shared_sdl": record.get("shared_sdl") or {"mode": "not-separate"},
              "assets": record.get("assets") or {"mode": "legacy-unreceipted"},
              "binding": "refused"}
    if reason is None:
        # Record the Build-owned values (they equal the UI's), with the files they name.
        contract = pin["ui_host_contract"]
        result.update(host=dict(contract["host"]), module=dict(contract["module"]),
                      shared_sdl=dict(contract["shared_sdl"]), assets=dict(contract["assets"]),
                      binding="selected-build-ui-host-contract")
        if isinstance(record.get("artifacts"), dict):
            result["artifacts"] = dict(record["artifacts"])
        else:
            # The identities match, but a module build links the record's prebuilt files
            # (module_artifacts) and would stop at its first stage; Studio shows this and
            # keeps Build off (PS1B-586).
            result["build_refusal"] = (
                "the identities match this build, but the record names no prebuilt files "
                "(artifacts.module_file, artifacts.sdl3_dll, artifacts.sdl3_cmake_dir), so its launcher module "
                "cannot be built or staged; load a UI record that carries them")
    return result


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_staged(pin, selection, product, hasher=sha256_file):
    """Hash the module, shared SDL and asset receipt staged in `product`.

    Each is compared with the selected Build's contract in STAGED_ORDER and the
    first mismatch raises; later identities are not read. A bundled UI stages
    no separate module, so there is nothing to compare. Writes nothing.
    """
    if selection.get("mode") == "bundled":
        return {"mode": "bundled", "checked": [],
                "note": "Bundled UI is embedded in the executable; no separate module, shared SDL or asset receipt."}
    contract, reason = build_contract(pin)
    if reason:
        raise UiSelectionError(f"Staged product refused: {reason}.")
    product = Path(product).resolve()
    checked = []
    for label, section, file_key, hash_key in STAGED_ORDER:
        relative = contract[section][file_key]
        expected = contract[section][hash_key]
        path = (product / relative).resolve()
        if product not in path.parents:
            raise UiSelectionError(f"Staged product refused: {label} file {relative} is outside the product.")
        if not path.is_file():
            raise UiSelectionError(
                f"Staged product refused at first mismatching identity: {label} {relative} is missing; "
                f"selected Build {pin.get('commit')} expects SHA-256 {expected}.")
        found = hasher(path)
        if found != expected:
            raise UiSelectionError(
                f"Staged product refused at first mismatching identity: {label} SHA-256 of {relative}: "
                f"selected Build {pin.get('commit')} expects {expected}, staged bytes are {found}.")
        checked.append({"identity": label, "file": relative, "sha256": found})
    return {"mode": "explicit", "checked": checked}


# A module UI record names where its prebuilt files are, relative to its UI.json:
# the launcher module, the shared SDL3 DLL and that SDL3's CMake package folder.
# These are locations only; the identities stay the Build's contract.
ARTIFACT_KEYS = ("module_file", "sdl3_dll", "sdl3_cmake_dir")


def module_artifacts(pin, selection, hasher=sha256_file):
    """The prebuilt module and shared SDL a module-mode build links, checked against the Build.

    None for a bundled UI: it builds as before. For a selected module UI the
    record's `artifacts` paths are resolved beside its UI.json (no absolute
    path, no step out of the record's folder); the module and the SDL3 DLL
    are hashed against the Build's contract before anything is built, and the
    first mismatch is refused.
    """
    if selection.get("mode") == "bundled":
        return None
    contract, reason = build_contract(pin)
    if reason:
        raise UiSelectionError(f"Module build refused: {reason}.")
    record = Path(selection["path"])
    artifacts = selection.get("artifacts")
    if not isinstance(artifacts, dict):
        raise UiSelectionError(
            f"Module build refused: UI record {record} names no prebuilt files (artifacts.module_file, "
            "artifacts.sdl3_dll, artifacts.sdl3_cmake_dir), so the launcher module cannot be staged.")
    base = record.parent.resolve()
    paths = {}
    for key in ARTIFACT_KEYS:
        value = artifacts.get(key)
        if not isinstance(value, str) or not value or Path(value).is_absolute() or ".." in Path(value).parts:
            raise UiSelectionError(f"Module build refused: UI record {record} artifacts.{key} must be a path "
                                   "relative to the record's folder.")
        path = (base / value).resolve()
        if base not in path.parents or not (path.is_dir() if key == "sdl3_cmake_dir" else path.is_file()):
            raise UiSelectionError(f"Module build refused: artifacts.{key} {value} is missing beside {record}.")
        paths[key] = path
    checked = []
    for label, key, section in (("launcher module", "module_file", "module"), ("shared SDL", "sdl3_dll", "shared_sdl")):
        expected, found = contract[section]["sha256"], hasher(paths[key])
        if found != expected:
            raise UiSelectionError(
                f"Module build refused at first mismatching identity: {label} SHA-256 of {paths[key]}: "
                f"selected Build {pin.get('commit')} expects {expected}, the file is {found}.")
        checked.append({"identity": label, "path": str(paths[key]), "sha256": found})
    config = paths["sdl3_cmake_dir"]
    if not any((config / name).is_file() for name in ("SDL3Config.cmake", "sdl3-config.cmake")):
        raise UiSelectionError(f"Module build refused: {config} holds no SDL3Config.cmake.")
    return {"record": str(record), "module": str(paths["module_file"]), "sdl3_dll": str(paths["sdl3_dll"]),
            "sdl3_cmake_dir": str(config), "checked": checked}


def asset_receipt_bytes(entries, root="assets"):
    """The asset receipt bytes the module producer wrote with PowerShell 7 ConvertTo-Json.

    `entries` are (path, bytes, sha256) in receipt order. The layout is that
    of ConvertTo-Json: two-space indent, CRLF line ends, UTF-8 without BOM,
    no final newline.
    """
    files = [{"path": path, "bytes": size, "sha256": digest} for path, size, digest in entries]
    text = json.dumps({"schema": 1, "root": root, "files": files}, indent=2, ensure_ascii=False)
    return text.replace("\n", "\r\n").encode("utf-8")


def asset_entries(folder):
    """Every file under `folder`, ordered as the producer's `Sort-Object FullName` ordered them.

    That sort compares full Windows paths without regard to case; the key here
    is the relative path with backslashes, case-folded. PowerShell's culture
    rules for punctuation are not reproduced: a name set where they differ
    gives other bytes, and verify_staged then refuses the product.
    """
    folder = Path(folder)
    files = [p for p in folder.rglob("*") if p.is_file()]
    files.sort(key=lambda p: str(p.relative_to(folder)).replace("/", "\\").casefold())
    return [(p.relative_to(folder).as_posix(), p.stat().st_size, sha256_file(p)) for p in files]


def write_asset_receipt(folder, path):
    """Write the receipt of the staged assets `folder` to `path`; return its file, bytes and SHA-256."""
    entries = asset_entries(folder)
    data = asset_receipt_bytes(entries, Path(folder).name)
    Path(path).write_bytes(data)
    return {"file": Path(path).name, "files": len(entries), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def _option(pin, path, name):
    try:
        record, exact = load_ui_record(path)
        return evaluate(pin, record, exact)
    except (OSError, ValueError, UiSelectionError) as error:
        return {"name": name, "mode": "explicit", "path": str(Path(path).resolve()),
                "compatible": False, "reason": str(error), "error": str(error)}


def options(pin, ui_root=None, ui_files=()):
    """The bundled UI, every UI.json in the registry, then each UI file loaded by hand.

    A loaded file is evaluated against this Build exactly as a registry record is;
    a refused one stays listed with its reason. A file already listed is not repeated.
    """
    rows = [bundled_option(pin)]
    if ui_root:
        for path in sorted(Path(ui_root).glob("*/UI.json")):
            rows.append(_option(pin, path, path.parent.name))
    for path in ui_files or ():
        if str(Path(path).resolve()) not in {row.get("path") for row in rows}:
            rows.append(_option(pin, path, Path(path).name))
    return rows


def select(pin, selected=None):
    if selected is None:
        return bundled_option(pin)
    record, path = load_ui_record(selected)
    result = evaluate(pin, record, path)
    if not result["compatible"]:
        raise UiSelectionError(
            f"UI selection refused before build: core {pin.get('commit')} / UI {result['ui']['commit']}: "
            f"{result['reason']}")
    return result
