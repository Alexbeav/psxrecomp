"""A data shard replay marks the RAM pages it writes (PS1B-343).

The C fixture links the real shard store (data_shards.c) and the real store
path and page bitmap (memory.c), including the PS1B-474 byte feeds. This driver
builds it at -O0 and -O2.
"""
import argparse
import tempfile
from pathlib import Path

import source_fixture_link
from source_fixture_link import build_and_run

HERE = Path(__file__).resolve().parent

# memory.c and data_shards.c compare bytes with memcmp, and Clang calls bcmp
# for a memcmp that is only tested for equality. A host without a static
# libc.a gives the link helper no list of the C library's functions, and its
# fixed list lacks these two names (see test_low_ram_game_entry.py).
source_fixture_link.ISO_C_FALLBACK |= {"memcmp", "bcmp"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as root:
        for opt in ("-O0", "-O2"):
            build_and_run(args.cc, HERE, HERE.parent, opt, Path(root),
                          ["memory.c", "data_shards.c", "crc32.c"],
                          "test_data_shard_replay_marks.c")
    print("PASS: shard word marks and byte replay/verification/MMIO/exclusions (O0/O2)")
