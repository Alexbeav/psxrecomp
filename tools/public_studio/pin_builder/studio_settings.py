"""Per-host build paths for Workbench Studio (PS1B-586).

A build needs local inputs that are not in the pin: the framework and UI
clones, the toolchain, chdman, the overlay archives and the kits. They used
to be fixed paths in pin_build.py, and every one of them disappeared in the
2026-10-12 move. Each is now a setting, read in this order:

1. the command-line option (--source-repo and so on);
2. its WORKBENCH_* environment variable;
3. the programme's Studio record (WORKBENCH-STUDIO.json, or the file named by
   WORKBENCH_STUDIO_RECORD), section hosts.<HOST>.paths, where HOST is this
   computer's name in upper case.

A setting with no value has no built-in folder to fall back to (two have a
standard install location: the toolchain pack and the run root). A build
that needs it stops at "Validate pin and tools" with the setting's name.

`apply_to_environ` copies the record's values into the environment variables
that are still unset, so every reader that already honours a WORKBENCH_*
variable also honours the record. `report` checks each value for the
Settings dialog, and `save` writes the dialog's values back to the record.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile

RECORD = Path("Z:/Share/Projects/Recompilations/PS1/Programme/WORKBENCH-STUDIO.json")
RECORD_ENV = "WORKBENCH_STUDIO_RECORD"

# key, label, kind, environment variable, command-line option, required for a build.
# kind: clone (a git clone), file, folder, folders (os.pathsep-separated list),
# toolchain (a folder holding retcomm-toolchain.json), package (a folder holding
# native/chdman/chdman.exe).
SETTINGS = (
    ("source_repo", "Framework clone (psxrecomp)", "clone", "WORKBENCH_SOURCE_REPO", "--source-repo", True),
    ("ui_repo", "UI clone (recomp-ui)", "clone", "WORKBENCH_UI_REPO", "--ui-repo", True),
    ("toolchain_pin", "Toolchain pin file (TOOLCHAIN-PIN.toml)", "file", "WORKBENCH_TOOLCHAIN_PIN", "--toolchain-pin", True),
    ("toolchain", "Installed toolchain pack (cmake-clang)", "toolchain", "WORKBENCH_TOOLCHAIN", "--toolchain", True),
    ("package", "Package folder holding native/chdman/chdman.exe", "package", "WORKBENCH_PACKAGE", "--chdman", True),
    ("overlay_archives", "Overlay archive folders (python embed, tcc)", "folders", "WORKBENCH_OVERLAY_ARCHIVES", "--overlay-archives", True),
    ("kit_roots", "Kit root folders", "folders", "WORKBENCH_KIT_ROOTS", "--kits-root", True),
    ("kit_inventory", "Kit inventory file (optional)", "file", "WORKBENCH_KIT_INVENTORY", None, False),
    ("submodule_fallback", "Clone with submodules checked out (optional)", "clone", "WORKBENCH_SUBMODULE_FALLBACK", None, False),
    ("run_root", "Run folder root (work trees and evidence)", "run_root", "WORKBENCH_RUN_ROOT", "--run-root", False),
    ("python", "Python 3.12 for Studio (optional)", "file", "WORKBENCH_PYTHON", None, False),
)
KEYS = tuple(row[0] for row in SETTINGS)
BY_KEY = {row[0]: row for row in SETTINGS}


class SettingsError(RuntimeError):
    pass


def host_name(host=None):
    return (host or socket.gethostname()).upper()


def record_path(path=None, environ=None):
    environ = os.environ if environ is None else environ
    return Path(path or environ.get(RECORD_ENV) or RECORD)


def read_record(path=None, environ=None):
    """The record as a dict, or {} when it is missing or unreadable."""
    try:
        data = json.loads(record_path(path, environ).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def host_paths(record, host=None):
    """hosts.<HOST>.paths of the record: {key: str}. A list value is joined with os.pathsep."""
    section = ((record.get("hosts") or {}).get(host_name(host)) or {}).get("paths") or {}
    values = {}
    for key, value in section.items():
        if key not in BY_KEY:
            continue
        if isinstance(value, list):
            value = os.pathsep.join(str(v) for v in value if str(v).strip())
        if isinstance(value, str) and value.strip():
            values[key] = value.strip()
    return values


def default_value(key, environ=None):
    """The two settings with a standard install location; None for the rest."""
    environ = os.environ if environ is None else environ
    local = environ.get("LOCALAPPDATA")
    if key == "toolchain" and local:
        return str(Path(local) / "RetComM/toolchains/cmake-clang-v1/1.0.14")
    if key == "run_root":
        return str(Path(local or tempfile.gettempdir()) / "PSXWorkbench/builds")
    return None


def resolve(environ=None, record=None, host=None, path=None):
    """{key: {"value", "source"}}: source is environment, studio-record, default or None."""
    environ = os.environ if environ is None else environ
    record = read_record(path, environ) if record is None else record
    stored = host_paths(record, host)
    result = {}
    for key, _label, _kind, env, _option, _required in SETTINGS:
        if environ.get(env, "").strip():
            result[key] = {"value": environ[env].strip(), "source": "environment"}
        elif key in stored:
            result[key] = {"value": stored[key], "source": "studio-record"}
        elif default_value(key, environ):
            result[key] = {"value": default_value(key, environ), "source": "default"}
        else:
            result[key] = {"value": None, "source": None}
    return result


def apply_to_environ(environ=None, path=None, host=None):
    """Fill each unset WORKBENCH_* variable from the record; return the resolution."""
    environ = os.environ if environ is None else environ
    resolution = resolve(environ, path=path, host=host)
    for key, item in resolution.items():
        if item["source"] == "studio-record":
            environ[BY_KEY[key][3]] = item["value"]
    return resolution


def describe(key, host=None):
    """How a stop names a setting: its label, key, option and variable."""
    _key, label, _kind, env, option, _required = BY_KEY[key]
    via = f"{option} or {env}" if option else env
    return (f"the Studio setting \"{label}\" ({key}) for host {host_name(host)}: set it in "
            f"Workbench Studio > Settings, or with {via}")


def require(values, needed, host=None):
    """Stop text for the first needed setting whose value is empty, else None."""
    for key in needed:
        value = values.get(key)
        if value is None or (isinstance(value, (list, tuple)) and not value) or str(value).strip() in ("", "."):
            return (f"A build path is not set: {describe(key, host)}. Nothing was built.")
    return None


def _git(repo, *args, timeout=30):
    try:
        proc = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return 1, str(error)
    return proc.returncode, (proc.stdout or proc.stderr).strip()


def pin_refs(pin):
    """The commits a pin needs from each clone: {"source_repo": commit, "ui_repo": commit}."""
    ref = pin.get("recomp_ui_ref")
    if isinstance(ref, dict):
        ui = ref.get("commit") or ref.get("ref")
    else:
        parts = str(ref or "").split()
        ui = parts[1] if len(parts) == 2 else (parts[0] if parts else None)
    return {"source_repo": pin.get("commit"), "ui_repo": ui}


def check(key, value, pin=None):
    """(ok, message) for one value of one setting."""
    kind = BY_KEY[key][2]
    if not value:
        required = BY_KEY[key][5]
        return (not required), ("not set; a build stops here" if required else "not set (optional)")
    if kind == "folders":
        parts = [p.strip() for p in str(value).split(os.pathsep) if p.strip()]
        missing = [p for p in parts if not Path(p).is_dir()]
        if missing:
            return False, "not a folder: " + "; ".join(missing)
        return True, f"{len(parts)} folder(s) found"
    path = Path(value)
    if kind == "file":
        return (True, "file found") if path.is_file() else (False, "no such file")
    if kind == "run_root":
        if path.is_dir():
            return True, "folder found"
        return (True, "will be created") if path.parent.is_dir() else (False, "neither it nor its parent folder exists")
    if not path.is_dir():
        return False, "no such folder"
    if kind == "toolchain":
        return (True, "toolchain pack found") if (path / "retcomm-toolchain.json").is_file() \
            else (False, "folder has no retcomm-toolchain.json")
    if kind == "package":
        return (True, "chdman found") if (path / "native/chdman/chdman.exe").is_file() \
            else (False, "folder has no native/chdman/chdman.exe")
    # clone
    code, _ = _git(path, "rev-parse", "--git-dir")
    if code:
        return False, "not a git clone"
    commit = pin_refs(pin or {}).get(key)
    if commit:
        code, _ = _git(path, "cat-file", "-e", f"{commit}^{{commit}}")
        if code:
            return False, f"git clone without the pin's commit {commit[:9]} (fetch it)"
        return True, f"git clone holding the pin's commit {commit[:9]}"
    return True, "git clone"


def report(path=None, host=None, pin_file=None, environ=None):
    """Every setting with its value, where it came from, and its check, for the Settings dialog."""
    environ = os.environ if environ is None else environ
    pin = None
    if pin_file:
        try:
            pin = json.loads(Path(pin_file).read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            pin = None
    record = read_record(path, environ)
    stored = host_paths(record, host)
    resolution = resolve(environ, record, host)
    rows = []
    for key, label, kind, env, option, required in SETTINGS:
        item = resolution[key]
        ok, message = check(key, item["value"], pin)
        rows.append({"key": key, "label": label, "kind": kind, "env": env, "option": option,
                     "required": required, "value": item["value"], "source": item["source"],
                     "saved": stored.get(key), "ok": ok, "message": message})
    return {"schema": 1, "host": host_name(host), "record": str(record_path(path, environ)),
            "record_readable": bool(record), "pin": str(pin_file) if pin_file else None, "settings": rows}


def save(values, path=None, host=None, environ=None):
    """Write {key: value} into hosts.<HOST>.paths of the record; an empty value removes the key.

    Every other field of the record is kept as it was. The file is replaced in
    one step, so a reader sees either the old or the new record.
    """
    target = record_path(path, environ)
    unknown = [k for k in values if k not in BY_KEY]
    if unknown:
        raise SettingsError("Unknown setting(s): " + ", ".join(unknown))
    try:
        text = target.read_text(encoding="utf-8-sig")
        record = json.loads(text)
    except FileNotFoundError:
        record = {}
    except (OSError, ValueError) as error:
        raise SettingsError(f"The Studio record {target} cannot be read, so it was not changed: {error}") from None
    if not isinstance(record, dict):
        raise SettingsError(f"The Studio record {target} is not a JSON object, so it was not changed.")
    hosts = record.setdefault("hosts", {})
    section = hosts.setdefault(host_name(host), {})
    paths = section.setdefault("paths", {})
    for key, value in values.items():
        value = (value or "").strip()
        if value:
            paths[key] = value
        else:
            paths.pop(key, None)
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=".WORKBENCH-STUDIO-", suffix=".json", dir=target.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(record, indent=2) + "\n")
        os.replace(temp, target)
    except BaseException:
        Path(temp).unlink(missing_ok=True)
        raise
    return report(target, host, environ=environ)
