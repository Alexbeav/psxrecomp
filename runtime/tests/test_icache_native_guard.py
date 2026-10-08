"""Control native admission without disabling cache contents or timing."""
import argparse
from pathlib import Path
import tempfile
from source_fixture_link import build_and_run

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    for name, extra, expected in (
        ("default", (), 1),
        ("on", ("PSX_NATIVE_ICACHE_GUARD=1",), 1),
        ("off", ("PSX_NATIVE_ICACHE_GUARD=0",), 0),
    ):
        with tempfile.TemporaryDirectory() as folder:
            output = build_and_run(
                args.cc, here, here.parent, "-O2", Path(folder),
                ["dirty_ram_interp.c", "psx_icache.c"],
                "test_icache_native_guard.c",
                ("PSX_ENABLE_BLOCK_CYCLES=1", "PSX_NO_DEBUG_TOOLS=1") + extra,
            )
            assert output.strip() == (
                f"native_guard_admitted={expected} cache_contents=42 miss_cycles=7 hit_cycles=0"
            ), output
            print(name + ": " + output.strip())
