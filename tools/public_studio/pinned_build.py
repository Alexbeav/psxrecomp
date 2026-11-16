"""The established PSX pin builder behind the separate edition's simple UI."""
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time
import urllib.request
import zipfile
import buildtools
from pin_builder import pin_build as pb, ui_selection


def checkout(kind, repository, commit, custom, bundles, cache, runner):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError(f"{kind} pin must name a full commit")
    source = cache / (kind[0] + "-" + commit[:12])
    if not (source / ".git").is_dir():
        source.mkdir(parents=True, exist_ok=True)
        runner.run(["git", "init", source], cache)
        fetch = custom or next((str(p) for p in bundles if p.is_file()), repository)
        runner.run(["git", "-C", source, "fetch", "--depth=1", fetch, commit], cache)
        runner.run(["git", "-C", source, "checkout", "--detach", commit], cache)
        runner.run(["git", "-C", source, "remote", "add", "origin", repository], cache)
    actual = runner.run(["git", "-C", source, "rev-parse", "HEAD"], cache)
    if actual != commit:
        raise ValueError(f"Cached {kind} source differs from its pin")
    runner.run(["git", "-c", "core.longpaths=true", "-C", source,
                "submodule", "update", "--init", "--recursive"], cache)
    return source


def download(url, expected, target, runner):
    if target.is_file() and buildtools.digest(target) == expected:
        return
    temporary = target.with_suffix(target.suffix + ".download")
    runner.log(f"Downloading {url}")
    try:
        with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                if runner.cancelled.is_set():
                    from backend import Cancelled
                    raise Cancelled("Build cancelled during download")
                output.write(chunk)
        if buildtools.digest(temporary) != expected:
            raise ValueError(f"Download checksum differs from selected core pin: {target.name}")
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)


