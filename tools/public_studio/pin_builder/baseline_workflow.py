#!/usr/bin/env python3
"""Prepare, build, and smoke-test a PSXRecomp baseline without title rules."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import time
import tomllib
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from . import chd_intake, local_input_binding, m3u_intake
except ImportError:
    import local_input_binding  # type: ignore
    import chd_intake  # type: ignore
    import m3u_intake  # type: ignore


STAGES = (
    "bind_inputs", "stage_disc_input", "identify_bios", "discover_boot_executable",
    "generate_native_source", "stage_recomp_ui", "configure_offline", "build_native",
    "launch_smoke_test", "promote_canonical_project",
    "direct_launcher_smoke_test", "write_report",
)
SCPH1001_SHA256 = "71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3"
OPENBIOS_SHA256 = "fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57"
RECOMP_UI_REVISION = "47005fb27516398381f4fda88413ff74bee14842"
RECOMP_UI_LICENSE_SHA256 = "f3c52fc52e49c3d0618c115b99927ae880b2e9e9d2a08a06b949b9b4be898894"
LIBCHDR_REVISION = "6cde5348eb118da3baf94f75a69577a005a484fd"
LIBCHDR_LICENSE_SHA256 = "5838e1d0c13f30d20045e27c6c434e1851497c7ee56e7a13b5ca1e08abc9ce40"
BIOS_NAMES = {
    SCPH1001_SHA256: "SCPH1001.BIN",
    OPENBIOS_SHA256: "openbios.bin",
}
OPENBIOS_COMPARISON_ROUTES = (
    "boot", "representative_route", "save_load", "cd", "event",
)


class BaselineError(ValueError):
    """The baseline request is invalid or cannot run offline."""


class IsoFileAbsent(BaselineError):
    """A named file is not on the disc's ISO-9660 filesystem."""


# The BIOS boots cdrom:PSX.EXE;1 when a disc has no SYSTEM.CNF. Very early
# titles (King's Field, SLPS-00017, December 1994) rely on it.
BIOS_FALLBACK_BOOT = "PSX.EXE"
SYSTEM_CNF_BOOT = "SYSTEM.CNF"
FALLBACK_BOOT_SOURCE = "PSX.EXE (BIOS fallback: no SYSTEM.CNF)"


def _build_parallelism(requested: int | None) -> dict[str, int | None]:
    """Resolve a bounded numeric build-job policy before any mutation."""
    detected = os.cpu_count()
    logical_processors = detected if isinstance(detected, int) and detected > 0 else 1
    policy_maximum = min(32, logical_processors)
    if requested is None:
        resolved = min(2, logical_processors)
    else:
        if type(requested) is not int or not 1 <= requested <= policy_maximum:
            raise BaselineError(
                f"build jobs must be an integer from 1 through {policy_maximum}")
        resolved = requested
    return {
        "requested": requested,
        "resolved": resolved,
        "logical_processors": logical_processors,
        "policy_maximum": policy_maximum,
    }


def _native_build_command(cmake: Path, build: Path, jobs: int) -> list[str]:
    """Return the exact bounded command used for the native build."""
    if type(jobs) is not int or jobs < 1:
        raise BaselineError("resolved build jobs must be a positive integer")
    return [str(cmake), "--build", str(build), "--config", "Release",
            "--parallel", str(jobs)]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _project_title(project: Path) -> str:
    manifest = project / "project-manifest.toml"
    if not manifest.is_file():
        raise BaselineError("the canonical project manifest is absent")
    data = tomllib.loads(manifest.read_text(encoding="utf-8"))
    title = data.get("project", {}).get("title")
    if not isinstance(title, str) or not title.strip():
        raise BaselineError("the canonical project title is absent")
    return title.strip()


def _cue_body(cue: Path) -> Path:
    text = cue.read_text(encoding="utf-8-sig", errors="strict")
    match = re.search(r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$', text,
                      re.IGNORECASE | re.MULTILINE)
    if not match:
        raise BaselineError("the CUE does not declare one quoted BINARY file")
    body = (cue.parent / match.group(1)).resolve()
    if body.parent != cue.parent.resolve():
        raise BaselineError("the CUE data file must stay beside the selected CUE")
    if not body.is_file():
        raise BaselineError(f"the CUE data file is absent: {body}")
    return body


def _disc_body(disc: Path) -> Path:
    return _cue_body(disc) if disc.suffix.lower() == ".cue" else disc


