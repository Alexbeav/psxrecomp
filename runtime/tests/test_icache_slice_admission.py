"""Execute production cache/slice admission against authored RAM only."""
import argparse
from pathlib import Path
import tempfile
from source_fixture_link import build_and_run

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as folder:
        for opt in ("-O0", "-O2"):
            output = build_and_run(args.cc, here, here.parent, opt, Path(folder),
                          ["dirty_ram_interp.c", "psx_icache.c"],
                          "test_icache_slice_admission.c",
                          ("PSX_ENABLE_BLOCK_CYCLES=1", "PSX_NO_DEBUG_TOOLS=1"))
            print(opt + ": " + output.strip())
            build_and_run(args.cc, here, here.parent, opt, Path(folder),
                          ["netplay_state_digest.c", "crc32.c"],
                          "test_icache_digest.c")
    print("PASS: cache nesting, BIOS word range and replay digest versions (O0/O2)")
