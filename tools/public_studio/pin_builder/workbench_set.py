"""Build a set whose discs boot different programs as one release folder (PS1B-333).

Resident Evil 2's two discs boot different programs. One build links one
program, so the set is one build per program. Alex's shape (2026-10-01): one
release folder, one exe per program, one shared `saves` folder, shared settings
and key binds, one shortcut per program.

How a set build runs:
  1. The ordinary build finds that the selected discs match several kits which
     declare one set (pin_build.program_set_plan) and stops with the plan.
  2. Each program is built on its own, with its own kit and discs, by the
     unchanged single-program pipeline, gates included.
  3. The finished product folders are merged into one (`merge_products`). A
     file both programs carry must be byte-identical, except a short list of
     known per-build files. Each exe gets its own config, `<exe>.game.toml`,
     which lists the whole set and names the program's own discs.
  4. Each program is started headless from a copy of the merged folder, and the
     first one again after the others, so a disc one program remembered cannot
     derail another (`set_boot_check`). What the starts write stays in the copy;
     the delivered folder must still match the merged payload afterwards.

The runtime must know `[game] program_discs` and the per-exe config (pin H).
A pin without them is refused before any program is built.

A pin that carries `tools/program_set.py` (the same pin H work) does step 3 with
that tool: one implementation of the join, the one a player's setup runs. Such a
pin can also make one setup package for the whole set, and the Windows test
installs it into a clean folder and compares the result with the private set
product (workbench_set_package, PS1B-347).
"""
from copy import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import tempfile
import time
import tomllib

try:
    from . import pin_build as pb
    from . import lean_boot_gate
    from . import workbench_runs as runs
    from . import workbench_output as wo
    from . import workbench_package as single
    from . import workbench_set_package as set_package
except ImportError:
    import pin_build as pb
    import lean_boot_gate
    import workbench_runs as runs
    import workbench_output as wo
    import workbench_package as single
    import workbench_set_package as set_package

# Files every program's product carries for itself. They are rebuilt for the
# merged folder, never copied across.
PER_PROGRAM = {"game.toml", "BUILDINFO.json", "SHA256SUMS.txt", "disc.cfg", "settings.toml"}
# Same path in every program's product, different bytes for a known, harmless
# reason. The first program's copy is kept and every hash is recorded.
#   overlay_toolchain/psxrecomp-game.exe  link timestamps differ per build
#   mods/README.md                        names the program
KNOWN_VARIANTS = {"overlay_toolchain/psxrecomp-game.exe", "mods/README.md"}
# Line-oriented caches both programs append to: the union is kept.
LINE_UNIONS = {"saves/disc_digests.tsv"}
RUNTIME_MARKERS = (("runtime/include/disc_roster.h", "disc_roster_program_subset"),
                   ("runtime/src/main.cpp", ".game.toml"))


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def require_runtime_support(repo, commit):
    """Stop unless the pinned runtime can share one folder between programs."""
    for path, marker in RUNTIME_MARKERS:
        try:
            text = pb.git(repo, "show", f"{commit}:{path}")
        except Exception as error:  # noqa: BLE001
            raise pb.BuildStop(f"Cannot read {path} at the pin commit to check program-set support: {error}")
        if marker not in text:
            raise pb.BuildStop(
                "The selected discs are the programs of one set, but this pin's runtime cannot share one "
                "folder between programs: it has no [game] program_discs and no per-exe config (pin H13). "
                "Build each program on its own (one disc per build), or choose a pin with program-set support.")


def set_program_discs(config, set_discs, set_serials, positions):
    """Make a program's config describe the whole set and name its own discs."""
    pb.set_game_media(config, set_discs, set_serials)
    text = config.read_text(encoding="utf-8-sig")
    text = re.sub(r"(?m)^\s*program_discs\s*=.*\n", "", text)
    updated, count = re.subn(r"(?m)^(disc_serials\s*=.*\n)",
                             lambda m: m.group(1) + "program_discs = " + json.dumps(positions) + "\n", text, count=1)
    parsed = tomllib.loads(updated)["game"] if count else {}
    if parsed.get("program_discs") != positions or parsed.get("discs") != set_discs:
        raise pb.BuildStop(f"Cannot write program_discs into {config.name}. Review the program's game.toml.")
    config.write_text(updated, encoding="utf-8")


def shared_settings(text):
    """settings.toml for the merged folder: every table except [disc].

    The programs share this file. A remembered disc would be one program's; each
    program's own config names its discs instead.
    """
    out, skipping = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            skipping = stripped == "[disc]"
        if not skipping:
            out.append(line)
    return "\n".join(out) + "\n"


