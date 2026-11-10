"""Public Studio build orchestration. Importing this module performs no I/O."""
from __future__ import annotations

import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
import preservation
import buildtools

CORE_URL = "https://github.com/RetroPortingToolKit/psxrecomp.git"
UI_URL = "https://github.com/RetroPortingToolKit/recomp-ui.git"


@dataclasses.dataclass
class BuildOptions:
    discs: list[str]
    bios: str
    output: str
    sbi: list[str] = dataclasses.field(default_factory=list)
    core_ref: str = "master"
    ui_ref: str = "master"
    custom_core: str = ""
    custom_ui: str = ""
    portable: bool = True
    skip_bios: bool = False
    pgo: bool = False
    diagnostics: bool = False
    compiler_optimization: str = "optimized"
    lto: bool = True
    startup_checks: bool = False
    bios_regions: dict[str, str] = dataclasses.field(default_factory=dict)
    bios_files: list[str] = dataclasses.field(default_factory=list)
    sbi_associations: dict[str, str] = dataclasses.field(default_factory=dict)


class Cancelled(RuntimeError):
    pass


class Runner:
    def __init__(self, log=print, event=lambda event: None):
        self.log = log
        self.event = event
        self.cancelled = threading.Event()
        self.process = None

    def cancel(self):
        self.cancelled.set()

    def run(self, argv, cwd, *, env=None):
        if self.cancelled.is_set():
            raise Cancelled("Build cancelled")
        self.log("$ " + subprocess.list2cmdline([str(a) for a in argv]))
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.process = subprocess.Popen(
            [str(a) for a in argv], cwd=cwd, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", creationflags=flags,
        )
        lines = []
        # A reader thread permits cancellation even while a child is silent.
        def read():
            for line in self.process.stdout:
                lines.append(line)
                self.log(line.rstrip())
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        while self.process.poll() is None:
            if self.cancelled.wait(.1):
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(self.process.pid), "/T", "/F"],
                                   capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW,
                                   text=True, encoding="utf-8", errors="replace")
                else:
                    self.process.terminate()
                self.process.wait()
                reader.join()
                self.process.stdout.close()
                self.process = None
                raise Cancelled("Build cancelled; build records and logs were retained")
        reader.join()
        code = self.process.returncode
        self.process.stdout.close()
        self.process = None
        if code:
            raise RuntimeError(f"Command failed (exit {code}): {argv[0]}\n" + "".join(lines[-12:]))
        return "".join(lines).strip()


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_source(source: Path, destination: Path):
    """Copy real working bytes, including uncommitted and untracked changes."""
    source = source.resolve()
    destination = destination.resolve()
    if destination == source or source in destination.parents:
        raise ValueError("Source snapshot must be outside the custom source folder")
    def ignore(folder, names):
        omitted = []
        for name in names:
            path = Path(folder) / name
            if name in {".git", "__pycache__"}:
                omitted.append(name)
            elif path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
                raise ValueError(f"Source contains a symbolic link: {path}; supply regular files")
        return omitted
    def copy_writable(original, target):
        shutil.copy2(original, target)
        # Only the private copy is writable; shared/custom inputs stay intact.
        Path(target).chmod(Path(target).stat().st_mode | stat.S_IWUSR)
        return target
    shutil.copytree(source, destination, ignore=ignore, copy_function=copy_writable)
    members = {p.relative_to(destination).as_posix(): sha256(p)
               for p in sorted(destination.rglob("*")) if p.is_file()}
    identity = hashlib.sha256(json.dumps(members, sort_keys=True).encode()).hexdigest()
    return {"content_sha256": identity, "files": members,
            "snapshot_excludes": [".git", "__pycache__"]}


