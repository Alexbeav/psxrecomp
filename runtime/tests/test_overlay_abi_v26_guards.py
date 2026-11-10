#!/usr/bin/env python3
"""Source guards for composed ABI v37 (upstream v25 prefix plus modern slots).

test_overlay_abi_layout.c pins the struct against a copy of upstream's header.
That is only worth something if the copy really is upstream's file, and if the
code on both sides of each changed slot agrees with the slot's type. This
checks those facts from the sources, so it needs no build.
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# runtime/include/overlay_api.h at RetroPortingToolKit/psxrecomp 3505f2a01.
UPSTREAM_COMMIT = "3505f2a01"
UPSTREAM_BLOB = "676510362e8da4bf00c94a9cbf6c122f456bb810"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def git_blob_id(data: bytes) -> str:
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def define(text: str, name: str) -> int:
    match = re.search(rf"^#define\s+{name}\s+(\d+)\s*$", text, flags=re.MULTILINE)
    assert match, f"{name} not defined"
    return int(match.group(1))


def main() -> None:
    # 1. The fixture is upstream's header, byte for byte. A checkout that
    #    converts line endings must not fail this, so CR is ignored.
    fixture = ROOT / "runtime/tests/fixtures/overlay_api_upstream_v25.h"
    blob = git_blob_id(fixture.read_bytes().replace(b"\r", b""))
    assert blob == UPSTREAM_BLOB, (
        f"{fixture.name} is not upstream's overlay_api.h at {UPSTREAM_COMMIT}: "
        f"blob {blob}, expected {UPSTREAM_BLOB}")
    upstream = fixture.read_text(encoding="utf-8")
    layout_test = read("runtime/tests/test_overlay_abi_layout.c")
    assert UPSTREAM_COMMIT in layout_test and UPSTREAM_BLOB[:12] in layout_test
    assert define(upstream, "PSX_OVERLAY_ABI_VERSION") == 25
    assert define(upstream, "PSX_OVERLAY_CODEGEN_VER") == 13

    # 2. The composed ABI and codegen number remain distinct from upstream, which does
    #    not use, so every cache folder a G2 build wrote (cg12) is left behind.
    api = read("runtime/include/overlay_api.h")
    assert define(api, "PSX_OVERLAY_ABI_VERSION") == 37
    assert define(api, "PSX_OVERLAY_CODEGEN_VER") == 19

    # 3. Upstream's struct text is in this tree's header unchanged, and the
    #    modern tail follows its last member, then observers and source extension.
    def struct_body(text: str) -> str:
        start = text.index("typedef struct OverlayCallbacks {")
        return text[start:text.index("} OverlayCallbacks;", start)]
    def members(text: str) -> list[str]:
        body = re.sub(r"/\*.*?\*/", "", struct_body(text), flags=re.S)
        return [a or b for a, b in re.findall(r"\(\*\s*(\w+)\s*\)|\*\s*(\w+)\s*;", body)]
    ours, theirs = members(api), members(upstream)
    theirs = ["cpu_charge" if name == "advance_cycles" else name for name in theirs]
    assert len(theirs) == 47 and ours[:47] == theirs, (ours[:47], theirs)
    assert ours[47:] == ["ws_masked_reject", "ws_nclip_branch", "ws_cull_scale",
        "nclip_exact_sign", "mod_instruction", "mod_memory_address",
        "mod_immediate_select", "mod_try_function_replacement",
        "cpu_step_boundary_enabled", "cpu_step_boundary", "source_bus_boundary",
        "psx_game_break", "source_latch_bltz"], ours[47:]
    assert "#ifdef PSX_SOURCE_BUS_BOUNDARY" in api
    assert "PSX_OVERLAY_FLAVOR_SOURCE_BUS 4" in api

    # 4. v24, last_store_pc: the host hands out its own word, the shim keeps a
    #    private one only as the pre-init / NULL-host fallback, and DLL builds
    #    write through the pointer.
    loader = read("runtime/src/overlay_loader.c")
    assert "s_callbacks.last_store_pc = &g_debug_last_store_pc;" in loader
    shim = read("runtime/include/overlay_dispatch_preamble.c.inc")
    assert "uint32_t *g_psx_last_store_pc_p = &s_last_store_pc_local;" in shim
    assert re.search(r"g_psx_last_store_pc_p = g_cbs\.last_store_pc \? "
                     r"g_cbs\.last_store_pc\s*:\s*&s_last_store_pc_local;", shim)
    assert not re.search(r"^uint32_t g_debug_last_store_pc;", shim,
                         flags=re.MULTILINE), "the shim defines a private copy"
    cpu_state = read("runtime/include/cpu_state.h")
    assert "#define g_debug_last_store_pc (*g_psx_last_store_pc_p)" in cpu_state

    # 5. v25, mod_function_entry returns int everywhere. The emitter writes
    #    the filter-result form; this tree's host has no function filters, so
    #    its one implementation returns 0 on every path.
    generator = read("recompiler/src/code_generator.cpp")
    assert '"if (psx_mod_function_entry(cpu, 0x{:08X}u)) return;"' in generator
    assert "extern int psx_mod_function_entry(CPUState* cpu, uint32_t address);" in generator
    assert "void psx_mod_function_entry" not in generator
    assert re.search(r"^int psx_mod_function_entry\(CPUState \*cpu, uint32_t address\) \{\n"
                     r"    overlay_flush_cycles\(\);\n"
                     r"    return g_cbs\.mod_function_entry \? "
                     r"g_cbs\.mod_function_entry\(cpu, address\) : 0;\n\}",
                     shim, flags=re.MULTILINE)
    assert "int psx_mod_function_entry(struct CPUState* cpu, uint32_t address);" \
        in read("runtime/include/mod_plugins.h")
    mods = read("runtime/src/mod_runtime.cpp")
    start = mods.index('extern "C" int psx_mod_function_entry(')
    body = mods[start:mods.index("\n}\n", start)]
    returns = re.findall(r"\breturn\b([^;]*);", body)
    assert returns and all(value.strip() for value in returns), returns
    # Modern function filters may transfer control; the preserved contract is int, not void.
    # Tracked C and C++ sources only. An in-tree build (runtime/build*) puts a
    # ".cmake" folder and generated sources under runtime/; neither is ours.
    scanned = 0
    for path in ROOT.glob("runtime/**/*"):
        parts = path.relative_to(ROOT).parts
        if (path.suffix not in (".c", ".cpp", ".cc", ".inc", ".in") or
                any(part == "CMakeFiles" or part.startswith("build") or
                    part.startswith(".") for part in parts[:-1]) or
                not path.is_file()):
            continue
        scanned += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        assert not re.search(r"\bvoid\s+psx_mod_function_entry\s*\(", text), (
            f"{path.relative_to(ROOT)} still declares the pre-v25 void form")
    assert scanned > 100, f"only {scanned} runtime sources scanned"

    # Source-flavor extension has a distinct tag and the real shared owner.
    assert "s_callbacks.source_bus_boundary = source_gpu_runtime_cpu_boundary;" in loader
    assert "g_cbs.source_bus_boundary(cpu,pc);" in shim
    assert "source overlay requires CPU boundary callback" in shim
    assert "s_callbacks.cpu_step_boundary_enabled = psx_cpu_step_boundary_enabled;" in loader
    assert "s_callbacks.cpu_step_boundary = psx_cpu_step_boundary_fn;" in loader
    assert "g_cbs.cpu_step_boundary_enabled(0) && g_cbs.cpu_step_boundary" in shim
    assert "g_cbs.cpu_step_boundary(cpu, addr);" in shim
    print("overlay composed ABI guards: PASS (fixture blob %s, ABI37, codegen19)"
          % UPSTREAM_BLOB[:12])


if __name__ == "__main__":
    main()