def merge_products(programs, target, plan):
    """Join the programs' product folders into `target`; return the merge record.

    `programs` is [{"program", "product": folder, "executable": file name,
    "exe_name": name for the merged folder, "positions": [...]}], in set order.
    Stops on any file the programs disagree about that is not a known variant.
    """
    target = Path(target)
    target.mkdir(parents=True, exist_ok=True)
    record = {"known_variants": {}, "line_unions": [], "per_program": [], "shared_files": 0}
    # The set roster: each program's product knows the runtime path of its own discs.
    set_discs = [None] * len(plan["serials"])
    for program in programs:
        game = tomllib.loads((Path(program["product"]) / "game.toml").read_text(encoding="utf-8-sig"))["game"]
        own_paths = game.get("discs") or ([game["disc"]] if game.get("disc") else [])
        own_serials = game.get("disc_serials") or []
        for path, serial in zip(own_paths, own_serials):
            if serial in plan["serials"]:
                set_discs[plan["serials"].index(serial)] = path
    if any(path is None for path in set_discs):
        missing = [s for s, p in zip(plan["serials"], set_discs) if p is None]
        raise pb.BuildStop("The programs' products do not name a disc for every serial of the set: "
                           + ", ".join(missing) + ".")

    settings_text = None
    for order, program in enumerate(programs):
        product = Path(program["product"])
        built_exe = program["executable"]
        exe_name = program["exe_name"] + Path(built_exe).suffix
        for source in sorted(p for p in product.rglob("*") if p.is_file()):
            relative = source.relative_to(product).as_posix()
            if relative in PER_PROGRAM or relative == built_exe:
                continue
            destination = target / relative
            if not destination.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
                record["shared_files"] += 1
                continue
            if relative in LINE_UNIONS:
                lines = destination.read_text(encoding="utf-8").splitlines()
                extra = [line for line in source.read_text(encoding="utf-8").splitlines() if line not in lines]
                if extra:
                    destination.write_text("\n".join(lines + extra) + "\n", encoding="utf-8", newline="\n")
                if relative not in record["line_unions"]:
                    record["line_unions"].append(relative)
                continue
            if sha(source) == sha(destination):
                continue
            if relative in KNOWN_VARIANTS:
                entry = record["known_variants"].setdefault(
                    relative, {"kept": programs[0]["program"], "sha256": {programs[0]["program"]: sha(destination)}})
                entry["sha256"][program["program"]] = sha(source)
                continue
            raise pb.BuildStop(
                f"The programs of this set disagree about {relative} ({programs[0]['program']} and "
                f"{program['program']}). One folder can hold one copy. Make the kits agree, or add the file "
                "to the known per-build list if the difference is harmless.")
        shutil.copy2(product / built_exe, target / exe_name)
        config = target / (program["exe_name"] + ".game.toml")
        shutil.copy2(product / "game.toml", config)
        set_program_discs(config, set_discs, plan["serials"], program["positions"])
        if order == 0 and (product / "settings.toml").is_file():
            settings_text = shared_settings((product / "settings.toml").read_text(encoding="utf-8-sig"))
        record["per_program"].append({"program": program["program"], "executable": exe_name,
                                      "executable_sha256": sha(target / exe_name), "config": config.name,
                                      "program_discs": program["positions"]})
    if settings_text is not None:
        (target / "settings.toml").write_text(settings_text, encoding="utf-8", newline="\n")
    (target / "saves").mkdir(exist_ok=True)
    record["set_discs"] = set_discs
    return record


def write_set_provenance(target, plan, programs, merge, boots):
    """BUILDINFO.json (schema 3) and SHA256SUMS.txt for the merged folder."""
    wo.assert_no_netplay_state(target)  # as every single product is checked (PS1B-278)
    wo.assert_no_stray_sources(target)  # no generated shard source, or any other source file, in a product
    entries = []
    for program in programs:
        info = dict(program.get("buildinfo") or {})
        info.pop("payload", None)  # the merged payload is listed once, below
        merged = next(row for row in merge["per_program"] if row["program"] == program["program"])
        entries.append({"program": program["program"], "executable": merged["executable"],
                        "executable_sha256": merged["executable_sha256"], "config": merged["config"],
                        "serials": program["serials"], "program_discs": program["positions"],
                        "shortcut": program["shortcut"], "kit": program["kit"],
                        # Kept per program: each is its own build of the framework.
                        "netplay": info.get("netplay"), "framework_pin_stamped": info.get("framework_pin_stamped"),
                        "lobby_version": info.get("lobby_version"), "build": info})
    first = entries[0]["build"] if entries else {}
    info = {"schema": 3, "build_route": "workbench_private_program_set", "platform": "windows-x64",
            "set": {"name": plan["name"], "title": plan["title"], "serials": plan["serials"]},
            "runtime_commit": first.get("runtime_commit"), "runtime_tree": first.get("runtime_tree"),
            "ui_commit": first.get("ui_commit"), "toolchain": first.get("toolchain"),
            "programs": entries,
            "shortcuts": [{"name": e["shortcut"], "executable": e["executable"]} for e in entries],
            "merge": {k: merge[k] for k in ("known_variants", "line_unions", "shared_files")},
            "set_boot_check": boots, "full_game_correctness": "NOT_CLAIMED",
            "payload": pb.inventory(target)}
    pb.write_json(target / "BUILDINFO.json", info)
    sums = pb.inventory(target)
    (target / "SHA256SUMS.txt").write_text("".join(f"{item['sha256']}  {item['path']}\n" for item in sums),
                                           encoding="utf-8")
    return info


