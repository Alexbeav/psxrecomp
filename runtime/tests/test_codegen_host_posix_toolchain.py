#!/usr/bin/env python3
"""Unix setup: native build tools first, then the pack built for this OS.

A Windows pack must never reach PATH on Linux or macOS, and a pack must never
shadow native cmake/ninja. Without native tools, the cmake-clang-v1 pack for
this OS supplies cmake/ninja (v1.0.14 publishes linux-x64 and macos-universal).
"""

from pathlib import Path
import re
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    source = (root / "host/psxrecomp_codegen_host.c").read_text(encoding="utf-8")
    cli = (root / "psxrecomp_cli.py").read_text(encoding="utf-8")

    resolve_cli = re.search(
        r"def resolve_embedded_toolchain_bin\(.*?(?=\n\ndef )",
        cli,
        re.DOTALL,
    )
    if not resolve_cli or 'sys.platform != "win32"' not in resolve_cli.group():
        raise AssertionError("POSIX CLI can resolve a cached Windows toolchain")
    if "return None" not in resolve_cli.group():
        raise AssertionError("POSIX CLI does not reject cached toolchain paths")

    activate_cli = re.search(
        r"def activate_embedded_toolchain\(.*?(?=\n\ndef )",
        cli,
        re.DOTALL,
    )
    if not activate_cli or "_native_toolchain_ready(log)" not in activate_cli.group():
        raise AssertionError("POSIX CLI does not accept native build tools")
    # Without native cmake/ninja, the pack ensure-toolchain installed for this
    # OS must be used (SF2 0.2.0 macOS replay: it was downloaded, then ignored).
    if "_host_pack_bin(project_root)" not in activate_cli.group():
        raise AssertionError("POSIX CLI ignores the host-OS toolchain pack")
    host_pack = re.search(r"def _host_pack_bin\(.*?(?=\n\ndef )", cli, re.DOTALL)
    if not host_pack or "pack_matches_host(" not in host_pack.group():
        raise AssertionError("POSIX CLI can activate a pack built for another OS")

    pack_src = (root / "tools/toolchain_pack.py").read_text(encoding="utf-8")
    hook = re.search(r"def _register_user_path_unix\(.*?(?=\n\ndef )", pack_src, re.DOTALL)
    if not hook or "env.sh" in hook.group().split("hook.write_text(", 1)[1].split("encoding=", 1)[0]:
        raise AssertionError("shell PATH hook sources the bash-only env.sh")

    sys.path.insert(0, str(root / "tools"))
    import json, tempfile
    from toolchain_pack import host_artifact, pack_matches_host
    host_os = {"windows": "windows-x64", "macos": "macos-universal", "linux": "linux-x64"}
    mine = host_os[host_artifact().split("-", 1)[0]]
    with tempfile.TemporaryDirectory() as tmp:
        for pack_os in ("windows-x64", "macos-universal", "linux-x64"):
            pack = Path(tmp) / pack_os
            pack.mkdir()
            (pack / "retcomm-toolchain.json").write_text(json.dumps({"os": pack_os}), encoding="utf-8")
            if pack_matches_host(pack) != (pack_os == mine):
                raise AssertionError(f"pack_matches_host({pack_os}) wrong on {mine}")

    ensure_cli = re.search(
        r"def ensure_toolchain_for_rebuild\(.*?(?=\n\ndef )",
        cli,
        re.DOTALL,
    )
    if not ensure_cli or "_native_toolchain_ready(progress.log)" not in ensure_cli.group():
        raise AssertionError("POSIX CLI can still download a Windows toolchain")

    activate = re.search(
        r"static void activate_toolchain_path\(void\) \{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not activate:
        raise AssertionError("Toolchain PATH activation is missing")
    posix_activate = activate.group("body").split("#endif", 1)[0]
    if "#if !defined(_WIN32)" not in posix_activate or "return;" not in posix_activate:
        raise AssertionError("POSIX setup lets a toolchain pack shadow native tools")
    if "pack_is_for_this_host(pack_root)" not in activate.group("body"):
        raise AssertionError("POSIX setup can activate a cached Windows toolchain")

    find_python = re.search(
        r"static int find_python\(char\* out, size_t cap\) \{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not find_python:
        raise AssertionError("Python resolver is missing")
    posix_python = find_python.group("body").split("#if defined(_WIN32)", 1)[0]
    if "find_toolchain_python(" in posix_python:
        raise AssertionError("POSIX setup can select Python from a cached Windows pack")

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

    print("POSIX setup and CLI native-toolchain source guard: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
