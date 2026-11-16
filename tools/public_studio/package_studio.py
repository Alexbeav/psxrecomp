"""Assemble an isolated portable Studio from a local CPython installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import zipfile

APP_FILES = ("studio.py", "backend.py", "settings.py", "selection.py", "dropfiles.py",
             "probe_disc.py", "setup_public.ps1", "preservation.py", "buildtools.py",
             "toolchain_request.py", "batch_state.py", "compiler_options.py", "delivery.py", "native_bridge.py", "support_files.py", "artwork.py", "art_chd.py", "pinned_build.py", "pin_entry.py", "asset_preflight.py", "toolchain-pin.toml", "README.md")


def package(destination, runtime_source, reuse_cache=None, work_root=None):
    import support_files
    support_files.check()
    destination = destination.resolve()
    runtime_source = runtime_source.resolve()
    if destination.exists():
        raise ValueError("Destination exists; choose a fresh candidate folder")
    if not (runtime_source / "pythonw.exe").is_file() or not (runtime_source / "DLLs" / "_tkinter.pyd").is_file():
        raise ValueError("The CPython source must include Windows Python and Tkinter")
    app = Path(__file__).parent
    for name in APP_FILES:
        if not (app / name).is_file():
            raise ValueError(f"Missing application file: {name}")
    destination.mkdir(parents=True)
    for name in APP_FILES:
        shutil.copy2(app / name, destination / name)
    shutil.copytree(app / "assets", destination / "assets")
    shutil.copytree(app / "pin_builder", destination / "pin_builder", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(app / "compatibility", destination / "compatibility")
    runtime = destination / "runtime"
    runtime.mkdir()
    for file in runtime_source.iterdir():
        if file.is_file() and (file.name in {"python.exe", "pythonw.exe", "LICENSE.txt"} or file.suffix.lower() == ".dll"):
            shutil.copy2(file, runtime / file.name)
    shutil.copytree(runtime_source / "DLLs", runtime / "DLLs",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pdb", "*.lib", "*.exp"))
    shutil.copytree(runtime_source / "tcl", runtime / "tcl")
    version = f"python{sys.version_info.major}{sys.version_info.minor}"
    with zipfile.ZipFile(runtime / (version + ".zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in (runtime_source / "Lib").rglob("*"):
            relative = file.relative_to(runtime_source / "Lib")
            if not file.is_file() or any(part in {"site-packages", "__pycache__", "test", "tests", "idlelib", "turtledemo", "ensurepip"} for part in relative.parts):
                continue
            if file.suffix in {".pyc", ".pyo"}:
                continue
            archive.write(file, relative.as_posix())
    (runtime / (version + "._pth")).write_text(f"{version}.zip\nDLLs\n.\n..\n", encoding="utf-8")
    cache_argument = ""
    if reuse_cache is not None:
        relative = os.path.relpath(reuse_cache.resolve(), destination)
        cache_argument = f' --cache-root "%~dp0{relative}"'
    (destination / "Workbench Studio.cmd").write_text(
        '@echo off\r\nset "PYTHONDONTWRITEBYTECODE=1"\r\n'
        'if not exist "%~dp0runtime\\pythonw.exe" (echo Studio runtime is missing & exit /b 1)\r\n'
        'start "" /D "%~dp0" "%~dp0runtime\\pythonw.exe" -B "%~dp0studio.py"' + cache_argument + ' %*\r\n',
        encoding="utf-8")
    work_environment = ""
    if work_root is not None:
        relative_work = os.path.relpath(work_root.resolve(), destination)
        work_environment = f'set "WORKBENCH_PUBLIC_WORK_ROOT=%~dp0{relative_work}"\r\n'
    native = app / "Workbench Studio.exe"
    if native.is_file():
        shutil.copy2(native, destination / native.name)
        shutil.copy2(app / "SDL3.dll", destination / "SDL3.dll")
        (destination / "Workbench Studio.cmd").write_text(
            '@echo off\r\nset "PYTHONDONTWRITEBYTECODE=1"\r\n'
            + work_environment + 'start "" /D "%~dp0" "%~dp0Workbench Studio.exe"' + cache_argument + ' %*\r\n', encoding="utf-8")
    framework_license = app.parents[1] / "LICENSE"
    if framework_license.is_file():
        licenses = destination / "licenses"
        licenses.mkdir()
        shutil.copy2(framework_license, licenses / "framework-LICENSE")
    members = {}
    total = 0
    for file in sorted(destination.rglob("*")):
        if file.is_file():
            content = file.read_bytes()
            total += len(content)
            members[file.relative_to(destination).as_posix()] = hashlib.sha256(content).hexdigest()
    receipt = {"schema": "public-studio-portable-v1", "python_version": sys.version.split()[0],
               "runtime_source": str(runtime_source), "bytes": total, "files": members,
               "source_packages_bundled": False, "input_media_bundled": False,
               "settings_bundled": False, "cache_bundled": False}
    (destination / "PACKAGE-RECORD.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--runtime", type=Path, default=Path(sys.base_prefix))
    parser.add_argument("--reuse-cache", type=Path)
    parser.add_argument("--work-root", type=Path)
    args = parser.parse_args()
    result = package(args.destination, args.runtime, args.reuse_cache, args.work_root)
    print(json.dumps({"bytes": result["bytes"], "files": len(result["files"]), "python": result["python_version"]}))