def stage_source_runtime(config, product, discs, bios, serials, portable, skip_bios):
    """Use the established media writer for the source-built player too."""
    from pin_builder import pin_build
    mode = "portable" if portable else "thin"
    receipt = {"inputs": []}
    if bios:
        staged, staged_bios = pin_build.runtime_media(discs, bios, product, mode, receipt)
    else:
        staged = ([str(pin_build.stage_disc(disc, product / "inputs" / f"disc-{i}", receipt))
                   for i, disc in enumerate(discs, 1)] if portable else list(map(str, discs)))
        staged_bios = ""
    pin_build.write_runtime_media(config, product, staged, staged_bios, serials, mode,
                                 fast_boot=True if skip_bios else None)
    pin_build.check_inputs(receipt)
    return receipt


def snapshot_source(kind, ref, custom, cache, destination, runner):
    url = CORE_URL if kind == "core" else UI_URL
    record = {"kind": kind, "requested_ref": ref, "repository": url}
    if custom:
        source = Path(custom).expanduser().resolve()
        record.update(origin="custom-folder", source=str(source))
        if not source.is_dir():
            raise ValueError(f"Custom {kind} folder does not exist: {source}")
        if (source / ".git").exists():
            record["base_commit"] = runner.run(["git", "-C", source, "rev-parse", "HEAD"], source)
    else:
        if not ref.strip() or ref.startswith("-") or any(c.isspace() for c in ref):
            raise ValueError(f"Invalid {kind} version")
        objects = cache / (kind + "-objects.git")
        if not objects.exists():
            objects.parent.mkdir(parents=True, exist_ok=True)
            runner.run(["git", "init", "--bare", objects], cache)
        runner.run(["git", "--git-dir", objects, "fetch", "--depth=1", url, ref], cache)
        commit = runner.run(["git", "--git-dir", objects, "rev-parse", "FETCH_HEAD^{commit}"], cache)
        tree = runner.run(["git", "--git-dir", objects, "rev-parse", commit + "^{tree}"], cache)
        record.update(origin="upstream", commit=commit, tree=tree)
        # Independent short gitdirs avoid the nested bare/worktrees/modules path
        # that Git for Windows rejected with "$GIT_DIR too big".
        source = cache / (kind[0] + "-" + commit[:12])
        if not (source / ".git").is_dir():
            source.mkdir(parents=True, exist_ok=True)
            runner.run(["git", "init", source], cache)
            runner.run(["git", "-C", source, "fetch", "--depth=1", objects, commit], cache)
            runner.run(["git", "-C", source, "checkout", "--detach", commit], cache)
        actual = runner.run(["git", "-C", source, "rev-parse", "HEAD"], cache)
        if actual != commit:
            raise ValueError("Cached source does not match the requested commit")
        # Retry incomplete submodules after a failed Build instead of trusting
        # the existence of the source directory as proof of completeness.
        runner.run(["git", "-c", "core.longpaths=true", "-C", source,
                    "submodule", "update", "--init", "--recursive"], cache)
    record.update(copy_source(source, destination))
    required = "psxrecomp_cli.py" if kind == "core" else "recomp_ui.cmake"
    if not (destination / required).is_file():
        raise ValueError(f"Selected {kind} is missing {required}")
    preservation.apply(kind, destination, record, runner)
    return record


def validate(options):
    if not options.discs:
        raise ValueError("Choose at least one disc image")
    for item in options.discs:
        path = Path(item).expanduser()
        if not path.is_file() or path.suffix.lower() not in {".cue", ".iso", ".chd"}:
            raise ValueError(f"Disc image not found: {item}")
        if path.suffix.lower() == ".iso":
            with path.open("rb") as stream:
                stream.seek(16 * 2048 + 1)
                if stream.read(5) != b"CD001":
                    raise ValueError(f"ISO volume descriptor not found: {item}")
        if path.suffix.lower() == ".chd":
            with path.open("rb") as stream:
                if stream.read(8) != b"MComprHD":
                    raise ValueError(f"CHD header not found: {item}")
        if path.suffix.lower() != ".cue":
            continue
        files = re.findall(r'^\s*FILE\s+"([^"]+)"', path.read_text(encoding="utf-8-sig"), re.M | re.I)
        if not files:
            raise ValueError(f"CUE contains no quoted FILE entries: {item}")
        for file in files:
            if not (path.parent / file).is_file():
                raise ValueError(f"Disc track not found: {path.parent / file}")
    if not options.skip_bios and not Path(options.bios).is_file():
        raise ValueError("Choose your BIOS file or enable Skip BIOS")
    for file in options.sbi:
        if not Path(file).is_file() or Path(file).suffix.lower() != ".sbi":
            raise ValueError(f"SBI file not found: {file}")
    if not options.output.strip():
        raise ValueError("Choose an output folder")