def set_boot_check(product, programs, plan, evidence, unrelated_cwd, env, on_run=None):
    """Start each program from the merged folder, then the first one again.

    The programs share settings.toml. The last start proves that what a later
    program left there does not make the first one mount another program's disc.
    """
    order = list(programs) + ([programs[0]] if len(programs) > 1 else [])
    rows = []
    for number, program in enumerate(order, start=1):
        merged_exe = Path(product) / (program["exe_name"] + Path(program["executable"]).suffix)
        expected = plan["serials"][program["positions"][0] - 1]
        label = program["program"] + ("-again" if number > len(programs) else "")
        lean_boot_gate.start_other_discs(
            Path(product), merged_exe, Path(evidence), Path(unrelated_cwd), env,
            [{"disc": label, "serial": expected, "select": None}], on_run=on_run, first_number=number, rows=rows,
            kind="program")
        rows[-1]["program"] = program["program"]
    return rows


def payload_difference(before, folder):
    """How `folder` differs from the inventory `before`: (added, missing, changed) path lists."""
    then = {item["path"]: item["sha256"] for item in before}
    now = {item["path"]: item["sha256"] for item in pb.inventory(Path(folder))}
    return (sorted(set(now) - set(then)), sorted(set(then) - set(now)),
            sorted(path for path in set(now) & set(then) if now[path] != then[path]))


def require_unchanged_payload(before, folder):
    """Stop unless `folder` still holds exactly the merged payload.

    A start writes into the folder it runs from: overlay captures, generated
    shard sources (game code), lock files, a remembered disc. None of that may
    be delivered or listed in BUILDINFO.json. The set check therefore starts a
    copy; this is the proof that the delivered folder was not touched.
    """
    added, missing, changed = payload_difference(before, folder)
    if not (added or missing or changed):
        return
    def some(paths):
        return ", ".join(paths[:6]) + (f" and {len(paths) - 6} more" if len(paths) > 6 else "")
    parts = [f"{label}: {some(paths)}" for label, paths in
             (("not part of the programs' payloads", added), ("missing", missing), ("changed", changed)) if paths]
    raise pb.BuildStop("The set's folder is not the merged payload any more (" + "; ".join(parts) + "). "
                       "Files a start leaves behind must never be delivered; the set check runs in a copy.")


def leftovers(before, folder):
    """What the starts did to the check copy, for the receipt.

    `files`, `bytes` and `examples` are the files they added. `changed` names
    the payload files they rewrote (the shared settings, at least); `removed`
    the ones they deleted.
    """
    folder = Path(folder)
    added, missing, changed = payload_difference(before, folder)
    return {"files": len(added), "bytes": sum((folder / path).stat().st_size for path in added),
            "examples": added[:8], "changed": changed, "removed": missing}


def remove_folder(folder, attempts=10, delay=1.0, sleep=time.sleep):
    """Remove a folder this build owns. Returns None, or the error text when it is still there."""
    error = None
    for attempt in range(attempts):
        try:
            shutil.rmtree(folder)
            return None
        except FileNotFoundError:
            return None
        except OSError as failure:
            error = failure
            if attempt + 1 < attempts:
                sleep(delay)
    return str(error)


