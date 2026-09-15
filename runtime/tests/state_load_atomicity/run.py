"""Check actual full-runtime state using an existing Windows GCC/Ninja consumer.

No guest code runs. The linked image may still contain private generated code;
put --output on private scratch storage and retain only source/receipt evidence.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def windows_argv(command):
    count = ctypes.c_int()
    api = ctypes.WinDLL("shell32").CommandLineToArgvW
    api.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    api.restype = ctypes.POINTER(ctypes.c_wchar_p)
    pointer = api(command, ctypes.byref(count))
    if not pointer:
        raise RuntimeError("Cannot parse the compiler command")
    try:
        return [pointer[i] for i in range(count.value)]
    finally:
        free = ctypes.WinDLL("kernel32").LocalFree
        free.argtypes = [ctypes.c_void_p]
        free.restype = ctypes.c_void_p
        free(ctypes.cast(pointer, ctypes.c_void_p))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-build", type=Path, required=True)
    parser.add_argument("--build-target", required=True)
    parser.add_argument("--executable-output", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=2)
    args = parser.parse_args()
    if os.name != "nt" or args.repeat < 1:
        parser.error("This runner requires Windows and a positive repeat count")
    source = Path(__file__).resolve().parent
    framework = source.parents[2]
    build = args.runtime_build.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    receipt = {"commands": [], "qualification_passed": False}

    def save():
        (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")

    def run(label, command, cwd=output, timeout=180):
        command = list(map(str, command))
        start = time.monotonic()
        result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
        (output / (label + ".stdout")).write_bytes(result.stdout)
        (output / (label + ".stderr")).write_bytes(result.stderr)
        receipt["commands"].append({"label": label, "argv": command, "cwd": str(cwd),
                                    "exit": result.returncode,
                                    "seconds": time.monotonic() - start})
        save()
        print(label, result.returncode, flush=True)
        if result.returncode:
            raise RuntimeError(f"{label} failed; see {output / (label + '.stderr')}")
        return result.stdout.decode("utf-8", errors="replace")

    try:
        receipt["source_head"] = run("head", ["git", "-C", framework, "rev-parse", "HEAD"]).strip()
        receipt["source_status"] = run("status", ["git", "-C", framework, "status", "--porcelain"])
        run("build", ["cmake", "--build", build, "--target", args.build_target, "--parallel", "2"],
            timeout=1800)
        commands = json.loads(run("compdb", ["ninja", "-C", build, "-t", "compdb", "-x"]))
        main_source = framework / "runtime/src/main.cpp"
        entries = [entry for entry in commands
                   if Path(entry.get("file", "")).resolve() == main_source]
        if len(entries) != 1:
            raise RuntimeError("Expected exactly one compile command for this source tree's main.cpp")
        entry = entries[0]
        compile_args = windows_argv(entry["command"])
        prefix_count = 2 if Path(compile_args[0]).stem.lower() == "ccache" else 1
        compiler = compile_args[:prefix_count]
        machine = run("compiler-target", [*compiler, "-dumpmachine"]).strip()
        version = run("compiler-version", [*compiler, "-dumpfullversion"]).strip()
        if machine != "x86_64-w64-mingw32" or version != "16.1.0":
            raise RuntimeError("This fixture is qualified for Windows x64 GCC 16.1.0")
        fixture = source / "fixture.cpp"
        compile_args[compile_args.index("-c") + 1] = str(fixture)
        compile_args[compile_args.index("-o") + 1] = str(output / "fixture.obj")
        compile_args[compile_args.index("-MF") + 1] = str(output / "fixture.d")
        compile_args[compile_args.index("-MT") + 1] = "fixture.obj"
        compile_args.append(f'-DPSX_RUNTIME_MAIN_SOURCE="{main_source.as_posix()}"')
        run("compile", compile_args, build)

        links = [entry for entry in commands if entry.get("output") == args.executable_output]
        if len(links) != 1:
            raise RuntimeError("Expected one explicit executable link command")
        # Ninja's Windows link edge has a cd prefix and optional post-link step.
        # Execute only the compiler argv, without passing its shell text onward.
        direct = links[0]["command"].split(" && ", 1)[1].split(" && ", 1)[0]
        link_args = [arg for arg in windows_argv(direct) if arg != "-mwindows"]
        main_objects = [i for i, arg in enumerate(link_args)
                        if arg.replace("\\", "/").endswith("/main.cpp.obj")]
        if len(main_objects) != 1:
            raise RuntimeError("Expected exactly one front-end object in the link")
        link_args[main_objects[0]] = str(output / "fixture.obj")
        executable = output / "full_machine_state.exe"
        link_args[link_args.index("-o") + 1] = str(executable)
        link_args.extend(["-Wl,--wrap=malloc", "-Wl,--wrap=realloc"])
        receipt["linked_inputs"] = {}
        for arg in link_args[1:]:
            path = Path(arg)
            if not path.is_absolute():
                path = build / path
            if path.is_file() and path != executable:
                receipt["linked_inputs"][str(path.resolve())] = sha256(path)
        receipt["source_files"] = {
            str(path.relative_to(framework)): sha256(path)
            for path in [fixture, Path(__file__), *sorted((framework / "runtime/include").glob("*.h")),
                         *sorted((framework / "runtime/src").glob("*.c")),
                         *sorted((framework / "runtime/src").glob("*.cpp"))]
        }
        run("link", link_args, build)
        bios = output / "original_zero_bios.bin"
        bios.write_bytes(bytes(524288))
        receipt["executable_sha256"] = sha256(executable)
        receipt["synthetic_bios_sha256"] = sha256(bios)
        for repeat in range(args.repeat):
            run(f"test{repeat}", [executable, bios])
        receipt["qualification_passed"] = True
    except Exception as exc:
        receipt["error"] = str(exc)
    finally:
        save()
    print(json.dumps({key: receipt.get(key) for key in ["qualification_passed", "error"]}))
    return 0 if receipt["qualification_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