def _sector(stream: Any, lba: int) -> bytes:
    stream.seek(lba * 2352)
    raw = stream.read(2352)
    if len(raw) != 2352:
        raise BaselineError("the disc image ended before the ISO record")
    if raw[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
        offset = 24 if raw[15] == 2 else 16
        return raw[offset:offset + 2048]
    return raw[:2048]


def _iso_file(disc: Path, iso_path: str) -> bytes:
    body = _disc_body(disc)
    with body.open("rb") as stream:
        pvd = _sector(stream, 16)
        if pvd[1:6] != b"CD001":
            raise BaselineError("the disc does not contain an ISO-9660 primary volume")
        root = pvd[156:]
        extent = int.from_bytes(root[2:6], "little")
        size = int.from_bytes(root[10:14], "little")
        parts = [part for part in iso_path.replace("\\", "/").strip("/").split("/") if part]
        for part_index, part in enumerate(parts):
            directory = b"".join(_sector(stream, lba) for lba in range(
                extent, extent + (size + 2047) // 2048))[:size]
            position = 0
            match = None
            while position < len(directory):
                length = directory[position]
                if length == 0:
                    position = (position // 2048 + 1) * 2048
                    continue
                record = directory[position:position + length]
                name_length = record[32]
                name = record[33:33 + name_length].decode("ascii", errors="replace")
                if name.split(";", 1)[0].upper() == part.upper():
                    match = record
                    break
                position += length
            if match is None:
                raise IsoFileAbsent(f"ISO file is absent: {iso_path}")
            extent = int.from_bytes(match[2:6], "little")
            size = int.from_bytes(match[10:14], "little")
            is_directory = bool(match[25] & 2)
            if part_index < len(parts) - 1 and not is_directory:
                raise BaselineError(f"ISO path component is not a directory: {part}")
        return b"".join(_sector(stream, lba) for lba in range(
            extent, extent + (size + 2047) // 2048))[:size]


def _boot_executable(disc: Path) -> str:
    return _boot_source(disc)[0]


def _boot_source(disc: Path) -> tuple[str, str]:
    """(boot path, where it came from), following the BIOS boot rule."""
    try:
        data = _iso_file(disc, "SYSTEM.CNF")
    except IsoFileAbsent:
        try:
            _iso_file(disc, BIOS_FALLBACK_BOOT)
        except IsoFileAbsent:
            raise BaselineError("the disc has no SYSTEM.CNF and no PSX.EXE, "
                                "so the BIOS has nothing to boot") from None
        return BIOS_FALLBACK_BOOT, FALLBACK_BOOT_SOURCE
    text = data.decode("ascii", errors="replace")
    match = re.search(r"BOOT\s*=\s*cdrom:\\*([^;\r\n]+)", text, re.IGNORECASE)
    if not match:
        raise BaselineError("SYSTEM.CNF does not contain a BOOT path")
    return match.group(1).replace("\\", "/").lstrip("/"), SYSTEM_CNF_BOOT


def volume_serial(volume: str) -> str | None:
    """SLPS-00017 from a volume identifier that is itself a disc serial."""
    match = re.fullmatch(r"(S[A-Z]{3})[-_ ]?(\d{3})\.?(\d{2})", volume.strip().upper())
    return f"{match.group(1)}-{match.group(2)}{match.group(3)}" if match else None


def disc_boot_identity(disc: Path) -> dict[str, Any]:
    boot, source = _boot_source(disc)
    body = _iso_file(disc, boot)
    serial, serial_source = _disc_serial(boot), "boot executable name"
    if serial is None and source == FALLBACK_BOOT_SOURCE:
        # PSX.EXE names no serial; such discs carry it as the volume id.
        serial = volume_serial(_disc_volume_id(disc))
        serial_source = "volume identifier" if serial else None
    return {
        "path": boot,
        "serial": serial,
        "length": len(body),
        "sha256": hashlib.sha256(body).hexdigest(),
        "boot_source": source,
        "serial_source": serial_source,
    }


def _disc_volume_id(disc: Path) -> str:
    body = _disc_body(disc)
    with body.open("rb") as stream:
        pvd = _sector(stream, 16)
    if pvd[1:6] != b"CD001":
        raise BaselineError("the disc does not contain an ISO-9660 primary volume")
    return pvd[40:72].decode("ascii", errors="replace").strip(" \0")


def _project_title_declaration(value: str | None) -> str:
    if value is None:
        raise BaselineError("an explicit project title is required for a new scaffold")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise BaselineError("the explicit project title contains a control character")
    title = value.strip()
    if not title:
        raise BaselineError("an explicit project title is required for a new scaffold")
    if len(title) > 160:
        raise BaselineError("the explicit project title is too long")
    if not any(character.isalnum() for character in title):
        raise BaselineError("the explicit project title is not human-readable")
    return title


def _disc_serial(boot: str) -> str | None:
    name = Path(boot).name.upper()
    match = re.fullmatch(r"(S[A-Z]{3})_(\d{3})\.(\d{2})(?:;1)?", name)
    return f"{match.group(1)}-{match.group(2)}{match.group(3)}" if match else None


def _scaffold_identity(disc: Path, canonical_project: Path,
                       project_title: str | None,
                       multi_disc_receipt: Path | None = None) -> dict[str, Any]:
    boot = _boot_executable(disc)
    volume = _disc_volume_id(disc)
    slug = canonical_project.name.lower()
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", slug):
        raise BaselineError("the canonical project folder needs a lowercase slug")
    title = _project_title_declaration(project_title) if project_title is not None else None
    multi = _multi_disc_context(multi_disc_receipt, disc) if multi_disc_receipt else None
    serials = ([item["boot_executable"]["serial"] for item in multi["discs"]]
               if multi else [_disc_serial(boot)])
    return {
        "boot_executable": boot,
        "canonical_slug": slug,
        "disc_serial": _disc_serial(boot),
        "disc_count": multi["disc_count"] if multi else 1,
        "disc_serials": [serial for serial in serials if serial],
        "project_title": title,
        "project_title_source": "explicit" if title is not None else "required",
        "volume_identifier": volume,
    }


def scaffold_project(disc: Path, canonical_project: Path, project_title: str | None,
                     *, _identity_project: Path | None = None,
                     multi_disc_receipt: Path | None = None) -> dict[str, Any]:
    """Create a safe canonical scaffold from local disc identity."""
    disc = disc.resolve()
    canonical_project = canonical_project.resolve()
    if not disc.is_file():
        raise BaselineError("the selected disc is absent")
    if not canonical_project.is_dir():
        raise BaselineError("the canonical project directory is absent")
    if any(canonical_project.iterdir()):
        raise BaselineError("the canonical project directory is not empty")
    identity = _scaffold_identity(
        disc, _identity_project or canonical_project,
        _project_title_declaration(project_title), multi_disc_receipt)
    boot, slug, title = (
        identity["boot_executable"], identity["canonical_slug"],
        identity["project_title"])
    serials = identity["disc_serials"]
    docs = canonical_project / "docs"
    docs.mkdir()
    manifest = (
        "[project]\n"
        f"title = {json.dumps(title, ensure_ascii=False)}\n"
        f'slug = "{slug}"\n'
        'region = "selected local disc; identity pending census"\n'
        'revision = "pending census"\n'
        'track = "recompilation"\n'
        'intended_audience = "research"\n'
        "commercial_use_possible = false\n\n"
        "[retail_identity]\n"
        f"disc_count = {identity['disc_count']}\n"
        f"serials = {json.dumps(serials)}\n"
        f'executable_paths = ["{boot}"]\n'
        'executable_sha256 = ""\n'
        'load_address = ""\n'
        'entry_point = ""\n\n'
        "[framework]\n"
        'name = "PSXRecomp offline baseline"\n'
        'repository = "https://github.com/mstan/psxrecomp"\n'
        'commit = "pinned by the PSX Workbench package"\n'
        'license = "PolyForm Noncommercial 1.0.0"\n\n'
        "[limits]\n"
        "maximum_local_generated_gib = 20\n"
        "maximum_single_trace_gib = 8\n\n"
        "[paths]\n"
        'local_context = ".local-context"\n'
        'generated = "out/generated"\n'
        'captures = "out/captures"\n'
        'traces = "out/traces"\n'
        'packages = "dist"\n')
    readme = (
        f"# {title} PSX port\n\n"
        f"This project is an offline PSXRecomp baseline for {title}.\n\n"
        "The first Workbench run will identify the selected retail revision. "
        "It will build and promote a local executable from the user-owned disc and BIOS.\n\n"
        "Generated code, retail data, the BIOS, memory cards, and local evidence stay outside Git.\n\n"
        "## Current status\n\n"
        "This project has a canonical scaffold. It does not yet have a qualified baseline, "
        "full-game validation, enhancements, or release status.\n")
    feasibility = (
        f"# {title} feasibility assessment\n\n"
        "## Identity\n\n"
        f"The selected disc boots `{boot}`. A baseline run must record the exact retail identity.\n\n"
        "## Current scope\n\n"
        "The current target is a launchable offline baseline. This target does not prove "
        "full-game correctness, code-coverage closure, enhancements, or release status.\n\n"
        "## First gate\n\n"
        "Workbench must generate, build, promote, and launch the baseline without a "
        "title-specific correction.\n")
    (canonical_project / "project-manifest.toml").write_text(manifest, encoding="utf-8")
    (canonical_project / "README.md").write_text(readme, encoding="utf-8")
    (docs / "FEASIBILITY.md").write_text(feasibility, encoding="utf-8")
    (canonical_project / ".gitignore").write_text(
        ".workbench-private/\nwork_dir*/\n.local-context/\nout/\ndist/\n",
        encoding="utf-8")
    validated_title = _project_title(canonical_project)
    return {
        "schema_version": 1, "created": True, "safe_scaffold": True,
        "title": validated_title, "slug": slug, "boot_executable": boot,
        "identity": identity,
        "canonical_project": str(canonical_project),
        "files": [".gitignore", "README.md", "docs/FEASIBILITY.md",
                  "project-manifest.toml"],
    }


def _tool(value: Path | None, package_root: Path, relative: str) -> Path:
    return (value or (package_root / relative)).resolve()


def _openbios_qualification(canonical_project: Path, disc_sha256: str | None,
                            openbios_sha256: str | None) -> dict[str, Any]:
    """Validate the maintained, title-specific OpenBIOS comparison receipt."""
    path = canonical_project / "openbios-qualification.json"
    result: dict[str, Any] = {
        "path": str(path), "qualified": False,
        "required_routes": list(OPENBIOS_COMPARISON_ROUTES),
        "reason": "title-specific comparison receipt is absent",
    }
    if not path.is_file():
        return result
    try:
        receipt = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        result["reason"] = f"comparison receipt is invalid: {error}"
        return result
    if receipt.get("schema_version") != 1:
        result["reason"] = "comparison receipt schema is not version 1"
        return result
    if receipt.get("disc_sha256") != disc_sha256:
        result["reason"] = "comparison receipt disc hash does not match"
        return result
    if receipt.get("openbios_sha256") != openbios_sha256:
        result["reason"] = "comparison receipt OpenBIOS hash does not match"
        return result
    if receipt.get("reference_bios_sha256") != SCPH1001_SHA256:
        result["reason"] = "comparison receipt lacks the qualified SCPH1001 reference"
        return result
    comparisons = receipt.get("comparisons")
    if not isinstance(comparisons, dict):
        result["reason"] = "comparison receipt has no route results"
        return result
    for route in OPENBIOS_COMPARISON_ROUTES:
        item = comparisons.get(route)
        evidence = item.get("evidence_sha256") if isinstance(item, dict) else None
        if (not isinstance(item, dict) or item.get("status") != "pass" or
                not isinstance(evidence, str) or
                re.fullmatch(r"[0-9a-f]{64}", evidence) is None):
            result["reason"] = f"comparison route is not proven: {route}"
            return result
    result.update({
        "qualified": True,
        "reason": "all title-specific OpenBIOS comparison routes passed",
        "receipt_sha256": _sha256(path),
        "qualified_at_utc": receipt.get("qualified_at_utc"),
    })
    return result


def _enable_recomp_ui(source: Path, recomp_ui: Path) -> None:
    """Vendor the pinned launcher and enable the generated project's UI seam."""
    revision_path = recomp_ui / "WORKBENCH_REVISION"
    if (not revision_path.is_file() or
            revision_path.read_text(encoding="ascii").strip() != RECOMP_UI_REVISION):
        raise BaselineError("the bundled recomp-ui revision is not qualified")
    cmake_path = source / "CMakeLists.txt"
    text = cmake_path.read_text(encoding="utf-8")
    forced_off = 'set(PSX_RECOMP_UI OFF CACHE BOOL "" FORCE)'
    if text.count(forced_off) != 1:
        raise BaselineError("generated project has an unknown recomp-ui gate")
    text = text.replace(forced_off, 'set(PSX_RECOMP_UI ON CACHE BOOL "" FORCE)')
    cmake_path.write_text(text, encoding="utf-8")
    destination = source / "recomp-ui"
    if destination.exists():
        raise BaselineError("generated recomp-ui destination already exists")
    shutil.copytree(recomp_ui, destination, ignore=shutil.ignore_patterns(".git"))
    if _sha256(destination / "LICENSE") != RECOMP_UI_LICENSE_SHA256:
        raise BaselineError("the staged recomp-ui license hash changed")
    header = (destination / "src" / "recomp_launcher.h").read_text(
        encoding="utf-8")
    if "int            allow_hybrid" not in header:
        runtime_main = source / "psxrecomp" / "runtime" / "src" / "main.cpp"
        runtime_text = runtime_main.read_text(encoding="utf-8")
        obsolete = "            gi.allow_hybrid         = ctrl_allow_hybrid ? 1 : 0;\n"
        if runtime_text.count(obsolete) != 1:
            raise BaselineError(
                "the generated runtime has an unknown recomp-ui controller ABI")
        runtime_main.write_text(runtime_text.replace(obsolete, ""), encoding="utf-8")


def _admit_lower_ram_dynamic_code(source: Path) -> None:
    """Admit a real dispatch into decodable RAM outside the static game text."""
    path = source / "psxrecomp" / "runtime" / "src" / "dirty_ram_interp.c"
    text = path.read_text(encoding="utf-8")
    old = """        if (phys < (2u * 1024u * 1024u) &&
            phys >= g_overlay_region_floor &&
            dirty_ram_word_looks_decodable(fetch_word(phys))) {
"""
    new = """        int outside_static_game_text = 0;
#ifdef PSX_HAS_GAME_DISPATCH
        outside_static_game_text =
            phys >= DIRTY_RAM_KERNEL_WINDOW_END &&
            !psx_game_address_in_text(addr);
#endif
        if (phys < (2u * 1024u * 1024u) &&
            (phys >= g_overlay_region_floor || outside_static_game_text) &&
            dirty_ram_word_looks_decodable(fetch_word(phys))) {
"""
    if text.count(old) != 1:
        raise BaselineError("the generated runtime has an unknown dynamic-code gate")
    path.write_text(text.replace(old, new), encoding="utf-8")


def _disc_stage_paths(plan: dict[str, Any], private: Path) -> tuple[Path, list[tuple[Path, Path, str]]]:
    """Map authoritative disc inputs to project-local generator paths."""
    inputs = plan["inputs"]
    disc = Path(inputs["disc"]["path"])
    body_value = inputs["disc_body"]["path"]
    body = Path(body_value) if body_value else None
    destination = private / "disc"
    staged_disc = destination / disc.name
    copies = [(disc, staged_disc, inputs["disc"]["sha256"])]
    if body is not None and str(body) != str(disc):
        copies.append((body, destination / body.name,
                       inputs["disc_body"]["sha256"]))
    subchannel = inputs.get("subchannel", {})
    if subchannel.get("status") == "present":
        source_sbi = Path(subchannel["source"]["path"])
        copies.append((source_sbi, destination / f"{disc.stem}.sbi",
                       subchannel["source"]["sha256"]))
    return staged_disc, copies


def _stage_disc(plan: dict[str, Any], private: Path) -> Path:
    """Copy a disc and its CUE payload to a local generator boundary."""
    staged_disc, copies = _disc_stage_paths(plan, private)
    staged_disc.parent.mkdir()
    for source, destination, expected_sha256 in copies:
        shutil.copyfile(source, destination)
        if _sha256(destination) != expected_sha256:
            raise BaselineError(f"staged disc hash changed: {source.name}")
    if staged_disc.suffix.lower() == ".cue":
        staged_body = _cue_body(staged_disc)
        expected_body = plan["inputs"]["disc_body"]["sha256"]
        if _sha256(staged_body) != expected_body:
            raise BaselineError("staged CUE payload hash changed")
    return staged_disc


def _multi_disc_context(receipt_path: Path, boot_disc: Path) -> dict[str, Any]:
    receipt_path = receipt_path.resolve()
    value = m3u_intake.validate_receipt(receipt_path)
    root = receipt_path.parent.resolve()
    first = (root / Path(*Path(value["discs"][0]["cue"]["path"]).parts)).resolve()
    if first != boot_disc.resolve():
        raise BaselineError("the selected boot disc is not disc 1 of the M3U receipt")
    actual = disc_boot_identity(boot_disc)
    for key in ("path", "serial", "length", "sha256"):
        if value["resident_executable"].get(key) != actual.get(key):
            raise BaselineError("the M3U resident executable identity changed")
    return value | {
        "receipt_path": str(receipt_path),
        "receipt_sha256": _sha256(receipt_path),
    }


def preflight(project: Path, disc: Path, bios: Path, package_root: Path,
              canonical_project: Path,
              *, psxrecomp: Path | None = None, cmake: Path | None = None,
              ninja: Path | None = None, sdl_source: Path | None = None,
              libchdr_source: Path | None = None,
              toolchain_bin: Path | None = None,
              recomp_ui: Path | None = None,
              multi_disc_receipt: Path | None = None,
              build_jobs: int | None = None,
              _allow_derived_project: bool = False,
              _scaffold_preview: dict[str, Any] | None = None) -> dict[str, Any]:
    """Return a complete plan. Do not create or change project files."""
    project, disc, bios, package_root, canonical_project = (
        item.resolve() for item in (
            project, disc, bios, package_root, canonical_project))
    build_parallelism = _build_parallelism(build_jobs)
    tools = {
        "psxrecomp": _tool(psxrecomp, package_root, "native/psxrecomp/psxrecomp.exe"),
        "cmake": _tool(cmake, package_root, "native/cmake/bin/cmake.exe"),
        "ninja": _tool(ninja, package_root, "native/ninja/ninja.exe"),
        "sdl_source": _tool(sdl_source, package_root, "native/sdl"),
        "libchdr_source": _tool(libchdr_source, package_root, "native/libchdr"),
        "toolchain_bin": _tool(toolchain_bin, package_root, "native/toolchain/bin"),
        "recomp_ui": _tool(recomp_ui, package_root, "native/recomp-ui"),
    }
    missing: list[str] = []
    if not project.is_dir() and not _allow_derived_project:
        missing.append("project directory")
    elif project.is_dir() and any(project.iterdir()):
        missing.append("empty project directory")
    elif project.is_dir() and not os.access(project, os.W_OK):
        missing.append("writable project directory")
    if not disc.is_file(): missing.append("local disc")
    if not bios.is_file(): missing.append("local BIOS")
    if not canonical_project.is_dir(): missing.append("canonical project directory")
    elif _scaffold_preview is None:
        for relative in ("project-manifest.toml", "README.md", "docs/FEASIBILITY.md"):
            if not (canonical_project / relative).is_file():
                missing.append(f"canonical project {relative}")
    for name in ("psxrecomp", "cmake", "ninja"):
        if not tools[name].is_file(): missing.append(f"bundled {name}")
    if not (tools["sdl_source"] / "CMakeLists.txt").is_file():
        missing.append("bundled SDL source")
    libchdr_revision = tools["libchdr_source"] / "WORKBENCH_REVISION"
    if (not (tools["libchdr_source"] / "CMakeLists.txt").is_file() or
            not libchdr_revision.is_file() or
            libchdr_revision.read_text(encoding="ascii").strip() != LIBCHDR_REVISION or
            not (tools["libchdr_source"] / "LICENSE.txt").is_file() or
            _sha256(tools["libchdr_source"] / "LICENSE.txt") !=
            LIBCHDR_LICENSE_SHA256):
        missing.append("qualified bundled libchdr source")
    if not tools["toolchain_bin"].is_dir(): missing.append("bundled compiler toolchain")
    if not (tools["recomp_ui"] / "recomp_ui.cmake").is_file():
        missing.append("bundled recomp-ui source")
    revision_path = tools["recomp_ui"] / "WORKBENCH_REVISION"
    if (not revision_path.is_file() or
            revision_path.read_text(encoding="ascii").strip() != RECOMP_UI_REVISION):
        missing.append("qualified recomp-ui revision")
    if (tools["recomp_ui"] / "LICENSE").is_file():
        if _sha256(tools["recomp_ui"] / "LICENSE") != RECOMP_UI_LICENSE_SHA256:
            missing.append("qualified recomp-ui license")
    else:
        missing.append("bundled recomp-ui license")
    multi_disc = None
    if multi_disc_receipt is not None:
        try:
            multi_disc = _multi_disc_context(multi_disc_receipt, disc)
        except (BaselineError, m3u_intake.M3UIntakeError, OSError,
                KeyError, TypeError, json.JSONDecodeError) as error:
            missing.append(f"valid M3U multi-disc receipt: {error}")
    disc_body = _disc_body(disc) if disc.is_file() else None
    disc_hash = _sha256(disc) if disc.is_file() else None
    body_hash = _sha256(disc_body) if disc_body else None
    try:
        subchannel = chd_intake.subchannel_companion(disc) if disc.is_file() else {
            "status": "absent", "qualified": False,
            "reason": "disc input is absent"}
    except chd_intake.IntakeError as error:
        subchannel = {"status": "invalid", "qualified": False, "reason": str(error)}
        missing.append(f"valid SBI subchannel companion: {error}")
    bios_hash = _sha256(bios) if bios.is_file() else None
    canonical = BIOS_NAMES.get(bios_hash or "")
    if bios.is_file() and canonical is None:
        missing.append("qualified BIOS identity")
    boot = _boot_executable(disc) if disc.is_file() else None
    title = _scaffold_preview.get("project_title") if _scaffold_preview else None
    manifest = canonical_project / "project-manifest.toml"
    if manifest.is_file():
        try:
            title = _project_title(canonical_project)
        except (BaselineError, tomllib.TOMLDecodeError) as error:
            missing.append(f"canonical project manifest is invalid: {error}")
    openbios_qualification = _openbios_qualification(
        canonical_project, disc_hash, bios_hash)
    bios_backend = "unknown"
    if bios_hash == OPENBIOS_SHA256:
        bios_backend = "openbios-shipped-default"
        if not openbios_qualification["qualified"]:
            missing.append("title-specific OpenBIOS qualification")
            bios_backend = "retail-bios-required"
    elif bios_hash == SCPH1001_SHA256:
        bios_backend = "retail-scph1001-local-reference"
    return {
        "schema_version": 1,
        "mode": "dry-run",
        "mutated_project": False,
        "ready": not missing,
        "inputs": {
            "project": str(project),
            "project_title": title,
            "disc": {"path": str(disc), "sha256": disc_hash},
            "disc_body": {"path": str(disc_body) if disc_body else None,
                          "sha256": body_hash},
            "subchannel": subchannel,
            "bios": {"path": str(bios), "sha256": bios_hash},
            "multi_disc": multi_disc,
        },
        "derived": {
            "project_id": project.name,
            "title": title,
            "canonical_project": str(canonical_project),
            "project_will_create": _allow_derived_project and not project.exists(),
            "scaffold_will_create": (
                _scaffold_preview is not None and
                _scaffold_preview.get("project_title") is not None),
            "scaffold_preview": _scaffold_preview,
            "project_title": title,
            "project_title_source": (
                _scaffold_preview.get("project_title_source")
                if _scaffold_preview else "canonical-manifest" if title else None),
            "canonical_slug": (
                _scaffold_preview.get("canonical_slug")
                if _scaffold_preview else canonical_project.name),
            "volume_identifier": (
                _scaffold_preview.get("volume_identifier")
                if _scaffold_preview else None),
            "disc_serial": (
                _scaffold_preview.get("disc_serial")
                if _scaffold_preview else _disc_serial(boot) if boot else None),
            "boot_executable": boot,
            "canonical_bios_name": canonical,
            "bios_backend": bios_backend,
            "openbios_qualification": openbios_qualification,
            "recomp_ui": {
                "revision": RECOMP_UI_REVISION,
                "license": "MIT",
                "license_sha256": RECOMP_UI_LICENSE_SHA256,
                "source": str(tools["recomp_ui"]),
            },
            "native_source": str(project / "native-source"),
            "build_directory": str(project / "native-build"),
            "report": str(project / "baseline-report.json"),
        },
        "tools": {name: str(path) for name, path in tools.items()},
        "stages": list(STAGES),
        "defaults": {
            "build_type": "Release",
            "ui": True,
            "network": "denied by local source override",
            "renderer": "opengl (generator default; no enhancement)",
            "vulkan_discovery": False,
            "player_launch_mode": "recomp-ui",
            "automated_launch_mode": "--headless --no-launcher bounded smoke test",
            "smoke_seconds": 8,
            "compiler_cache": "project-private",
            "build_jobs": build_parallelism,
        },
        "missing_inputs": missing,
    }


def _write_launcher(canonical_project: Path, title: str) -> Path:
    tools = canonical_project / "tools"
    tools.mkdir(exist_ok=True)
    script = tools / "launch-local.ps1"
    script.write_text(r'''param([int]$SmokeSeconds = 0)
$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$PrivateRoot = Join-Path $ProjectRoot '.workbench-private'
$CurrentPath = Join-Path $PrivateRoot 'current.json'
if (-not (Test-Path -LiteralPath $CurrentPath -PathType Leaf)) {
    throw 'No promoted local baseline exists. Run PSX Workbench first.'
}
$Current = Get-Content -LiteralPath $CurrentPath -Raw | ConvertFrom-Json
$ReceiptPath = Join-Path $PrivateRoot $Current.receipt
$Receipt = Get-Content -LiteralPath $ReceiptPath -Raw | ConvertFrom-Json
$RunRoot = Split-Path -Parent $ReceiptPath
$Executable = Join-Path $RunRoot $Receipt.executable
$GameConfig = Join-Path $RunRoot $Receipt.game_config
$Disc = Join-Path $RunRoot $Receipt.disc
$Bios = if ($Receipt.bios.storage -eq 'promoted') {
    Join-Path $RunRoot $Receipt.bios.path
} else {
    [System.IO.Path]::GetFullPath([string]$Receipt.bios.path)
}
$State = Join-Path $RunRoot 'state'
New-Item -ItemType Directory -Path $State -Force | Out-Null
foreach ($Required in @($Executable, $GameConfig, $Disc, $Bios)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "A promoted launch file is absent: $Required"
    }
}
function Get-Sha256([string]$Path) {
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    $FileStream = [System.IO.File]::OpenRead($Path)
    try {
        return ([System.BitConverter]::ToString($Hasher.ComputeHash($FileStream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $FileStream.Dispose()
        $Hasher.Dispose()
    }
}
$ActualHash = Get-Sha256 $Executable
if ($ActualHash -ne $Receipt.executable_sha256) {
    throw 'The promoted executable hash changed.'
}
if ((Get-Sha256 $Bios) -ne $Receipt.bios.sha256) {
    throw 'The selected BIOS hash changed.'
}
$StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
$StartInfo.FileName = $Executable
$StartInfo.WorkingDirectory = Split-Path -Parent $GameConfig
$StartInfo.UseShellExecute = $false
$LaunchArguments = @('--bios', $Bios, '--game', $GameConfig, '--disc', $Disc,
    '--memcard-dir', $State)
if ($SmokeSeconds -gt 0) {
    $LaunchArguments += @('--headless', '--no-launcher')
} else {
    $LaunchArguments += '--launcher'
}
$LaunchArguments = $LaunchArguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
$StartInfo.Arguments = $LaunchArguments -join ' '
$Process = [System.Diagnostics.Process]::Start($StartInfo)
if ($SmokeSeconds -gt 0) {
    if ($Process.WaitForExit($SmokeSeconds * 1000)) {
        throw "The promoted launcher exited during the $SmokeSeconds-second smoke test."
    }
    $Process.Kill()
    $Process.WaitForExit()
}
[ordered]@{ status = 'launched'; process_id = $Process.Id; smoke_seconds = $SmokeSeconds } |
    ConvertTo-Json -Compress
'''.replace("\n", "\r\n"), encoding="ascii", newline="")
    safe_title = re.sub(r'[^A-Za-z0-9 ._()-]', '', title).strip()
    launcher = canonical_project / f"Play {safe_title}.cmd"
    body = (
        "@echo off\r\n"
        "setlocal\r\n"
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File "
        '"%~dp0tools\\launch-local.ps1"\r\n'
        "if errorlevel 1 pause\r\n"
    )
    launcher.write_bytes(body.encode("ascii"))
    return launcher


def promote(project: Path, canonical_project: Path, report: dict[str, Any],
            started_at_utc: str, elapsed_seconds: float) -> dict[str, Any]:
    """Export a private local runtime and a safe maintained launcher."""
    canonical_project = canonical_project.resolve()
    title = _project_title(canonical_project)
    ignore = canonical_project / ".gitignore"
    ignore_text = ignore.read_text(encoding="utf-8") if ignore.is_file() else ""
    if ".workbench-private/" not in ignore_text.splitlines():
        with ignore.open("a", encoding="utf-8", newline="") as stream:
            if ignore_text and not ignore_text.endswith(("\n", "\r")):
                stream.write("\n")
            stream.write(".workbench-private/\n")
    run_id = re.sub(r"[^0-9]", "", started_at_utc)[:14] + "Z"
    private_root = canonical_project / ".workbench-private"
    run_root = private_root / "runs" / run_id
    if run_root.exists():
        raise BaselineError("the canonical promotion namespace already exists")
    run_root.mkdir(parents=True)
    executable = Path(report["artifacts"]["executable"])
    game_config = Path(report["artifacts"]["game_config"])
    source = run_root / "runtime"
    inputs = run_root / "inputs"
    source.mkdir()
    inputs.mkdir()
    copied_executable = source / executable.name
    copied_config = source / "game.toml"
    shutil.copy2(executable, copied_executable)
    shutil.copy2(game_config, copied_config)
    built_assets = executable.parent / "assets"
    if not built_assets.is_dir():
        raise BaselineError("the built recomp-ui assets are absent")
    shutil.copytree(built_assets, source / "assets")
    notices = source / "licenses"
    notices.mkdir()
    ui_license = game_config.parent / "recomp-ui" / "LICENSE"
    if (not ui_license.is_file() or
            _sha256(ui_license) != RECOMP_UI_LICENSE_SHA256):
        raise BaselineError("the built recomp-ui license provenance is invalid")
    shutil.copy2(ui_license, notices / "recomp-ui-MIT.txt")
    disc = Path(report["preflight"]["inputs"]["disc"]["path"])
    disc_body = Path(report["preflight"]["inputs"]["disc_body"]["path"])
    bios = Path(report["preflight"]["inputs"]["bios"]["path"])
    copied_disc = inputs / disc.name
    shutil.copy2(disc, copied_disc)
    if disc_body.resolve() != disc.resolve():
        shutil.copy2(disc_body, inputs / disc_body.name)
    subchannel = report["preflight"]["inputs"].get("subchannel", {})
    if subchannel.get("status") == "present":
        source_sbi = Path(subchannel["source"]["path"])
        copied_sbi = inputs / f"{disc.stem}.sbi"
        shutil.copy2(source_sbi, copied_sbi)
        if _sha256(copied_sbi) != subchannel["source"]["sha256"]:
            raise BaselineError("the promoted SBI subchannel hash changed")
    multi_disc = report["preflight"]["inputs"].get("multi_disc")
    promoted_multi_disc = None
    if multi_disc:
        source_root = Path(multi_disc["receipt_path"]).parent
        multi_root = inputs / "multi-disc"
        multi_root.mkdir()
        copied_discs = []
        for disc_item in multi_disc["discs"]:
            destination = multi_root / f"disc-{disc_item['index']:03d}"
            destination.mkdir()
            copied_files = []
            source_items = [disc_item["cue"], *disc_item["track_files"]]
            if disc_item.get("subchannel", {}).get("status") == "preserved":
                source_items.append(disc_item["subchannel"]["output"])
            for item in source_items:
                source_item = source_root / Path(*Path(item["path"]).parts)
                copied = destination / source_item.name
                shutil.copy2(source_item, copied)
                if _sha256(copied) != item["sha256"]:
                    raise BaselineError("a promoted multi-disc input hash changed")
                copied_files.append({
                    "path": str(copied.relative_to(run_root)).replace("\\", "/"),
                    "length": copied.stat().st_size,
                    "sha256": _sha256(copied),
                })
            copied_discs.append({
                "index": disc_item["index"],
                "source_sha256": disc_item["source"]["sha256"],
                "boot_executable": disc_item["boot_executable"],
                "files": copied_files,
            })
        promoted_multi_disc = {
            "contract": multi_disc["contract"],
            "source_receipt_sha256": multi_disc["receipt_sha256"],
            "disc_count": multi_disc["disc_count"],
            "playlist_order": [item["source_entry"] for item in multi_disc["discs"]],
            "resident_executable": multi_disc["resident_executable"],
            "discs": copied_discs,
            "disc_swap": multi_disc["disc_swap"],
        }
    bios_hash = report["preflight"]["inputs"]["bios"]["sha256"]
    bios_backend = report["preflight"]["derived"]["bios_backend"]
    if bios_hash == OPENBIOS_SHA256:
        qualification = report["preflight"]["derived"]["openbios_qualification"]
        if not qualification.get("qualified"):
            raise BaselineError("OpenBIOS cannot be promoted without title qualification")
        copied_bios = inputs / "openbios.bin"
        shutil.copy2(bios, copied_bios)
        openbios_notice = game_config.parent / "psxrecomp" / "bios" / "OpenBIOS.LICENSE"
        if not openbios_notice.is_file():
            raise BaselineError("the OpenBIOS redistribution notice is absent")
        shutil.copy2(openbios_notice, notices / "OpenBIOS-MIT.txt")
        receipt_bios = {
            "backend": bios_backend,
            "storage": "promoted",
            "path": str(copied_bios.relative_to(run_root)).replace("\\", "/"),
            "sha256": bios_hash,
            "qualification_receipt_sha256": qualification["receipt_sha256"],
        }
    elif bios_hash == SCPH1001_SHA256:
        receipt_bios = {
            "backend": bios_backend,
            "storage": "external-local-reference",
            "path": str(bios.resolve()),
            "sha256": bios_hash,
        }
    else:
        raise BaselineError("the selected BIOS is not a promotable identity")
    receipt = {
        "schema_version": 1,
        "title": title,
        "executable": str(copied_executable.relative_to(run_root)).replace("\\", "/"),
        "executable_sha256": _sha256(copied_executable),
        "game_config": str(copied_config.relative_to(run_root)).replace("\\", "/"),
        "disc": str(copied_disc.relative_to(run_root)).replace("\\", "/"),
        "multi_disc": promoted_multi_disc,
        "bios": receipt_bios,
        "ui": {
            "name": "mstan/recomp-ui",
            "revision": RECOMP_UI_REVISION,
            "license": "MIT",
            "license_sha256": RECOMP_UI_LICENSE_SHA256,
            "assets": "runtime/assets",
            "settings": ["runtime/settings.toml", "runtime/keybinds.ini",
                         "runtime/input.ini"],
        },
    }
    receipt_path = run_root / "launch.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    launcher = _write_launcher(canonical_project, title)
    current = {"schema_version": 1,
               "receipt": str(receipt_path.relative_to(private_root)).replace("\\", "/")}
    (private_root / "current.json").write_text(
        json.dumps(current, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    smoke_started_at = _utc_now()
    smoke_started = time.perf_counter()
    smoke = subprocess.run([
        "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
        str(canonical_project / "tools" / "launch-local.ps1"), "-SmokeSeconds", "8"],
        cwd=canonical_project, capture_output=True, text=True, encoding="utf-8",
        errors="replace")
    smoke_finished_at = _utc_now()
    smoke_elapsed = round(time.perf_counter() - smoke_started, 6)
    if smoke.returncode:
        smoke_log = project / "logs" / "canonical-launch-smoke.log"
        smoke_log.write_text(smoke.stdout + smoke.stderr, encoding="utf-8")
        raise BaselineError(
            "the direct canonical launcher smoke test failed; see "
            "canonical-launch-smoke.log")
    promotion = {
        "schema_version": 1,
        "status": "promoted-smoke-passed",
        "title": title,
        "launcher": launcher.name,
        "executable_sha256": receipt["executable_sha256"],
        "bios": receipt_bios,
        "ui": receipt["ui"],
        "multi_disc": promoted_multi_disc,
        "source_baseline_report_sha256": _sha256(project / "baseline-report.json"),
        "time": {
            "baseline_started_at_utc": started_at_utc,
            "promotion_finished_at_utc": smoke_finished_at,
            "baseline_build_elapsed_seconds": elapsed_seconds,
            "launcher_smoke_started_at_utc": smoke_started_at,
            "launcher_smoke_finished_at_utc": smoke_finished_at,
            "launcher_smoke_elapsed_seconds": smoke_elapsed,
            "elapsed_clock": "time.perf_counter",
        },
        "scope": {
            "runtime_smoke_passed": True,
            "gameplay_qualified": False,
            "gameplay_status": "untested",
            "full_game_correctness": False,
            "coverage_closed": False,
            "enhancements_complete": False,
            "release_ready": False,
        },
    }
    (canonical_project / "baseline-promotion.json").write_text(
        json.dumps(promotion, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return promotion


def _run(arguments: list[str], cwd: Path, log: Path,
         environment: dict[str, str]) -> None:
    result = subprocess.run(arguments, cwd=cwd, env=environment,
                            capture_output=True, text=True, encoding="utf-8",
                            errors="replace")
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise BaselineError(
            f"command failed ({result.returncode}); see {log.name}")


def _runtime_failure_detail(source: Path) -> dict[str, Any]:
    """Return a bounded, player-safe summary of a failed runtime smoke."""
    report_path = source / "psx_last_run_report.json"
    detail: dict[str, Any] = {
        "classification": "runtime-exited-during-smoke",
        "summary": "the native runtime exited during the launch smoke test",
    }
    if not report_path.is_file():
        return detail
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return detail
    entries = report.get("unknown_dispatch_tail", {}).get("entries", [])
    if not entries or not isinstance(entries[-1], dict):
        return detail
    entry = entries[-1]
    detail.update({
        "classification": "runtime-unknown-dispatch",
        "frame": entry.get("frame"),
        "address": entry.get("addr"),
        "physical_address": entry.get("phys"),
        "caller_ra": entry.get("ra"),
        "source_report": str(report_path),
        "source_report_sha256": _sha256(report_path),
    })
    config_path = source / "game.toml"
    try:
        config = tomllib.loads(config_path.read_text(encoding="utf-8"))["game"]
        text_lo = int(str(config["load_address"]), 0) & 0x1FFFFFFF
        text_hi = text_lo + int(str(config["text_size"]), 0)
        phys = int(str(entry.get("phys")), 0)
        detail["static_text"] = {
            "physical_start": f"0x{text_lo:08X}",
            "physical_end": f"0x{text_hi:08X}",
        }
        detail["below_static_text"] = phys < text_lo
    except (KeyError, OSError, TypeError, ValueError, tomllib.TOMLDecodeError):
        pass
    detail["summary"] = (
        f"runtime unknown dispatch {detail['address']} at frame "
        f"{detail['frame']} (caller {detail['caller_ra']})")
    if detail.get("below_static_text"):
        detail["diagnostic"] = (
            "target is below declared resident text; copied or relocated code is "
            "a candidate and requires runtime-write evidence before correction")
    return detail


def _bios_registry_path(path: Path | None = None) -> Path:
    if path is not None:
        return path.resolve()
    local = os.environ.get("LOCALAPPDATA")
    root = Path(local) if local else Path.home() / ".local" / "share"
    return (root / "PSXWorkbench" / "bios-library.json").resolve()


def register_retail_bios(bios: Path, registry_path: Path | None = None) -> dict[str, Any]:
    """Register one user-owned SCPH1001 image in the machine-local library."""
    bios = bios.resolve()
    if not bios.is_file():
        raise BaselineError("the selected retail BIOS is absent")
    digest = _sha256(bios)
    if digest != SCPH1001_SHA256:
        raise BaselineError("the selected file is not the qualified SCPH1001 BIOS")
    target = _bios_registry_path(registry_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "bios": {"scph1001": {"path": str(bios), "sha256": digest}},
    }
    temporary = target.with_name(target.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    os.replace(temporary, target)
    return {"registered": True, "backend": "scph1001", "sha256": digest,
            "registry": str(target)}


def _registered_retail_bios(registry_path: Path | None = None) -> Path | None:
    target = _bios_registry_path(registry_path)
    try:
        data = json.loads(target.read_text(encoding="utf-8"))
        item = data["bios"]["scph1001"]
        path = Path(item["path"]).resolve()
        if (data.get("schema_version") == 1 and
                item.get("sha256") == SCPH1001_SHA256 and path.is_file() and
                _sha256(path) == SCPH1001_SHA256):
            return path
    except (KeyError, OSError, TypeError, json.JSONDecodeError):
        pass
    return None


def _next_work_directory(canonical_project: Path) -> tuple[Path, list[str]]:
    occupied: list[str] = []
    for index in range(1, 1000):
        name = "work_dir" if index == 1 else f"work_dir_{index}"
        candidate = canonical_project / name
        if not candidate.exists():
            return candidate, occupied
        occupied.append(name)
    raise BaselineError("no free derived work directory remains")


def happy_preflight(canonical_project: Path, disc: Path, bios_backend: str,
                    package_root: Path, *, registry_path: Path | None = None,
                    project_title: str | None = None,
                    **options: Any) -> dict[str, Any]:
    """Plan the one-folder player workflow without mutating the project."""
    _build_parallelism(options.get("build_jobs"))
    canonical_project = canonical_project.resolve()
    disc = disc.resolve()
    package_root = package_root.resolve()
    if bios_backend not in ("openbios", "scph1001"):
        raise BaselineError("BIOS backend must be openbios or scph1001")
    if not canonical_project.is_dir():
        raise BaselineError("the selected canonical project folder is absent")
    work_dir, occupied = _next_work_directory(canonical_project)
    scaffold_preview = None
    manifest = canonical_project / "project-manifest.toml"
    scaffold_blocked = False
    scaffold_title_missing = False
    if not manifest.is_file():
        if any(canonical_project.iterdir()):
            scaffold_blocked = True
        elif disc.is_file():
            scaffold_preview = _scaffold_identity(
                disc, canonical_project,
                _project_title_declaration(project_title)
                if project_title is not None else None,
                options.get("multi_disc_receipt"))
            scaffold_title_missing = scaffold_preview["project_title"] is None
    if bios_backend == "openbios":
        bios = package_root / "native" / "psxrecomp" / "bios" / "openbios.bin"
        registered = True
        bios_source = "bundled redistributable image"
    else:
        selected = _registered_retail_bios(registry_path)
        registered = selected is not None
        bios = selected or package_root / "config" / "unregistered-SCPH1001.BIN"
        bios_source = "registered machine-local reference" if selected else "not registered"
    plan = preflight(
        work_dir, disc, bios, package_root, canonical_project,
        _allow_derived_project=True, _scaffold_preview=scaffold_preview, **options)
    if scaffold_blocked:
        plan["missing_inputs"].append(
            "empty canonical folder or maintained canonical manifest")
    if scaffold_title_missing:
        plan["missing_inputs"].append("explicit project title for new scaffold")
    if bios_backend == "scph1001" and not registered:
        plan["missing_inputs"] = [
            item for item in plan["missing_inputs"] if item != "local BIOS"]
        plan["missing_inputs"].append("registered SCPH1001 BIOS")
        plan["derived"]["bios_backend"] = "retail-bios-registration-required"
    plan["ready"] = not plan["missing_inputs"]
    plan["happy_path"] = {
        "canonical_project": str(canonical_project),
        "derived_work_directory": str(work_dir),
        "occupied_work_directories": occupied,
        "bios_selection": bios_backend,
        "bios_source": bios_source,
        "bios_registered": registered,
        "scaffold_will_create": scaffold_preview is not None and not scaffold_title_missing,
        "scaffold_preview": scaffold_preview,
    }
    return plan


def _create_scaffold_atomically(disc: Path, canonical_project: Path,
                                project_title: str,
                                multi_disc_receipt: Path | None = None) -> None:
    if any(canonical_project.iterdir()):
        raise BaselineError("the canonical folder changed before scaffold creation")
    token = uuid.uuid4().hex
    staged = canonical_project.parent / f".{canonical_project.name}.scaffold-{token}"
    empty_backup = canonical_project.parent / f".{canonical_project.name}.empty-{token}"
    staged.mkdir()
    try:
        scaffold_project(disc, staged, project_title,
                         _identity_project=canonical_project,
                         multi_disc_receipt=multi_disc_receipt)
        canonical_project.replace(empty_backup)
        try:
            staged.replace(canonical_project)
        except BaseException:
            empty_backup.replace(canonical_project)
            raise
        empty_backup.rmdir()
    finally:
        if staged.exists() and not any(staged.iterdir()):
            staged.rmdir()


def happy_run(canonical_project: Path, disc: Path, bios_backend: str,
              package_root: Path, *, registry_path: Path | None = None,
              project_title: str | None = None,
              **options: Any) -> dict[str, Any]:
    """Create derived namespaces, then execute the authoritative baseline."""
    plan = happy_preflight(canonical_project, disc, bios_backend, package_root,
                           registry_path=registry_path,
                           project_title=project_title, **options)
    if not plan["ready"]:
        raise BaselineError("preflight is not ready: " +
                            ", ".join(plan["missing_inputs"]))
    canonical = Path(plan["happy_path"]["canonical_project"])
    if plan["happy_path"]["scaffold_will_create"]:
        title = plan["happy_path"]["scaffold_preview"]["project_title"]
        _create_scaffold_atomically(disc.resolve(), canonical, title,
                                    options.get("multi_disc_receipt"))
    ignore = canonical / ".gitignore"
    ignore_text = ignore.read_text(encoding="utf-8") if ignore.is_file() else ""
    if "work_dir*/" not in ignore_text.splitlines():
        with ignore.open("a", encoding="utf-8", newline="") as stream:
            if ignore_text and not ignore_text.endswith(("\n", "\r")):
                stream.write("\n")
            stream.write("work_dir*/\n")
    project = Path(plan["happy_path"]["derived_work_directory"])
    project.mkdir(exist_ok=False)
    bios = Path(plan["inputs"]["bios"]["path"])
    return run(project, disc, bios, package_root, canonical, **options)


def run(project: Path, disc: Path, bios: Path, package_root: Path,
        canonical_project: Path, **options: Any) -> dict[str, Any]:
    """Run the frozen preflight plan from a new, empty project namespace."""
    started_at_utc = _utc_now()
    started = time.perf_counter()
    plan = preflight(project, disc, bios, package_root, canonical_project, **options)
    if not plan["ready"]:
        raise BaselineError("preflight is not ready: " + ", ".join(plan["missing_inputs"]))
    project = project.resolve()
    if any(project.iterdir()):
        raise BaselineError("project directory must be empty; use a fresh namespace")
    tools = {name: Path(value) for name, value in plan["tools"].items()}
    logs = project / "logs"
    private = project / ".workbench-private"
    logs.mkdir()
    private.mkdir()
    binding, receipt = local_input_binding.create_binding(
        project, disc, bios,
        disc_sha256=plan["inputs"]["disc"]["sha256"],
        bios_sha256=plan["inputs"]["bios"]["sha256"])
    (project / "local-input-binding.json").write_text(
        json.dumps(binding, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (project / "local-input-receipt.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    staged_bios = private / plan["derived"]["canonical_bios_name"]
    shutil.copy2(bios, staged_bios)
    staged_disc = _stage_disc(plan, private)
    source = project / "native-source"
    environment = os.environ.copy()
    environment.pop("CMAKE_BUILD_PARALLEL_LEVEL", None)
    environment["CCACHE_DIR"] = str(private / "ccache")
    environment["CCACHE_BASEDIR"] = str(project)
    environment["HTTP_PROXY"] = "http://127.0.0.1:9"
    environment["HTTPS_PROXY"] = "http://127.0.0.1:9"
    environment["ALL_PROXY"] = "http://127.0.0.1:9"
    environment["NO_PROXY"] = ""
    environment["PATH"] = os.pathsep.join((
        str(tools["toolchain_bin"]), str(tools["ninja"].parent),
        str(tools["cmake"].parent), environment.get("PATH", "")))
    _run([str(tools["psxrecomp"]), "build", "--disc", str(staged_disc),
          "--bios", str(staged_bios), "--output", str(source)],
         project, logs / "generate.log", environment)
    _admit_lower_ram_dynamic_code(source)
    _enable_recomp_ui(source, tools["recomp_ui"])
    build = project / "native-build"
    _run([str(tools["cmake"]), "-S", str(source), "-B", str(build),
          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPSX_RECOMP_UI=ON",
          f"-DRECOMP_UI_ROOT={source / 'recomp-ui'}", "-DRECOMP_UI_SDL3=ON",
          "-DPSX_SDL3_FETCH=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE",
          "-DPSX_ENABLE_VULKAN=OFF", "-DSDL_VULKAN=OFF",
          "-DSDL_RENDER_VULKAN=OFF",
          f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}",
          f"-DFETCHCONTENT_SOURCE_DIR_SDL3={tools['sdl_source']}",
          f"-DFETCHCONTENT_SOURCE_DIR_PSX_LIBCHDR={tools['libchdr_source']}"],
         project, logs / "configure.log", environment)
    _run(_native_build_command(
        tools["cmake"], build, plan["defaults"]["build_jobs"]["resolved"]),
         project, logs / "build.log", environment)
    executables = sorted(path for path in build.glob("*.exe") if path.is_file())
    if len(executables) != 1:
        raise BaselineError(f"expected one native executable, found {len(executables)}")
    executable = executables[0]
    launch_log = logs / "launch.log"
    with launch_log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen([
            str(executable), "--bios", str(staged_bios), "--game",
            str(source / "game.toml"), "--disc", str(staged_disc),
            "--headless", "--memcard-dir", str(private / "state")],
            cwd=source, env=environment, stdout=stream, stderr=subprocess.STDOUT,
            text=True)
        deadline = time.monotonic() + 8
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        launchable = process.poll() is None
        if launchable:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
    explicit_tool_overrides = sorted(
        name for name, value in options.items()
        if value is not None and name in (
            "psxrecomp", "cmake", "ninja", "sdl_source", "libchdr_source",
            "toolchain_bin", "recomp_ui"))
    report = {
        "schema_version": 1,
        "status": "smoke-passed-not-promoted" if launchable else "launch-failed",
        "ready": False,
        "preflight": plan,
        "artifacts": {
            "executable": str(executable), "sha256": _sha256(executable),
            "game_config": str(source / "game.toml"),
            "logs": str(logs),
        },
        "interventions": {
            "human_equivalent_total": len(explicit_tool_overrides),
            "advanced_tool_overrides": explicit_tool_overrides,
            "automatic_actions": list(STAGES),
            "irreducible_user_inputs": [
                "project directory", "canonical project directory",
                "local disc", "local BIOS"] +
                (["validated M3U multi-disc receipt"]
                 if plan["inputs"].get("multi_disc") else []),
        },
        "time": {
            "started_at_utc": started_at_utc,
            "finished_at_utc": None,
            "elapsed_seconds": None,
            "elapsed_clock": "time.perf_counter",
        },
        "promotion": {"status": "not-promoted", "ready": False},
        "qualification": {
            "runtime_smoke_passed": launchable,
            "gameplay_qualified": False,
            "gameplay_status": "untested",
        },
    }
    if not launchable:
        failure = _runtime_failure_detail(source)
        report["runtime_failure"] = failure
        report["time"]["finished_at_utc"] = _utc_now()
        report["time"]["elapsed_seconds"] = round(time.perf_counter() - started, 6)
    (project / "baseline-report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if not launchable:
        raise BaselineError(report["runtime_failure"]["summary"])
    baseline_elapsed = round(time.perf_counter() - started, 6)
    promotion = promote(project, canonical_project, report, started_at_utc,
                        baseline_elapsed)
    report["status"] = "promoted-smoke-passed"
    report["ready"] = True
    report["promotion"] = promotion
    report["time"]["finished_at_utc"] = _utc_now()
    report["time"]["elapsed_seconds"] = round(time.perf_counter() - started, 6)
    (project / "baseline-report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report