def mark_plan_run(plan_receipt, cache, receipt):
    """Relabel the planning run and drop its work tree.

    The ordinary build that found the set stopped with the plan. It is not a
    failed build: left as `human-stop` with a work tree, each set build would
    take one of the work trees retention keeps for real failures (PS1B-328).
    """
    path = (plan_receipt or {}).get("receipt")
    if not path or not Path(path).is_file():
        return
    try:
        path = Path(path)
        run = path.parent.parent
        saved = json.loads(path.read_text(encoding="utf-8"))
        if saved.get("status") != "human-stop" or not saved.get("program_set"):
            return
        work = run / "work"
        if work.is_dir() and run.resolve().parent == Path(cache).resolve():
            error = remove_folder(work)
            if error:
                receipt["warnings"].append(f"The planning run's work folder could not be removed: {work} ({error}).")
        saved.update(status=runs.PLAN_STATUS, work_retained=work.exists(),
                     plan_note="This run found that the selected discs are the programs of one set and "
                               "handed over to the set build. It is not a failed build; the set build's own "
                               "receipt says how that build ended.")
        pb.write_json(path, saved)
        receipt["plan_run"]["status"] = runs.PLAN_STATUS
    except (OSError, ValueError) as error:
        receipt["warnings"].append(f"The planning run could not be relabelled: {error}")


def set_steps(cache, pin, kind):
    """A record for the steps the set build runs itself (join, package, setup pass),
    in the shape pin_build.run_command and the progress stages write into."""
    return {"commands": [], "inputs": [], "sources": [], "warnings": [], "pin": pin, "output_kind": kind,
            "stage_history": str(Path(cache) / "workbench-stage-history.json")}


def kit_configs(plan):
    return {program["program"]: (Path(program["kit"]) / "game.toml").read_text(encoding="utf-8-sig")
            for program in plan["programs"]}


# What a single build's receipt says that a wave summary lists (PS1B-266, PS1B-345, PS1B-352, PS1B-487).
PROGRAM_FINDINGS = ("game_code_interpreted", "overlay_code_interpreted", "cdrom_reads", "bundled_notice_gaps",
                    "start_memory")


def overlay_rank(record):
    """How bad an overlay-only gate record is (PS1B-487): a warned record (fail, then refused) before a
    pass, then the higher rate. No record ranks lowest."""
    if not record:
        return (-1, -1)
    warned = {"fail": 2, "refused": 1}.get(record.get("verdict"), 0)
    rate = record.get("per_frame")
    return (warned, rate if type(rate) in (int, float) else -1)


def carry_program_findings(receipt, name, one):
    """Keep, at the set's top level, the worst of each finding over its programs, with the program's name.

    The queue's result row and the wave summary read one record per title. In
    the first real set build with the pin H runtime (Pegasus, 2026-10-01) both
    programs interpreted game code at 26 and 35 dispatches a frame and the set's
    own receipt said nothing.
    """
    interpreted = one.get("game_code_interpreted")
    if interpreted and (interpreted.get("per_frame") or 0) >= (receipt.get("game_code_interpreted") or {}).get("per_frame", -1):
        receipt["game_code_interpreted"] = {**interpreted, "program": name}
    overlay = one.get("overlay_code_interpreted")
    if overlay and overlay_rank(overlay) >= overlay_rank(receipt.get("overlay_code_interpreted")):
        receipt["overlay_code_interpreted"] = {**overlay, "program": name}
    reads = one.get("cdrom_reads")
    if reads and reads.get("silent_run_max", 0) >= (receipt.get("cdrom_reads") or {}).get("silent_run_max", -1):
        receipt["cdrom_reads"] = {**reads, "program": name}
    gaps = one.get("bundled_notice_gaps")
    if gaps:
        receipt["bundled_notice_gaps"] = [*(receipt.get("bundled_notice_gaps") or []), *(f"{name}: {gap}" for gap in gaps)]
    memory = one.get("start_memory")
    if memory and memory.get("peak_process_mb") is not None and \
            memory["peak_process_mb"] >= (receipt.get("start_memory") or {}).get("peak_process_mb", -1):
        receipt["start_memory"] = {**memory, "heaviest_start": f"{name} build, start {memory.get('heaviest_start')}"}


def keep_heavier(receipt, earlier):
    """After the set's own starts wrote `start_memory`: keep a program build's record if it was heavier."""
    now = receipt.get("start_memory") or {}
    if earlier and earlier.get("peak_process_mb") is not None and \
            earlier["peak_process_mb"] > now.get("peak_process_mb", -1):
        receipt["start_memory"] = {**earlier, "starts": (earlier.get("starts") or 0) + (now.get("starts") or 0)}


def start_summary(boots):
    """One line per start of a set check, for the receipt."""
    return [{"program": row["program"], "start": row["disc"], "serial": row.get("mounted_serial"),
             "frames": row.get("frames"), "passed": bool(row.get("passed")),
             "peak_process_mb": (row.get("memory") or {}).get("peak_process_mb")} for row in boots]