def freeze_ui(pin, path, destination):
    selected = ui_selection.select(pin, path)
    artifacts = ui_selection.module_artifacts(pin, selected)
    destination.mkdir()
    record = destination / "UI.json"
    record.write_bytes(path.read_bytes())
    if artifacts:
        base = path.parent.resolve()
        # Copy only the selected module and SDK members, preserving relative paths.
        module = Path(artifacts["module"])
        relative = module.relative_to(base)
        (destination / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(module, destination / relative)
        sdk = Path(os.path.commonpath([Path(artifacts["sdl3_dll"]).parent,
                                      Path(artifacts["sdl3_cmake_dir"])]))
        for name in ("bin", "lib", "include", "share"):
            folder = sdk / name
            if folder.is_dir():
                shutil.copytree(folder, destination / folder.relative_to(base), dirs_exist_ok=True)
    return record, selected


def batch(jobs, preferences, data_root, runner):
    """Freeze pin files and sources once; run the original builder serially."""
    from backend import BuildOptions, Cancelled, choose_bios, normalize_image, stage_disc, validate
    import probe_disc
    import support_files
    support_files.check()
    available_bios = next((path for path in preferences.bios_files if Path(path).is_file()), "")
    for game in jobs:
        validate(BuildOptions(discs=[str(path) for path in game.discs], bios=available_bios,
                              output=preferences.output_folder, sbi=[str(path) for path in game.sbi.values()],
                              skip_bios=preferences.skip_bios))
    pin_path = Path(preferences.core_pin).expanduser().resolve()
    pin = pb.load_pin(pin_path)
    if not jobs:
        raise ValueError("Choose your game files")
    batch = Path(data_root).resolve() / "history" / ("build-" + time.strftime("%Y%m%d-%H%M%S") + "-" + os.urandom(3).hex())
    batch.mkdir(parents=True)
    cache = Path(os.environ.get("WORKBENCH_PUBLIC_CACHE_ROOT", str(Path(data_root) / "cache"))) / "pins"
    cache.mkdir(parents=True, exist_ok=True)
    receipt = {"schema": "public-studio-established-pin-v1", "state": "running", "games": [],
               "pin_sha256": pb.sha(pin_path), "core_commit": pin["commit"], "log": str(batch / "build.log")}
    from batch_state import BatchState
    timing = BatchState(tuple(jobs), getattr(runner, "event", lambda event: None))
    previous = runner.log
    stream = (batch / "build.log").open("w", encoding="utf-8")
    def log(line):
        stream.write(str(line) + "\n")
        stream.flush()
        previous(line)
    runner.log = log
    try:
        selected_pin = batch / "PIN.json"
        selected_pin.write_bytes(pin_path.read_bytes())
        ui_file = None
        ui_record = None
        if preferences.ui_pin:
            original = Path(preferences.ui_pin).expanduser().resolve()
            ui_file, ui_record = freeze_ui(pin, original, batch / "ui-input")
        ui_commit = ui_record["ui"]["commit"] if ui_record else pb.pin_ui_commitish(pin)
        bundles = list(Path(preferences.ui_pin).parent.glob("*.bundle")) if preferences.ui_pin else []
        ui_source = preferences.custom_ui
        if not ui_source:
            roots = [pin_path.parent]
            if preferences.ui_pin:
                roots += list(Path(preferences.ui_pin).resolve().parents)[:3]
            for root in roots:
                candidate = root / "recomp-ui"
                if (candidate / ".git").exists():
                    ui_source = str(candidate)
                    break
                adjacent = root / "recomp-ui"
                if adjacent.is_dir():
                    bundles += list(adjacent.glob("*/*.bundle"))
        core = checkout("core", pin["repo"], pin["commit"], preferences.custom_core, list(pin_path.parent.glob("*.bundle")), cache, runner)
        source_package = json.loads(Path(preferences.ui_pin).read_text(encoding="utf-8")).get("source_package") if preferences.ui_pin else None
        if source_package and not preferences.custom_ui:
            archive = Path(preferences.ui_pin).parent / source_package["file"]
            if buildtools.digest(archive) != source_package["sha256"]:
                raise ValueError("Bundled UI source package checksum differs from selected version")
            ui = cache / ("u-" + ui_commit[:12])
            if not ui.exists():
                ui.mkdir()
                with zipfile.ZipFile(archive) as package:
                    for member in package.infolist():
                        target = (ui / member.filename).resolve()
                        if not target.is_relative_to(ui.resolve()):
                            raise ValueError("UI source package contains an unsafe path")
                    package.extractall(ui)
            with zipfile.ZipFile(archive) as package:
                if (ui / "SOURCE-SNAPSHOT.json").read_bytes() != package.read("SOURCE-SNAPSHOT.json"):
                    raise ValueError("Cached UI source manifest differs from the selected source package")
            from pin_builder import source_snapshot
            source_snapshot.validate(ui, ui_commit)
        else:
            ui = checkout("ui", "https://github.com/Alexbeav/recomp-ui.git", ui_commit,
                          ui_source, bundles, cache, runner)
        receipt["ui_commit"] = ui_commit
        environment = buildtools.environment(cache, batch / "temp")
        result = batch / "toolchain.json"
        runner.run([sys.executable, Path(__file__).with_name("toolchain_request.py"), core, cache, result], batch, env=environment)
        toolchain = Path(json.loads(result.read_text(encoding="utf-8"))["bin"]).parent
        python = toolchain / "python" / "python.exe"
        if not python.is_file():
            raise ValueError("The selected toolchain lacks the Python runtime required by its pin builder")
        if ui_file:
            import asset_preflight
            receipt["ui_asset_preflight"] = asset_preflight.check(core, ui, pin, ui_file,
                batch / "asset-check", toolchain / "bin" / "cmake.exe", runner, environment)
        downloads = cache / "downloads"
        downloads.mkdir(exist_ok=True)
        tree = ast.parse((core / "tools" / "release_stage.py").read_text(encoding="utf-8"))
        archives = next(ast.literal_eval(node.value) for node in tree.body if isinstance(node, ast.Assign)
                        and any(isinstance(target, ast.Name) and target.id == "TOOLCHAIN_PINS" for target in node.targets))["win"]
        for part in ("python", "tcc"):
            url, digest = archives[part + "_url"], archives[part + "_sha256"]
            download(url, digest, downloads / url.rsplit("/", 1)[1], runner)
        work_root = Path(os.environ.get("WORKBENCH_PUBLIC_WORK_ROOT", str(Path(data_root).resolve().parent / "w"))).resolve()
        work_root.mkdir(parents=True, exist_ok=True)
        catalog = cache / "recipes"
        catalog.mkdir(exist_ok=True)
        environment.update(WORKBENCH_BUILD_JOBS="1", WORKBENCH_NO_FETCH="1",
                           WORKBENCH_STUDIO_RECORD=str(batch / "edition-record.json"),
                           WORKBENCH_PIN_ROOT=str(batch), WORKBENCH_UI_ROOT=str(batch / "ui-input"))
        for index, game in enumerate(jobs, 1):
            if runner.cancelled.is_set():
                raise Cancelled("Build cancelled; later games were not started")
            timing.start(index - 1)
            try:
                discs = []
                serial = ""
                for number, image in enumerate(game.discs, 1):
                    image = Path(image)
                    # The established builder creates its pinned libchdr reader and
                    # reads CHD hunks directly. Do not extract before that decoder exists.
                    cue = image if image.suffix.lower() == ".chd" else normalize_image(
                        image, batch / f"image-{index}-{number}", core, runner, environment)
                    # A selected SBI lives next to a copied image, never edits the source.
                    if image in game.sbi:
                        if cue.suffix.lower() == ".chd":
                            destination = batch / f"media-{index}-{number}"
                            destination.mkdir()
                            shutil.copy2(cue, destination / cue.name)
                            cue = destination / cue.name
                        else:
                            cue = stage_disc(cue, batch / f"media-{index}-{number}")
                        shutil.copy2(game.sbi[image], cue.with_suffix(".sbi"))
                    discs.append(cue)
                    if not serial and cue.suffix.lower() != ".chd":
                        serial = probe_disc.probe(cue, identity_only=True).serial
                bios = choose_bios(preferences.bios_files, preferences.bios_regions, serial)
                wanted = pb.region_bios_stem([serial])
                profiles = pb.bios_profiles(core)
                for candidate in preferences.bios_files:
                    if Path(candidate).is_file() and profiles.get(pb.sha(candidate)) == wanted:
                        bios = candidate
                        break
                name = re.sub(r"[^\w .-]", "_", game.name).strip(" .")[:70] or "Game"
                output = batch / "products" / f"{index:02d}-{name}"
                bios_candidates = batch / f"bios-{index}.json"
                bios_candidates.write_text(json.dumps(preferences.bios_files), encoding="utf-8")
                argv = [python, "-u", "-B", Path(__file__).with_name("pin_entry.py"), "build",
                        "--pin", selected_pin, "--bios", bios, "--bios-candidates", bios_candidates, "--output", output,
                        "--source-repo", core, "--ui-repo", ui, "--toolchain-pin", Path(__file__).with_name("toolchain-pin.toml"),
                        "--toolchain", toolchain, "--overlay-archives", downloads, "--kits-root", catalog,
                        "--run-root", work_root, "--input-mode", "portable" if preferences.portable else "thin",
                        "--platform", "windows-x64"]
                if ui_file:
                    argv += ["--ui", ui_file]
                if preferences.skip_bios:
                    argv += ["--fast-boot"]
                argv += ["--compiler-optimization", preferences.compiler_optimization]
                if not preferences.lto:
                    argv += ["--disable-lto"]
                if preferences.diagnostics:
                    argv += ["--diagnostics"]
                if preferences.startup_checks:
                    argv += ["--startup-checks"]
                if preferences.pgo:
                    argv += ["--optimize-pgo"]
                for disc in discs:
                    argv += ["--disc", disc]
                log(f"Game {index} of {len(jobs)} Ξ’Β· {game.name}")
                text = runner.run(argv, batch, env=environment)
                import delivery
                destination, verified = delivery.deliver(output / "windows-x64", preferences.output_folder, game.name)
                lines = [line for line in text.splitlines() if line.startswith("{")]
                if lines:
                    result = json.loads(lines[-1])
                    if result.get("receipt"):
                        record_path = Path(result["receipt"])
                        record = json.loads(record_path.read_text(encoding="utf-8"))
                        record["delivery"] = {"output": str(destination), **verified}
                        record["output"] = str(destination)
                        record["executable"]["path"] = str(destination / Path(record["executable"]["path"]).name)
                        record["runtime_config"] = str(destination / "game.toml")
                        retained = batch / "jobs" / f"{index:02d}"
                        retained.mkdir(parents=True)
                        for source in record_path.parent.rglob("*"):
                            if source.is_file() and source.suffix.lower() in {".json", ".log", ".txt", ".out", ".err", ".tsv"} and "failed-payload" not in source.relative_to(record_path.parent).parts:
                                target = retained / source.relative_to(record_path.parent)
                                target.parent.mkdir(parents=True, exist_ok=True)
                                shutil.copy2(source, target)
                        record["delivery"]["builder_scratch"] = str(record_path.parent.parent)
                        (retained / "build-receipt.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
                        run = record_path.parent.parent.resolve()
                        if run.parent != work_root:
                            raise RuntimeError("Compiler scratch escaped the selected work root")
                        try:
                            delivery.remove_owned_tree(run, work_root)
                        except OSError as error:
                            log(f"Game delivered; temporary build files retained at {run}: {error}")
                if not output.resolve().is_relative_to(batch.resolve()):
                    raise RuntimeError("Product scratch escaped Studio history")
                delivery.remove_owned_tree(output, batch)
                timing.end("completed", output=str(destination), delivery=verified)
            except Cancelled:
                timing.end("cancelled")
                raise
            except Exception as error:
                timing.end("failed", error=str(error))
                log(str(error))
            receipt["games"] = timing.snapshot()
            (batch / "batch-record.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        receipt["state"] = "completed" if all(game["state"] == "completed" for game in receipt["games"]) else "failed"
        return receipt
    except Exception as error:
        receipt.update(state="cancelled" if runner.cancelled.is_set() else "failed", error=str(error))
        raise
    finally:
        receipt.update(timing.finish(receipt["state"]))
        runner.log = previous
        stream.close()
        if receipt["state"] == "completed":
            ui_snapshot = batch / "ui-input" / "UI.json"
            if ui_snapshot.is_file():
                shutil.copy2(ui_snapshot, batch / "UI.json")
            for name in ("asset-check", "temp", "toolchain-host", "ui-input", "products"):
                scratch = (batch / name).resolve()
                if scratch.parent != batch.resolve():
                    raise RuntimeError("Scratch cleanup escaped Studio history")
                if scratch.is_dir():
                    import delivery
                    delivery.remove_owned_tree(scratch, batch)
        (batch / "batch-record.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
