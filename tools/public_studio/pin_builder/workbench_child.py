"""Tee this pin CLI's captured emitter/CMake output without editing its source."""
import json
from pathlib import Path
import runpy
import subprocess
import sys

try:
    from . import workbench_progress as progress
except ImportError:
    import workbench_progress as progress


def captured_run(original, argv, **kwargs):
    name = Path(str(argv[0])).stem.lower() if isinstance(argv, (list, tuple)) and argv else ""
    if name not in ("cmake", "ninja", "psxrecomp-game", "psxrecomp-bios") or not kwargs.get("capture_output"):
        return original(argv, **kwargs)
    # toolchain_pack.toolchain_bin_runs uses a short version probe with a
    # timeout. Keep its native subprocess semantics; it has no live build work.
    if name == "cmake" and list(map(str, argv[1:])) == ["--version"]:
        return original(argv, **kwargs)
    allowed = {"capture_output", "text", "encoding", "errors", "cwd", "env", "check"}
    if set(kwargs) - allowed or not kwargs.get("text") or kwargs.get("encoding", "utf-8").lower() != "utf-8":
        raise RuntimeError(f"Unsupported pinned CLI capture shape for {name}: {sorted(kwargs)}")
    def emit(event, **values):
        print(progress.CHILD_PREFIX + json.dumps({"event": event, "argv": list(map(str, argv)), **values}), flush=True)
    emit("start")
    result = progress.stream(argv, cwd=kwargs.get("cwd"), env=kwargs.get("env"),
        on_line=lambda channel, line: emit("output", channel=channel, line=line))
    emit("end", exit_code=result.returncode)
    # Preserve subprocess.run(text=True)'s captured values for the pin's own
    # parsing/error handling. The pin can still replay these after completion.
    encoding, errors = kwargs.get("encoding", "utf-8"), kwargs.get("errors", "strict")
    result.stdout = result.stdout.decode(encoding, errors).replace("\r\n", "\n").replace("\r", "\n")
    result.stderr = result.stderr.decode(encoding, errors).replace("\r\n", "\n").replace("\r", "\n")
    if kwargs.get("check") and result.returncode:
        raise subprocess.CalledProcessError(result.returncode, argv, result.stdout, result.stderr)
    return result


def main():
    progress.configure_stdio()
    script = Path(sys.argv[1]).resolve()
    if script.name != "psxrecomp_cli.py":
        raise RuntimeError("The Workbench tee adapter only accepts psxrecomp_cli.py.")
    original = subprocess.run
    subprocess.run = lambda argv, **kwargs: captured_run(original, argv, **kwargs)
    sys.argv = [str(script), *sys.argv[2:]]
    sys.path.insert(0, str(script.parent))
    try:
        runpy.run_path(str(script), run_name="__main__")
    finally:
        subprocess.run = original


if __name__ == "__main__":
    main()