def check_installed(installed, plan, check, evidence, steps, bios=None):
    """Studio's product rules on the folder a setup pass installed, then every program started from a copy.

    The rules are the private set product's: no player's netplay state, no source
    file outside the allowlist, the bundled programs' licence files (a warning).
    Returns the start summary.

    `bios` is the BIOS the setup pass was given. On a player's machine the wizard
    records the BIOS choice in `bios.cfg` beside the programs; the setup pass run
    as commands has no wizard, so the installed folder has no such file and a
    start would have no BIOS. The copy the programs start from gets that one
    line, as the wizard writes it. The installed folder itself is not touched.
    """
    wo.assert_no_netplay_state(installed)
    wo.assert_no_stray_sources(installed)
    wo.record_bundled_notice_gaps(installed, steps)
    payload = pb.inventory(installed)
    shutil.copytree(installed, check)
    if bios is not None and not (check / "bios.cfg").exists():
        (check / "bios.cfg").write_text(str(bios) + "\n", encoding="utf-8")
        steps["bios_cfg_for_starts"] = "written into the copy the programs start from, as the wizard records it"
    launch_cwd = evidence / "replay-unrelated-cwd"
    launch_cwd.mkdir()
    boots_dir = evidence / "replay-set-boots"
    boots_dir.mkdir()
    programs = [{**program, "executable": program["exe_name"] + ".exe"} for program in plan["programs"]]
    boots = set_boot_check(check, programs, plan, boots_dir, launch_cwd, os.environ.copy())
    require_unchanged_payload(payload, installed)
    lean_boot_gate.record_start_memory(boots, steps)  # the installed folder's starts; warnings join the receipt's
    return start_summary(boots)


