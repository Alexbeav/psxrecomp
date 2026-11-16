#!/usr/bin/env python3
"""PSX Workbench authoritative offline command."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

try:
    from . import (
        autonomy_run,
        baseline_workflow,
        chd_intake,
        component_freeze,
        intervention_ledger,
        m3u_intake,
        retention_policy,
        pin_build,
        studio_settings,
        workbench_runs,
    )
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import autonomy_run  # type: ignore
    import baseline_workflow  # type: ignore
    import chd_intake  # type: ignore
    import component_freeze  # type: ignore
    import intervention_ledger  # type: ignore
    import m3u_intake  # type: ignore
    import retention_policy  # type: ignore
    import pin_build  # type: ignore
    import studio_settings  # type: ignore
    import workbench_runs  # type: ignore


def main() -> int:
    pin_build.progress.configure_stdio()
    # PS1B-586: the Studio record's per-host paths fill each WORKBENCH_* variable
    # that is still unset, before any option default is read.
    # The settings command reports the environment as it was before that.
    ambient = dict(os.environ)
    studio_settings.apply_to_environ(os.environ)
    parser = argparse.ArgumentParser(prog="workbench", description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pin_build.add_commands(commands)
    settings = commands.add_parser("settings", help="Show, check or save this host's build paths (PS1B-586)")
    settings.add_argument("--pin", type=Path, help="PIN.json whose commits the clone settings must hold.")
    settings.add_argument("--record", type=Path, help="Studio record to read and write. Default: WORKBENCH_STUDIO_RECORD, then the share's.")
    settings.add_argument("--host", help="Host name. Default: this computer's name.")
    settings.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                          help="Save one setting into hosts.<HOST>.paths; an empty VALUE removes it. Repeatable.")
    run_parser = commands.add_parser("run")
    run_parser.add_argument("local_binding", type=Path)
    run_parser.add_argument("route_set", type=Path)
    for name in ("baseline-preflight", "baseline-run"):
        baseline = commands.add_parser(name)
        baseline.add_argument("project", type=Path)
        baseline.add_argument("disc", type=Path)
        baseline.add_argument("bios", type=Path)
        baseline.add_argument("canonical_project", type=Path)
        baseline.add_argument("--package-root", type=Path,
                              default=Path(__file__).resolve().parent.parent)
        baseline.add_argument("--psxrecomp", type=Path)
        baseline.add_argument("--cmake", type=Path)
        baseline.add_argument("--ninja", type=Path)
        baseline.add_argument("--sdl-source", type=Path)
        baseline.add_argument("--libchdr-source", type=Path)
        baseline.add_argument("--toolchain-bin", type=Path)
        baseline.add_argument("--recomp-ui", type=Path)
        baseline.add_argument("--multi-disc-receipt", type=Path)
        baseline.add_argument("--build-jobs", action="append")
    for name in ("baseline-go-preflight", "baseline-go"):
        go = commands.add_parser(name)
        go.add_argument("canonical_project", type=Path)
        go.add_argument("disc", type=Path)
        go.add_argument("bios_backend", choices=("openbios", "scph1001"))
        go.add_argument("--package-root", type=Path,
                        default=Path(__file__).resolve().parent.parent)
        go.add_argument("--registry", type=Path)
        go.add_argument("--title")
        go.add_argument("--multi-disc-receipt", type=Path)
        go.add_argument("--build-jobs", action="append")
    register = commands.add_parser("bios-register")
    register.add_argument("bios", type=Path)
    register.add_argument("--registry", type=Path)
    scaffold = commands.add_parser("baseline-scaffold")
    scaffold.add_argument("disc", type=Path)
    scaffold.add_argument("canonical_project", type=Path)
    scaffold.add_argument("--title", required=True)
    scaffold.add_argument("--multi-disc-receipt", type=Path)
    for name in ("chd-preview", "chd-extract"):
        chd = commands.add_parser(name)
        chd.add_argument("source", type=Path)
        chd.add_argument("stage", type=Path)
        chd.add_argument("--chdman", type=Path)
    for name in ("m3u-preview", "m3u-extract", "m3u-validate"):
        m3u = commands.add_parser(name)
        if name == "m3u-validate":
            m3u.add_argument("receipt", type=Path)
        else:
            m3u.add_argument("playlist", type=Path)
            m3u.add_argument("stage", type=Path)
            m3u.add_argument("--chdman", type=Path)
    freeze_parser = commands.add_parser("freeze")
    freeze_parser.add_argument("local_binding", type=Path)
    freeze_parser.add_argument("route_set", type=Path)
    freeze_parser.add_argument("output", type=Path)
    event_parser = commands.add_parser("record-intervention")
    event_parser.add_argument("ledger", type=Path)
    event_parser.add_argument("category", choices=sorted(intervention_ledger.CATEGORIES))
    event_parser.add_argument("detail")
    event_parser.add_argument("--wall-seconds", type=float)
    event_parser.add_argument("--machine-seconds", type=float)
    event_parser.add_argument("--agent-seconds", type=float)
    event_parser.add_argument("--compute-cost", type=float)
    retention = commands.add_parser("retention-plan")
    retention.add_argument("namespace", type=Path)
    retention.add_argument("inventory", type=Path)
    retention.add_argument("--evaluated-at-utc", required=True)
    retention.add_argument("--min-age-days", type=float, default=0)
    retention.add_argument("--min-size-bytes", type=int, default=0)
    args = parser.parse_args()
    if hasattr(args, "build_jobs"):
        values = args.build_jobs or []
        if len(values) > 1:
            parser.error("--build-jobs must be specified at most once")
        if values:
            value = values[0]
            if not value.isascii() or not value.isdecimal():
                parser.error("--build-jobs must be an ASCII decimal integer")
            args.build_jobs = int(value)
            if not 1 <= args.build_jobs <= 32:
                parser.error("--build-jobs must be from 1 through 32")
        else:
            args.build_jobs = None
    try:
        if args.command == "settings":
            values = {}
            for item in args.set:
                key, sep, value = item.partition("=")
                if not sep:
                    raise ValueError(f"--set needs KEY=VALUE: {item}")
                values[key.strip()] = value
            if values:
                result = studio_settings.save(values, args.record, args.host, ambient)
                result = studio_settings.report(args.record, args.host, args.pin, ambient) if args.pin else result
            else:
                result = studio_settings.report(args.record, args.host, args.pin, ambient)
            print(json.dumps({"command": "settings", "ok": True, "result": result}, sort_keys=True))
            return 0
        if args.command == "runs":
            root = workbench_runs.run_root(args.run_root)
            rows = workbench_runs.prune(root, execute=args.prune, keep_failed=args.keep_failed)
            print(json.dumps({"command": "runs", "ok": not any(r.get("error") for r in rows), "result": {
                "run_root": str(root), "pruned": bool(args.prune),
                "bytes": sum(r["removed_bytes"] for r in rows), "runs": rows}}, sort_keys=True))
            return 0
        if args.command in ("pins", "build"):
            result = pin_build.pins(args.pin_root, args.pin, args.ui_root, args.ui) \
                if args.command == "pins" else pin_build.build(args)
            ok = result.get("status") != "human-stop"
            print(json.dumps({"command": args.command, "ok": ok, "result": result if args.command == "pins" else {
                "status": result["status"], "next_action": result.get("next_action"),
                "warnings": result.get("warnings", []),
                "failed_stage": result.get("failed_stage"),
                "stop_file": result.get("stop_file") if not ok else None,
                "output": result.get("output"), "logs": result.get("logs"),
                "work_directory": result.get("work_directory"), "work_retained": result.get("work_retained"),
                "stage_history": result.get("stage_history"), "private_validation": result.get("private_validation"),
                "optimize_pgo": result.get("optimize_pgo", False),
                "keep_development_files": result.get("keep_development_files", False),
                "output_kind": result.get("output_kind"), "input_mode": result.get("input_mode"),
                "kit": result.get("kit"), "kit_review": result.get("kit_review"), "host": result.get("host"),
                "disc_reader": (result.get("kit_preflight") or {}).get("reader"),
                "game_code_interpreted": result.get("game_code_interpreted"),
                "overlay_code_interpreted": result.get("overlay_code_interpreted"),
                "bundled_notice_gaps": result.get("bundled_notice_gaps"),
                "cdrom_reads": result.get("cdrom_reads"),
                "start_memory": result.get("start_memory"),
                "overlay_compile_check": result.get("overlay_compile_check"),
                "start_residue": result.get("start_residue"),
                "overlay_compile_starts": result.get("overlay_compile_starts"),
                "package_mode": result.get("package_mode"), "executable": result.get("executable"),
                "windows_package": result.get("windows_package"),
                "receipt": result.get("receipt", str(args.output.resolve() / "build-receipt.json"))}}, sort_keys=True))
            return 0 if ok else 2
        elif args.command == "m3u-validate":
            result = m3u_intake.validate_receipt(args.receipt.resolve())
        elif args.command in ("m3u-preview", "m3u-extract"):
            package_root = Path(__file__).resolve().parent.parent
            tool = args.chdman or package_root / "native" / "chdman" / "chdman.exe"
            result = m3u_intake.preview(args.playlist.resolve(), args.stage.resolve(), tool.resolve()) \
                if args.command == "m3u-preview" else m3u_intake.extract(
                    args.playlist.resolve(), args.stage.resolve(), tool.resolve())
        elif args.command in ("chd-preview", "chd-extract"):
            package_root = Path(__file__).resolve().parent.parent
            tool = args.chdman or package_root / "native" / "chdman" / "chdman.exe"
            result = chd_intake.preview(args.source.resolve(), args.stage.resolve(), tool.resolve()) \
                if args.command == "chd-preview" else chd_intake.extract(
                    args.source.resolve(), args.stage.resolve(), tool.resolve())
        elif args.command == "baseline-scaffold":
            result = baseline_workflow.scaffold_project(
                args.disc, args.canonical_project, args.title,
                multi_disc_receipt=args.multi_disc_receipt)
        elif args.command == "bios-register":
            result = baseline_workflow.register_retail_bios(
                args.bios, args.registry)
        elif args.command in ("baseline-go-preflight", "baseline-go"):
            if args.command == "baseline-go-preflight":
                result = baseline_workflow.happy_preflight(
                    args.canonical_project, args.disc, args.bios_backend,
                    args.package_root, registry_path=args.registry,
                    project_title=args.title,
                    multi_disc_receipt=args.multi_disc_receipt,
                    build_jobs=args.build_jobs)
            else:
                result = baseline_workflow.happy_run(
                    args.canonical_project, args.disc, args.bios_backend,
                    args.package_root, registry_path=args.registry,
                    project_title=args.title,
                    multi_disc_receipt=args.multi_disc_receipt,
                    build_jobs=args.build_jobs)
        elif args.command in ("baseline-preflight", "baseline-run"):
            options = {
                "psxrecomp": args.psxrecomp, "cmake": args.cmake,
                "ninja": args.ninja, "sdl_source": args.sdl_source,
                "libchdr_source": args.libchdr_source,
                "toolchain_bin": args.toolchain_bin,
                "recomp_ui": args.recomp_ui,
                "multi_disc_receipt": args.multi_disc_receipt,
                "build_jobs": args.build_jobs,
            }
            if args.command == "baseline-preflight":
                result = baseline_workflow.preflight(
                    args.project, args.disc, args.bios, args.package_root,
                    args.canonical_project, **options)
            else:
                result = baseline_workflow.run(
                    args.project, args.disc, args.bios, args.package_root,
                    args.canonical_project, **options)
        elif args.command == "retention-plan":
            result = retention_policy.plan(
                args.namespace, args.inventory,
                evaluated_at_utc=args.evaluated_at_utc,
                min_age_days=args.min_age_days,
                min_size_bytes=args.min_size_bytes)
        elif args.command == "run":
            result = autonomy_run.run(args.local_binding, args.route_set)
        elif args.command == "freeze":
            result = component_freeze.freeze(args.local_binding, args.route_set, args.output)
        else:
            result = json.loads(args.ledger.read_text(encoding="utf-8"))
            if result.get("status") != "running":
                raise intervention_ledger.LedgerError("ledger is not running")
            intervention_ledger.record_intervention(
                result, args.category, args.detail,
                wall_seconds=args.wall_seconds,
                machine_seconds=args.machine_seconds,
                agent_seconds=args.agent_seconds,
                compute_cost=args.compute_cost,
            )
            intervention_ledger.write(args.ledger, result)
        print(json.dumps({"command": args.command, "ok": True, "result": result}, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        payload = {"command": args.command, "ok": False, "error": str(error)}
        if args.command == "build":
            # A build that stops before its run folder exists (occupied output,
            # too little free space) has no receipt. Callers read result.status
            # and result.next_action, so give them the shape of a stopped build.
            payload["result"] = {"status": "human-stop", "next_action": str(error), "failed_stage": None,
                                 "stopped_before_run": True, "warnings": [], "stop_file": None,
                                 "output": None, "logs": None, "receipt": None}
        print(json.dumps(payload, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
