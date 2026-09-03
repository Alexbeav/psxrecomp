#!/usr/bin/env python3
"""Keep Unix setup on native build tools instead of the Windows pack."""

from pathlib import Path
import re
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    source = (root / "host/psxrecomp_codegen_host.c").read_text(encoding="utf-8")

    ready = re.search(
        r"static int host_system_toolchain_ready\(void\) \{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not ready:
        raise AssertionError("POSIX native-tool readiness check is missing")
    for tool in ("cmake", "ninja", "cc", "c++"):
        if f'find_on_path("{tool}"' not in ready.group("body"):
            raise AssertionError(f"POSIX readiness does not check {tool}")
    if 'find_on_path("python3"' not in ready.group("body"):
        raise AssertionError("POSIX readiness does not check native Python")
    if "find_python(" in ready.group("body"):
        raise AssertionError("POSIX readiness can select a cached Windows Python")
    if ready.group("body").count("posix_command_runs(") < 5:
        raise AssertionError("POSIX readiness does not execute every selected tool")

    ensure = re.search(
        r"static int host_ensure_toolchain_with_progress\((?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not ensure or "Native build tools are missing" not in ensure.group("body"):
        raise AssertionError("POSIX setup does not report missing native tools")
    if "if (host_system_toolchain_ready())" not in ensure.group("body"):
        raise AssertionError("POSIX setup does not accept native build tools")

    update = re.search(
        r"static int host_toolchain_update_available\((?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not update or "#if !defined(_WIN32)\n    return 0;" not in update.group("body"):
        raise AssertionError("POSIX setup can still offer the Windows pack update")

    print("POSIX setup native-toolchain source guard: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