def build(args, plan, build_one, plan_receipt=None):
    """Build every program of `plan`, join, check, and deliver one folder or one setup package.

    `build_one(args)` is the single-program build (workbench_output.build_single).
    Returns a receipt in the shape the CLI and Studio already read.

    Private output: the delivered folder is the joined payload and nothing else.
    Every program is started from a copy of it, and the folder itself must still
    match its inventory afterwards (`require_unchanged_payload`).

    Setup output (PS1B-347): one package for the whole set, made by the pin's
    packager from fresh exports and checked against a set package's inventory.

    With the Windows test, the package is unpacked into a clean folder and the
    setup pass is run as commands; the installed folder must pass the product
    rules and start every program. For a private build it must also equal the
    private set product (workbench_set_package.compare_installed).

    The output folder is reserved for the whole build. After a stop, everything
    this build staged beside the output moves into the run's evidence (PS1B-256).
    """
    destination = args.output.resolve()
    cache = runs.run_root(getattr(args, "run_root", None))
    cache.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=cache))
    runs.claim(run, destination)  # which process builds this run (PS1B-351)
    evidence = run / "evidence"
    evidence.mkdir()
    work = run / "work"
    kind = getattr(args, "output_kind", "private")
    receipt = {"status": "started", "output": str(destination), "output_kind": kind,
               "build_route": "workbench_setup_program_set" if kind == "setup" else "workbench_private_program_set",
               "program_set": plan, "programs": [],
               "warnings": [], "logs": str(evidence), "receipt": str(evidence / "build-receipt.json"),
               "host": socket.gethostname(), "run_root": {"path": str(cache), "free_gb_at_start": None},
               "plan_run": {"receipt": (plan_receipt or {}).get("receipt"), "logs": (plan_receipt or {}).get("logs")}}
    pending = {}  # folders this build staged beside the output, by role, until delivery
    reservation = destination / ".workbench-reservation"
    reserved = made = False
    steps = None
    try:
        # Hold the output for the whole set build, and take it first. The planning
        # run released its reservation when it stopped; until this build holds it
        # again, a second build could take the same output. In the first real set
        # build (Eagle, 2026-10-01) the planning run's work tree was removed before
        # the reservation, and the output stood unreserved for that long.
        held = "Another build holds this output directory. Select a new empty directory."
        if reservation.exists():
            raise pb.BuildStop(held)
        if destination.exists() and any(destination.iterdir()):
            raise pb.BuildStop("The output directory is occupied. Select a new empty directory.")
        made = not destination.exists()  # ours to remove after a stop; an existing folder is the operator's
        destination.mkdir(parents=True, exist_ok=True)
        try:
            with reservation.open("x", encoding="utf-8"):
                pass
        except FileExistsError:  # another build reserved it between the check and here
            raise pb.BuildStop(held) from None
        reserved = True
        mark_plan_run(plan_receipt, cache, receipt)
        receipt["run_root"]["free_gb_at_start"] = runs.require_free_space(cache)
        if kind not in ("private", "setup"):
            raise pb.BuildStop(f"A program set builds as a private product or as a setup package, not as {kind}.")
        if set(getattr(args, "platform", None) or ["windows-x64"]) != {"windows-x64"}:
            raise pb.BuildStop("A program set builds for Windows x64 only for now.")
        tests = set(getattr(args, "test", None) or [])
        if tests - {"windows-x64"}:
            raise pb.BuildStop("Only the Windows package test is available for a program set.")
        if kind == "setup" and getattr(args, "optimize_pgo", False):
            raise pb.BuildStop("PGO is a private product option; it cannot be applied to an inputs-free setup package.")
        testing = bool(tests)
        packaging = kind == "setup" or testing
        receipt["automated_tests"] = {"windows-x64": {
            "selected": testing, "status": "requested" if testing else "not run (operator choice)"}}
        pin = pb.load_pin(args.pin)
        repo = args.source_repo.resolve()
        require_runtime_support(repo, pin["commit"])
        if packaging:
            set_package.require_setup_support(repo, pin)  # before any program is built
        pinned_tools = set_package.has_setup_support(repo, pin["commit"])
        if any(not program.get("discs") for program in plan["programs"]):
            raise pb.BuildStop("The set plan does not name a disc for every program.")
        env = fw = None
        if packaging or pinned_tools:
            # The set's own steps: the pin's join, the package, the setup pass. They use one
            # export of the pinned framework, made now so a package can be refused early.
            work.mkdir()
            steps = receipt["set_steps"] = set_steps(cache, pin, kind)
            toolchain = args.toolchain.resolve()
            jobs = pb.resolve_build_jobs(getattr(args, "build_jobs", None))
            env = pb.build_environment(toolchain, work, jobs)
            fw = work / "psxrecomp"
            pb.snapshot(repo, pin["commit"], fw, steps["sources"])
        if pinned_tools and set_package.has_kit_check(fw):
            # Before any program is built: the kits must agree about what one folder holds once.
            set_package.kit_check(fw, plan, work, env, steps)
        if packaging:
            # Before any program is built: what no build would change (PS1B-347).
            set_package.early_admission(work, fw, plan, env, steps)
        built, ones = [], []
        for program in plan["programs"]:
            sub = copy(args)
            sub.disc = list(program["discs"])
            sub.kit = Path(program["kit"])
            sub.test = []  # the set is tested as a whole, below
            sub.output = Path(tempfile.mkdtemp(
                prefix=f".{destination.name} program {program['program']} ", dir=destination.parent))
            sub.output.rmdir()  # the single build wants to create and reserve it
            pending[f"program-{program['program']}"] = sub.output
            print(f"note: building program {program['program']} of {plan['title']}", flush=True)
            one = build_one(sub)
            ones.append(one)
            receipt["programs"].append({"program": program["program"], "status": one.get("status"),
                                        "receipt": one.get("receipt"), "logs": one.get("logs"),
                                        "output": str(sub.output), "kit": program["kit"],
                                        "next_action": one.get("next_action"),
                                        **{name: one[name] for name in PROGRAM_FINDINGS if one.get(name) is not None}})
            # A program's warnings are the set's: a reader of the set's result sees them.
            receipt["warnings"] += [f"{program['program']}: {warning}" for warning in one.get("warnings") or []]
            carry_program_findings(receipt, program["program"], one)
            if one.get("status") == "human-stop":
                raise pb.BuildStop(f"Program {program['program']} did not build: {one.get('next_action')}")
            if kind == "private":
                product = sub.output / "windows-x64"
                buildinfo = json.loads((product / "BUILDINFO.json").read_text(encoding="utf-8"))
                built.append({**program, "product": str(product), "executable": Path(one["executable"]["path"]).name,
                              "buildinfo": buildinfo})
        if packaging or pinned_tools:
            sources = set_package.framework_sources(ones)
            exported = steps["sources"][0]["commit"]
            if sources["psxrecomp"]["commit"] != exported:
                raise pb.BuildStop(f"The programs were built from framework commit {sources['psxrecomp']['commit'][:9]}, "
                                   f"not from the pin's {exported[:9]} that the set's own steps use. Build the set again.")
        merged = None
        if kind == "private":
            staged = Path(tempfile.mkdtemp(prefix=f".{destination.name} set ", dir=destination.parent))
            pending["set"] = staged
            merged = staged / "windows-x64"
            if pinned_tools:
                # One implementation of the join, in the pinned tree: the one a
                # player's setup runs (seam S4).
                set_file = work / set_package.SET_FILE
                set_package.write_set_toml(set_file, plan, set_package.set_exe_name(plan),
                                           set_package.set_bios_stem(kit_configs(plan)))
                receipt["join"] = "the pin's tools/program_set.py"
                try:
                    merge = set_package.join_with_pinned_tool(fw, set_file, built, merged, work, env, steps)
                finally:
                    # Kept with the run after a pass (the work tree goes) and after a stop: a
                    # stopped join's record lists every file the programs disagree about.
                    if (work / "program-set-join.json").is_file():
                        shutil.copy2(work / "program-set-join.json", evidence / "program-set-join.json")
                    differing = set_package.join_disagreements(work)
                    if differing:
                        receipt["join_disagreements"] = differing
            else:
                merge = merge_products(built, merged, plan)
                receipt["join"] = "workbench_set.merge_products (the pin has no tools/program_set.py)"
            payload = pb.inventory(merged)  # what may be delivered, before anything is started
            # Start the programs in a copy. What a start writes stays in the copy.
            check = Path(tempfile.mkdtemp(prefix=f".{destination.name} set check ", dir=destination.parent))
            pending["set-check"] = check
            shutil.copytree(merged, check / "windows-x64")
            launch_cwd = evidence / "unrelated-cwd"
            launch_cwd.mkdir()
            boots_dir = evidence / "set-boots"
            boots_dir.mkdir()
            boots = set_boot_check(check / "windows-x64", built, plan, boots_dir, launch_cwd, os.environ.copy())
            summary = start_summary(boots)
            receipt["set_boot_check"] = summary  # now: a stop after this point must not lose the starts
            from_programs = receipt.pop("start_memory", None)
            lean_boot_gate.record_start_memory(boots, receipt)  # limit and heaviest process (PS1B-352)
            keep_heavier(receipt, from_programs)
            receipt["set_check_leftovers"] = leftovers(payload, check / "windows-x64")
            require_unchanged_payload(payload, merged)
            wo.record_bundled_notice_gaps(merged, receipt)  # a warning and a receipt field, never a stop (PS1B-345)
            info = write_set_provenance(merged, plan, built, merge, summary)
            receipt["merge"] = info["merge"]
        package = None
        if packaging:
            package, _set_toml, host = set_package.make_package(
                work, fw, plan, sources, toolchain, jobs, env, steps, getattr(args, "overlay_archives", None))
            receipt["windows_package"] = steps["windows_package"]
            receipt["setup_exe_name"] = host
        if testing:
            test = receipt["automated_tests"]["windows-x64"]
            test["status"] = "running"
            discs = [next(Path(program["discs"][program["positions"].index(position)])
                          for program in plan["programs"] if position in program["positions"])
                     for position in range(1, len(plan["serials"]) + 1)]
            bios = Path(args.bios) if Path(args.bios).is_absolute() else Path(args.bios).absolute()
            stem = set_package.set_bios_stem(kit_configs(plan))
            installed = set_package.install(package, work / "set-kit", plan, host, discs, bios, stem,
                                            env, work, steps, getattr(args, "overlay_archives", None))
            test["evidence"] = set_package.record_installed(installed.parent, installed, evidence)
            if merged is not None:
                comparison = set_package.compare_installed(merged, installed, plan)
                test["comparison"] = comparison
            else:
                test["comparison"] = "not made: a setup build has no private set product to compare with"
                # With nothing to compare with, at least the package's own licence texts must be
                # beside the programs. Judged after the starts, like the comparison.
                test["licences"] = single.installed_notices(installed.parent, installed)
            test["starts"] = check_installed(installed, plan, work / "set-kit-check", evidence, steps, bios=bios)
            if steps.get("start_memory"):
                test["start_memory"] = steps.pop("start_memory")
            if merged is not None:
                # After the starts, so that one run says both things: whether the installed
                # programs start, and how the folder differs. The first real comparison
                # stopped before any installed program had been started.
                set_package.require_equal(comparison)
            else:
                single.require_installed_notices(test["licences"])
            test.update(status="passed", package_sha256=receipt["windows_package"]["sha256"],
                        scope="The set's package unpacked into a clean folder, the setup pass run as commands, "
                              "the installed folder checked by the product rules, and every program started "
                              "from a copy; no TAS or whole-game qualification.")
        if kind == "private":
            reservation.unlink()
            reserved = False
            destination.rmdir()  # only our verified empty, reserved output directory
            staged.rename(destination)
            pending.pop("set")
            if package is not None:  # kept with the run, as a single private build keeps its test package
                shutil.copy2(package, evidence / package.name)
                receipt["windows_package"] = pb.identity(evidence / package.name)
        else:
            staged = Path(tempfile.mkdtemp(prefix=f".{destination.name} set package ", dir=destination.parent))
            pending["set-package"] = staged
            shutil.copy2(package, staged / package.name)
            if pb.sha(staged / package.name) != receipt["windows_package"]["sha256"]:
                raise pb.BuildStop("The set's package changed while it was copied to the output.")
            reservation.unlink()
            reserved = False
            destination.rmdir()  # only our verified empty, reserved output directory
            staged.rename(destination)
            pending.pop("set-package")
            receipt["windows_package"] = pb.identity(destination / package.name)
        # The copy and the single-program products are replaced by the delivered folder.
        for role in [r for r in pending]:
            error = remove_folder(pending[role])
            if error is None:
                pending.pop(role)
            else:
                receipt["warnings"].append(
                    f"A folder this build staged beside the output could not be removed: {pending[role]} ({error}). "
                    "Remove it by hand; it is not part of the product.")
        if kind == "private":
            receipt.update(status="build-pass", set_boot_check=summary, merge=info["merge"],
                           executables=[pb.identity(destination / "windows-x64" / row["executable"])
                                        for row in merge["per_program"]],
                           shortcuts=info["shortcuts"],
                           qualification="One build per program; each passed its own startup gate, and every program "
                                         "started from a copy of the merged folder. No TAS or whole-game qualification.")
            # Readers that take one executable get the first program's.
            receipt["executable"] = receipt["executables"][0]
        else:
            receipt.update(status="setup-package-pass",
                           qualification="Inputs-free setup package for the whole set; no private product or "
                                         "gameplay qualification."
                                         + (" The setup pass was run on a clean folder and every program started."
                                            if testing else ""))
        if pending:
            receipt["staging_left"] = [str(folder) for folder in pending.values()]
        receipt["outputs"] = pb.inventory(destination)
    except Exception as error:  # noqa: BLE001
        receipt.update(status="human-stop", next_action=str(error))
        for test in (receipt.get("automated_tests") or {}).values():
            if test.get("status") == "running":
                test["status"] = "failed"
            elif test.get("status") == "requested":
                test["status"] = "not run (the build stopped before the test)"
        (evidence / "HUMAN-STOP.txt").write_text(str(error) + "\n", encoding="utf-8")
        receipt["stop_file"] = str(evidence / "HUMAN-STOP.txt")
        # Nothing this build staged stays in the operator's output root: finished
        # program products, the merged folder and the check copy all move to the
        # run's evidence. A program that stopped left only an empty folder.
        for role in [r for r in pending]:
            folder = pending[role]
            if folder.is_dir() and not any(folder.iterdir()):
                folder.rmdir()
                pending.pop(role)
        wo.retain_failed_payloads(pending, evidence, receipt)
    finally:
        if reserved:
            reservation.unlink(missing_ok=True)
            if made and destination.is_dir() and not any(destination.iterdir()):
                destination.rmdir()  # the empty folder this build made for the reservation
        stopped = receipt["status"] == "human-stop"
        if steps is not None:
            for path in work.glob("*.log") if work.is_dir() else []:
                shutil.copy2(path, evidence / path.name)
            for row in [*steps.get("commands", []), *steps.get("stages", [])]:
                if row.get("log") and (evidence / Path(row["log"]).name).is_file():
                    row["log"] = str(evidence / Path(row["log"]).name)
            pb.progress.finish(steps, evidence, stopped, receipt.get("next_action"))
            if stopped and steps.get("failed_stage"):
                receipt.setdefault("failed_stage", steps["failed_stage"])  # the CLI result names it
            receipt["warnings"] += steps.pop("warnings", [])
            if steps.get("bundled_notice_gaps"):
                receipt["installed_bundled_notice_gaps"] = steps["bundled_notice_gaps"]
        # A passed build keeps no work tree; a stopped one keeps it for the
        # diagnosis, under the same retention as a single build's (PS1B-328).
        receipt["work_directory"] = str(work)
        if work.is_dir() and not stopped and not getattr(args, "keep_development_files", False):
            error = remove_folder(work)
            if error:
                receipt["warnings"].append(f"The set build's work folder could not be removed: {work} ({error}).")
        receipt["work_retained"] = work.is_dir()
        pb.write_json(evidence / "build-receipt.json", receipt)
        if steps is not None:
            runs.after_build(cache, run, evidence, receipt)
            pb.write_json(evidence / "build-receipt.json", receipt)
    return receipt
