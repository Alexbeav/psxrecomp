"""Build one shared SDL/module pair for an explicitly requested batch."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tarfile
import urllib.request


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def environment(cache, temp):
    env = os.environ.copy()
    # Capture the persistent user cache before isolating build-tool settings.
    env.setdefault("WORKBENCH_SHARED_COMPILER_ROOT", str(Path(env.get("LOCALAPPDATA", str(cache))) / "RPT" / "compilers"))
    temp.mkdir(parents=True, exist_ok=True)
    env.update(TEMP=str(temp), TMP=str(temp), PYTHONDONTWRITEBYTECODE="1",
               PYTHONUTF8="1", PYTHONIOENCODING="utf-8",
               PSX_STUDIO_PROJECT_ONLY="1", RETCOMM_TOOLCHAIN_CACHE=str(cache / "toolchains"),
               RETCOMM_DATA_HOME=str(cache / "retcomm"), XDG_DATA_HOME=str(cache / "xdg"),
               LOCALAPPDATA=str(cache / "appdata"), HOME=str(cache / "home"),
               USERPROFILE=str(cache / "home"), CMAKE_BUILD_PARALLEL_LEVEL="1",
               GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="Never", PSXRECOMP_PYTHON=sys.executable)
    return env


def shared_sdl(core, cache, binary, runner, env):
    rows = (core / "third_party" / "deps.manifest").read_text(encoding="utf-8").splitlines()
    fields = next((line.split() for line in rows if line.split() and line.split()[0] == "SDL3"), None)
    if not fields or len(fields) != 4:
        raise ValueError("Selected core has no supported pinned SDL3 dependency record")
    _, filename, checksum, url = fields
    prefix = cache / "shared-sdl" / checksum
    receipt_file = prefix / "studio-provider.json"
    if receipt_file.is_file():
        receipt = json.loads(receipt_file.read_text(encoding="utf-8"))
        if receipt["source_sha256"] == checksum and digest(prefix / "bin" / "SDL3.dll") == receipt["sha256"]:
            return prefix, receipt
        raise ValueError("Cached shared SDL provider does not match its recorded checksum")
    downloads = cache / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / filename
    if not archive.exists():
        runner.log(f"Downloading SDL3 from {url}")
        temporary = archive.with_suffix(archive.suffix + ".download")
        try:
            with urllib.request.urlopen(url, timeout=60) as source, temporary.open("wb") as target:
                while chunk := source.read(1024 * 1024):
                    if runner.cancelled.is_set():
                        raise RuntimeError("Build cancelled during SDL download")
                    target.write(chunk)
            if digest(temporary) != checksum:
                raise ValueError("Downloaded SDL3 archive checksum differs from the selected core pin")
            temporary.replace(archive)
        finally:
            if temporary.exists():
                temporary.unlink()
    if digest(archive) != checksum:
        raise ValueError("Cached SDL3 archive checksum differs from the selected core pin")
    source_root = cache / "sdl-source" / checksum
    if not source_root.exists():
        source_root.mkdir(parents=True)
        with tarfile.open(archive) as source:
            source.extractall(source_root, filter="data")
    sources = [p for p in source_root.iterdir() if (p / "CMakeLists.txt").is_file()]
    if len(sources) != 1:
        raise ValueError("SDL3 archive has no unique CMake source root")
    build = cache / "sdl-build" / checksum
    cmake = binary / "cmake.exe"
    runner.run([cmake, "-S", sources[0], "-B", build, "-G", "Ninja",
                f"-DCMAKE_C_COMPILER={binary / 'clang.exe'}", f"-DCMAKE_CXX_COMPILER={binary / 'clang++.exe'}",
                f"-DCMAKE_MAKE_PROGRAM={binary / 'ninja.exe'}", "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF",
                "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF"], cache, env=env)
    runner.run([cmake, "--build", build, "--parallel", "1", "--target", "install"], cache, env=env)
    receipt = {"source_url": url, "source_sha256": checksum, "archive_bytes": archive.stat().st_size,
               "sha256": digest(prefix / "bin" / "SDL3.dll"), "origin": "built-from-pinned-source"}
    receipt_file.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    return prefix, receipt


def prepare(pair, batch, cache, runner):
    core, core_record = pair["core"]
    ui, ui_record = pair["ui"]
    env = environment(cache, batch / "temp")
    result = batch / "toolchain.json"
    runner.run([sys.executable, Path(__file__).with_name("toolchain_request.py"), core, cache, result], batch, env=env)
    toolchain = json.loads(result.read_text(encoding="utf-8"))
    binary = Path(toolchain["bin"]).resolve()
    env["RETCOMM_TOOLCHAIN_DIR"] = str(binary)
    env["PATH"] = str(binary) + os.pathsep + env.get("PATH", "")
    sdl, provider = shared_sdl(core, cache, binary, runner, env)
    identity = hashlib.sha256(json.dumps({"module_recipe": 2, "core": core_record["content_sha256"],
        "ui": ui_record["content_sha256"], "provider": provider["sha256"],
        "toolchain": toolchain["metadata"]}, sort_keys=True).encode()).hexdigest()
    root = cache / "modules" / identity
    root.mkdir(parents=True, exist_ok=True)
    module = root / "recomp-launcher-v2.dll"
    module_record = root / "studio-module.json"
    if module_record.is_file():
        receipt = json.loads(module_record.read_text(encoding="utf-8"))
        if digest(module) != receipt["sha256"]:
            raise ValueError("Cached launcher module differs from its recorded checksum")
    else:
        generator = ui / "tools" / "generate_launcher_v2_witness.py"
        runner.run([sys.executable, generator, ui / "src" / "recomp_launcher.h",
                    ui / "tools" / "launcher_v2_contract.tsv", root / "records-check.inc",
                    "--boundary-header", ui / "src" / "recomp_launcher_module.h",
                    "--boundary-output", root / "boundary-check.inc"], batch, env=env)
        for generated, supplied in [("records-check.inc", "launcher_module_v2_records_witness.inc"),
                                     ("boundary-check.inc", "launcher_module_v2_boundary_witness.inc")]:
            if (root / generated).read_bytes().replace(b"\r\n", b"\n") != (ui / "src" / "common" / supplied).read_bytes().replace(b"\r\n", b"\n"):
                raise ValueError("Launcher source and generated boundary witnesses disagree")
        runner.run([binary / "cmake.exe", "-S", ui, "-B", root, "-G", "Ninja",
                    f"-DCMAKE_C_COMPILER={binary / 'clang.exe'}", f"-DCMAKE_CXX_COMPILER={binary / 'clang++.exe'}",
                    f"-DCMAKE_MAKE_PROGRAM={binary / 'ninja.exe'}", f"-DSDL3_DIR={sdl / 'lib' / 'cmake' / 'SDL3'}",
                    "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DRECOMP_UI_BUILD_LAUNCHER_MODULE=ON",
                    "-DRECOMP_UI_ENABLE_MODS=ON", f"-DRECOMP_UI_PSXRECOMP_ROOT={core}",
                    f"-DRECOMP_LAUNCHER_V2_BUILD_IDENTITY={identity}"], batch, env=env)
        runner.run([binary / "cmake.exe", "--build", root, "--parallel", "1", "--target", "recomp-launcher-module"], batch, env=env)
        receipt = {"identity": identity, "sha256": digest(module), "shared_sdl": provider,
                   "toolchain": toolchain["metadata"], "abi_major": 2, "module_recipe": 2,
                   "origin": "built-from-selected-pair"}
        module_record.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    return {"environment": env, "module": str(module), "sdl_prefix": str(sdl), "receipt": receipt}
