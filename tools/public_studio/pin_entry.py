"""Run the established PIN.json/UI.json builder with explicit edition paths."""
import argparse
import json
import os
from pathlib import Path
import sys
from pin_builder import pin_build

if "--no-op" in sys.argv:
    assert Path(__file__).with_name("pin_builder").joinpath("workbench_child.py").is_file()
    print("noop: established pin builder imports; no sources fetched or process started")
    raise SystemExit(0)
parser = argparse.ArgumentParser()
commands = parser.add_subparsers(dest="command", required=True)
pin_build.add_commands(commands)
commands.choices["build"].add_argument("--bios-candidates", type=Path)
commands.choices["build"].add_argument("--reader-only", action="store_true")
commands.choices["build"].add_argument("--compiler-optimization", choices=("fast", "optimized"), default="optimized")
commands.choices["build"].add_argument("--disable-lto", action="store_true")
commands.choices["build"].add_argument("--diagnostics", action="store_true")
commands.choices["build"].add_argument("--startup-checks", action="store_true")
args = parser.parse_args()
args.build_jobs = "1"
if args.command != "build":
    parser.error("This edition entry runs builds only")
if args.bios_candidates:
    candidates = [Path(path) for path in json.loads(args.bios_candidates.read_text(encoding="utf-8"))]
    serials = []
    reader_receipt = {"commands": [], "output": str(args.source_repo.parent / "reader-check")}
    reader_output = Path(reader_receipt["output"])
    reader_output.mkdir(parents=True, exist_ok=True)
    dll_directory = None
    if os.name == "nt":
        dll_directory = os.add_dll_directory(str(args.toolchain / "bin"))
    for image in map(Path, args.disc):
        if image.suffix.lower() == ".chd":
            environment = pin_build.build_environment(args.toolchain, reader_output, 1)
            decoder = pin_build.ensure_libchdr(args.source_repo, reader_output, reader_receipt,
                                              env=environment, toolchain=args.toolchain)
            if decoder is None:
                raise RuntimeError("The selected core's CHD reader could not be built; see chd-reader logs")
            library = decoder.find_libchdr(args.source_repo.parent, args.source_repo)
            if library is None:
                raise RuntimeError("The selected core's built CHD library could not be loaded")
            decoder.LibChdr(library)  # Surface DLL dependency errors before identity helpers.
            os.environ["PSXRECOMP_LIBCHDR"] = str(library)
            serial = pin_build.read_chd_boot_serial_psx_chd(decoder, image, library)
        else:
            serial = pin_build.baseline_workflow.disc_boot_identity(image)["serial"]
        if not serial:
            raise RuntimeError(f"Could not read the boot serial from {image.name}")
        serials.append(serial)
    wanted = pin_build.region_bios_stem(serials)
    profiles = pin_build.bios_profiles(args.source_repo)
    for candidate in candidates:
        if candidate.is_file() and profiles.get(pin_build.sha(candidate)) == wanted:
            args.bios = str(candidate)
            break
    (reader_output / "reader-record.json").write_text(json.dumps({"decoder": reader_receipt,
        "serials": serials, "bios": str(args.bios), "access": "read-only CHD decoder"}, indent=2), encoding="utf-8")
if args.reader_only:
    print(json.dumps({"state": "reader-pass", "serials": serials, "bios": str(args.bios),
                      "libchdr": os.environ.get("PSXRECOMP_LIBCHDR")}))
    raise SystemExit(0)
result = pin_build.build(args)
print(json.dumps({"status": result.get("status"), "output": result.get("output"),
                  "receipt": result.get("receipt"), "next_action": result.get("next_action")}))
raise SystemExit(0 if result.get("status") in {"build-pass", "setup-package-pass"} else 1)