def normalize_image(path, destination, core, runner, env):
    path = Path(path).resolve()
    if path.suffix.lower() == ".cue":
        return path
    destination.mkdir(parents=True)
    if path.suffix.lower() == ".iso":
        with path.open("rb") as stream:
            stream.seek(16 * 2048 + 1)
            if stream.read(5) != b"CD001":
                raise ValueError(f"ISO has no 2048-byte ISO9660 volume descriptor: {path.name}")
        cue = destination / (path.stem + ".cue")
        # An absolute reference avoids copying a large ISO before Portable staging.
        cue.write_text(f'FILE "{path.as_posix()}" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', encoding="utf-8")
        return cue
    runner.run([sys.executable, core / "tools" / "psx_chd.py", path,
                "--extract", destination, "--no-hash"], core, env=env)
    cue = destination / (path.stem + ".cue")
    if not cue.is_file():
        raise RuntimeError("CHD reader did not produce a CUE image")
    return cue


def choose_bios(files, regions, serial):
    serial = serial.upper()
    region = ("USA" if serial.startswith(("SLUS", "SCUS")) else
              "Europe" if serial.startswith(("SLES", "SCES")) else
              "Japan" if serial.startswith(("SLPS", "SCPS", "SLPM", "PAPX", "PCPX")) else "Auto")
    available = [p for p in files if Path(p).is_file()]
    for wanted in (region, "Auto"):
        candidate = next((p for p in available if regions.get(p, "Auto") == wanted), None)
        if candidate:
            return candidate
    raise ValueError(f"No configured BIOS file for {region}; add one in Settings")


def stage_disc(cue, destination):
    """Stage a CUE and its tracks without flattening relative directories."""
    destination.mkdir(parents=True, exist_ok=True)
    text = cue.read_text(encoding="utf-8-sig")
    names = {}
    def copy_match(match):
        path = (cue.parent / match.group(1)).resolve()
        name = path.name
        if name in names and names[name] != path:
            name = str(len(names) + 1) + "-" + name
        names[name] = path
        shutil.copy2(path, destination / name)
        return 'FILE "' + name + '"'
    text = re.sub(r'FILE\s+"([^"]+)"', copy_match, text, flags=re.I)
    result = destination / cue.name
    result.write_text(text, encoding="utf-8")
    return result


