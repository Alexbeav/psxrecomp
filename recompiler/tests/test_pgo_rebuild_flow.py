"""No real build, game launch, cache cleanup, or profile mutation."""
import argparse
import contextlib
import io
from pathlib import Path
import sys
from unittest import mock

root = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(root), str(root / "tools")]
import psxrecomp_cli as cli
import compile_overlays as overlays

# Existing source paths satisfy the file guards; ALL consumers of their
# contents and all mutating/build/process operations are mocked below.
def check_flow(fail_training=False):
    args = argparse.Namespace(config=str(root / "CLAUDE.md"), project_root=str(root),
        build_dir="review-not-created", target="psx-runtime", exe_basename="psx-runtime",
        no_pgo=True, force_pgo=False, disc=str(root / "CLAUDE.md"), cmake_extra=[],
        train_secs=100, train_runs=1)
    progress = mock.Mock()
    events = []
    def configure(*a, **kw):
        flags = [e for e in kw["extra"] if e.startswith(("-DPSX_DEBUG_TOOLS=", "-DPSX_RUNTIME_IPO="))]
        events.append(("configure", kw["pgo"], *flags))
    def train(*a, **kw):
        events.append(("train", kw["train_secs"], kw["train_runs"]))
        if fail_training:
            raise RuntimeError("synthetic training failure")
    with contextlib.ExitStack() as stack:
        replacements = dict(load_sections=lambda _: {},
            activate_embedded_toolchain=lambda *a: True,
            clamp_future_mtimes=lambda *a, **kw: 0,
            product_lto_enabled=lambda *a: True,
            pgo_merge_tool_available=lambda *a: "llvm-profdata",
            _assert_configured=lambda *a, **kw: None,
            stage_overlay_toolchain_for_product=lambda *a: None,
            _cmake_configure=configure,
            _cmake_build=lambda *a: events.append(("build",)),
            run_pgo_train=train,
            _resolve_runtime_exe=lambda *a: (root / "fake-runtime.exe", None))
        for name, replacement in replacements.items():
            stack.enter_context(mock.patch.object(cli, name, replacement))
        result = cli.cmd_pgo_train(args, progress)
    expected = [("configure", "generate", "-DPSX_DEBUG_TOOLS=OFF", "-DPSX_RUNTIME_IPO=OFF"), ("build",), ("train", 100, 1)]
    assert result == cli.EXIT_OK
    kw = progress.result.call_args.kwargs
    assert kw["ok"] is True
    if not fail_training:
        expected += [("configure", "use", "-DPSX_DEBUG_TOOLS=OFF", "-DPSX_RUNTIME_IPO=ON"), ("build",)]
        assert kw["pgo"] is True and kw["pgo_skipped"] is None
    else:
        # daa78107f: a failed PGO route rebuilds the plain product instead of erroring
        expected += [("configure", "", "-DPSX_DEBUG_TOOLS=OFF", "-DPSX_RUNTIME_IPO=ON"), ("build",)]
        assert kw["pgo"] is False and "synthetic training failure" in kw["pgo_skipped"]
        progress.error.assert_not_called()
    assert events == expected, events
    print("PGO flow", "failure" if fail_training else "success", events)

check_flow()
check_flow(True)
with mock.patch.object(overlays, "_toolchain_env", return_value=({}, "fake-toolchain")), \
     mock.patch.object(overlays.subprocess, "run", return_value=mock.Mock(returncode=0)) as run, \
     contextlib.redirect_stdout(io.StringIO()):
    assert overlays._compile_dll_direct("fake.c", "fake.dll", [], gcc="fake-gcc")
command = run.call_args.args[0]
assert "-O2" in command and "-shared" in command
assert not any("profile" in arg.lower() for arg in command)
print("Overlay DLL command (mocked subprocess):", command)
print("PASS: successful PGO finishes use/IPO-ON; a failed training run falls back to the plain product; overlay DLL command has no PGO flags")