def build(options, data_root, runner, *, prepared_sources=None, prepared_runtime=None):
    """Only this explicit Build operation resolves/downloads source packages."""
    validate(options)
    if shutil.which("pwsh") is None:
        raise ValueError("Advanced source builds require PowerShell 7 (pwsh). The included supported versions do not require it.")
    output = Path(options.output).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    job = output / ("build-" + time.strftime("%Y%m%d-%H%M%S") + "-" + os.urandom(3).hex())
    job.mkdir()
    cache = Path(data_root) / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    record = {"schema": "public-studio-build-v1", "options": dataclasses.asdict(options),
              "state": "running", "sources": [], "started": time.time()}
    log_file = (job / "build.log").open("w", encoding="utf-8")
    original_log = runner.log
    def log(line):
        log_file.write(line + "\n")
        log_file.flush()
        original_log(line)
    runner.log = log
    try:
        for kind, ref, custom in [("core", options.core_ref, options.custom_core),
                                  ("ui", options.ui_ref, options.custom_ui)]:
            log(f"Preparing {kind} sourceΞ²β‚¬Β¦")
            if prepared_sources:
                source, identity = prepared_sources[kind]
                actual = copy_source(source, job / kind)
                if actual["content_sha256"] != identity["content_sha256"]:
                    raise RuntimeError(f"Batch {kind} snapshot changed after version resolution")
                record["sources"].append(identity)
            else:
                record["sources"].append(snapshot_source(kind, ref, custom, cache, job / kind, runner))
        core = job / "core"
        toolkit = core / "tools" / "new_project_layout"
        if prepared_runtime is None:
            pair = {record["kind"]: (job / record["kind"], record) for record in record["sources"]}
            prepared_runtime = buildtools.prepare(pair, job, cache, runner)
        record["module"] = prepared_runtime["receipt"]
        # The adapter is shipped with Studio, not required in custom core snapshots.
        setup = Path(__file__).parent / "setup_public.ps1"
        env = prepared_runtime["environment"].copy()
        env["TEMP"] = env["TMP"] = str(job / "temp")
        Path(env["TEMP"]).mkdir()
        env["PSXRECOMP_PYTHON"] = sys.executable
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["RETCOMM_TOOLCHAIN_CACHE"] = str(cache / "toolchains")
        env["RETCOMM_DATA_HOME"] = str(cache / "retcomm")
        env["XDG_DATA_HOME"] = str(cache / "xdg")
        env["LOCALAPPDATA"] = str(cache / "appdata")
        env["HOME"] = env["USERPROFILE"] = str(cache / "home")
        env["CMAKE_BUILD_PARALLEL_LEVEL"] = "1"
        discs = [normalize_image(p, job / "media" / str(i), core, runner, env)
                 for i, p in enumerate(options.discs, 1)]
        if options.portable:
            discs = [stage_disc(p, job / "staged" / str(i)) for i, p in enumerate(discs, 1)]
        probes = []
        for i, disc in enumerate(discs, 1):
            probe_file = job / f"identity-{i}.json"
            runner.run([sys.executable, toolkit / "probe_disc.py", disc,
                        "--identity-only", "--json-out", probe_file], job, env=env)
            probes.append(probe_file)
        identity = json.loads(probes[0].read_text(encoding="utf-8"))
        if options.bios_files and not options.skip_bios:
            options.bios = choose_bios(options.bios_files, options.bios_regions, identity.get("serial", ""))
            record["selected_bios"] = {"file": options.bios, "sha256": sha256(options.bios)}
        record["disc_identity"] = [json.loads(p.read_text(encoding="utf-8")) for p in probes]
        if len(discs) > 1:
            runner.run([sys.executable, toolkit / "verify_disc_set.py", *probes,
                        "--json-out", job / "disc-set.json"], job, env=env)
        name = identity.get("display_name") or identity.get("volume_id") or Path(options.discs[0]).stem
        command = ["pwsh", "-NoProfile", "-File", setup, "-Disc", discs[0],
                   "-Name", name, "-Dir", job / "title", "-Yes", "-NoGenerate", "-NoBuild",
                   "-NoGithub", "-NoCi", "-NoFetchBoxart", "-NoNetplay", "-EnableRecompUi",
                   "-NoWizard", "-PsxrecompSourceRoot", core, "-RecompUiSourceRoot", job / "ui",
                   "-ToolkitRoot", toolkit]
        (job / "title").mkdir()
        runner.run(command, job, env=env)
        projects = [p for p in (job / "title").iterdir() if (p / "game.toml").is_file()]
        if len(projects) != 1:
            raise RuntimeError("Setup did not produce exactly one game project")
        title = projects[0]
        if options.portable:
            discs = [stage_disc(p, title / "disc" / str(i)) for i, p in enumerate(discs, 1)]
        if len(discs) > 1 or options.portable:
            refresh = [sys.executable, toolkit / "probe_disc.py", discs[0],
                       "--write-game-toml", title / "game.toml", "--disc-rel",
                       discs[0].relative_to(title).as_posix() if options.portable else str(discs[0]),
                       "--out-dir", "disc", "--display-name", name]
            for disc in discs[1:]:
                refresh += ["--extra-disc", disc.relative_to(title).as_posix() if options.portable else str(disc)]
            runner.run(refresh, title, env=env)
        for sbi in options.sbi:
            source = Path(sbi)
            matches = [i for i, original in enumerate(options.discs)
                       if options.sbi_associations.get(original) == sbi or Path(original).stem.casefold() == source.stem.casefold()]
            if options.portable and len(matches) == 1:
                shutil.copy2(source, discs[matches[0]].with_suffix(".sbi"))
            else:
                shutil.copy2(source, title / "disc" / source.name)
        cli = title / "psxrecomp" / "psxrecomp_cli.py"
        generate = [sys.executable, cli, "generate", "--config", title / "game.toml",
                    "--project-root", title, "--disc", discs[0]]
        if options.bios and not options.skip_bios:
            generate += ["--bios", Path(options.bios).resolve()]
        runner.run(generate, title, env=env)
        rebuild = [sys.executable, cli, "rebuild", "--config", title / "game.toml",
                   "--project-root", title, "--build-dir", "build-release",
                   "--force-pgo" if options.pgo else "--no-pgo", "--pgo-mute", "--pgo-hide-video",
                   "--cmake-extra=-DPSX_DEBUG_TOOLS=" + ("ON" if options.diagnostics else "OFF")]
        rebuild += ["--cmake-extra=-DPSX_LAUNCHER_MODULE=ON", "--cmake-extra=-DPSX_STATIC_RUNTIME=OFF",
                    "--cmake-extra=-DRECOMP_LAUNCHER_MODULE_FILE=" + prepared_runtime["module"],
                    "--cmake-extra=-DSDL3_DIR=" + str(Path(prepared_runtime["sdl_prefix"]) / "lib" / "cmake" / "SDL3")]
        import compiler_options
        rebuild += compiler_options.arguments(options.compiler_optimization, options.lto, source=True)
        runner.run(rebuild, title, env=env)
        players = list((title / "build-release").glob("*.exe"))
        if len(players) != 1:
            raise RuntimeError("Expected one source-built game executable")
        record["module_compatibility"] = runner.run(
            [players[0], "--launcher-module-check"], title / "build-release", env=env)
        record["runtime_media"] = stage_source_runtime(
            title / "game.toml", title / "build-release", discs, options.bios,
            [probe.get("serial", "") for probe in record["disc_identity"]],
            options.portable, options.skip_bios)
        record["compiler_options_verified"] = compiler_options.verify(title / "build-release", options.compiler_optimization, options.lto)
        record.update(state="completed", project=str(title), output=str(title / "build-release"))
        return record
    except Exception as error:
        record.update(state="cancelled" if isinstance(error, Cancelled) or runner.cancelled.is_set() else "failed", error=str(error))
        raise
    finally:
        record["ended"] = time.time()
        (job / "build-record.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        runner.log = original_log
        log_file.close()


def build_batch(jobs, preferences, data_root, runner):
    """Resolve a pair once, then build each game serially on that exact pair."""
    if not jobs:
        raise ValueError("Choose your game files")
    if getattr(preferences, "core_pin", ""):
        import pinned_build
        return pinned_build.batch(jobs, preferences, data_root, runner)
    if shutil.which("pwsh") is None:
        raise ValueError("Advanced source builds require PowerShell 7 (pwsh). The included supported versions do not require it.")
    bios = next((p for p in preferences.bios_files if Path(p).is_file()), "")
    if preferences.startup_checks:
        raise ValueError("Startup checks currently require a core PIN.json selection")
    options = []
    for game in jobs:
        item = BuildOptions(
            discs=[str(p) for p in game.discs], bios=bios, output=preferences.output_folder,
            sbi=[str(p) for p in game.sbi.values()], core_ref=preferences.core_ref,
            ui_ref=preferences.ui_ref, custom_core=preferences.custom_core,
            custom_ui=preferences.custom_ui, portable=preferences.portable,
            skip_bios=preferences.skip_bios, pgo=preferences.pgo, diagnostics=preferences.diagnostics,
            compiler_optimization=preferences.compiler_optimization, lto=preferences.lto, startup_checks=preferences.startup_checks,
            bios_files=list(preferences.bios_files), bios_regions=dict(preferences.bios_regions),
            sbi_associations={str(disc): str(sbi) for disc, sbi in game.sbi.items()})
        validate(item)
        options.append(item)
    root = Path(data_root).expanduser().resolve() / "history"
    batch = root / ("batch-" + time.strftime("%Y%m%d-%H%M%S") + "-" + os.urandom(3).hex())
    batch.mkdir(parents=True)
    cache = Path(data_root) / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    receipt = {"schema": "public-studio-batch-v1", "state": "running", "sources": [], "games": []}
    pair = {}
    log_file = (batch / "build.log").open("w", encoding="utf-8")
    from batch_state import BatchState
    timing = BatchState(tuple(jobs), runner.event)
    original_log = runner.log
    def batch_log(line):
        log_file.write(str(line) + "\n")
        log_file.flush()
        original_log(line)
    runner.log = batch_log
    receipt["log"] = str(batch / "build.log")
    try:
        for kind, ref, custom in [("core", preferences.core_ref, preferences.custom_core),
                                  ("ui", preferences.ui_ref, preferences.custom_ui)]:
            destination = batch / kind
            identity = snapshot_source(kind, ref, custom, cache, destination, runner)
            pair[kind] = (destination, identity)
            receipt["sources"].append(identity)
        runtime = buildtools.prepare(pair, batch, cache, runner)
        receipt["module"] = runtime["receipt"]
        for index, (game, item) in enumerate(zip(jobs, options), 1):
            if runner.cancelled.is_set():
                raise Cancelled("Batch cancelled; later games were not started")
            timing.start(index - 1)
            name = re.sub(r'[^\w .-]', "_", game.name).strip(" .")[:80] or "Game"
            item.output = str(batch / f"{index:02d}-{name}")
            runner.log(f"Game {index} of {len(jobs)} Ξ’Β· {game.name}")
            try:
                result = build(item, data_root, runner, prepared_sources=pair, prepared_runtime=runtime)
                import delivery
                from pin_builder.workbench_output import RUNTIME_FILES, RUNTIME_DIRS
                payload = Path(result["output"])
                members = set(RUNTIME_FILES) | set(RUNTIME_DIRS) | {"inputs"}
                members.update(path.name for path in payload.glob("*.exe"))
                members.update(path.name for path in payload.glob("*.dll"))
                destination, verified = delivery.deliver(payload, preferences.output_folder, game.name, members)
                timing.end("completed", output=str(destination), delivery=verified)
            except Cancelled:
                timing.end("cancelled")
                raise
            except Exception as error:
                timing.end("failed", error=str(error))
                runner.log(f"Could not build {game.name}: {error}")
            receipt["games"] = timing.snapshot()
        receipt["state"] = "completed" if all(g["state"] == "completed" for g in receipt["games"]) else "failed"
        return receipt
    except Exception as error:
        receipt.update(state="cancelled" if isinstance(error, Cancelled) or runner.cancelled.is_set() else "failed", error=str(error))
        raise
    finally:
        receipt.update(timing.finish(receipt["state"]))
        runner.log = original_log
        log_file.close()
        (batch / "batch-record.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
