# psyq_sigs: name the PsyQ SDK functions in a game

Mods need to find `DrawOTag`, `SetRotMatrix`, `VSync` and the other Sony
library calls in each game. This tool names them from byte signatures, so a
mod can hook a function by name through `[recompiler] mod_function_entry_funcs`
instead of by a per-game address.

## Use

```
python tools/psyq_sigs/fetch_signatures.py --dest <private dir>/psyq_sigs
python tools/psyq_sigs/psyq_sigs.py match --sigs <private dir>/psyq_sigs \
    --exe <game>/SLES_029.13 --toml psyq.toml --json psyq.json
```

`psyq.toml` holds `[[func]]` entries in the game-root `symbols.toml` format
(`status = "psyq"`, `emit = false`); merge it and run `sync_symbols.py`.
`psyq.json` is the report: counts per library, which SDK versions each library
matched, and why signatures were rejected. Overlay files are raw: pass
`--load-address`.

## How names are given

Four levels, each only where the earlier ones left bytes free:

1. `psyq-obj`: a whole library object matches exactly once (wildcards over
   relocated bytes). Every function label in the object is named.
2. `psyq-fn`: for objects that did not match whole, each function's slice of
   the object signature, at least 32 concrete bytes, matching once.
3. `psyq-near`: short objects (a `ctc2` and a return, say) match in many
   places. One is accepted only flush against an object already placed,
   within 16 bytes, because the linker packs a library's objects together.
4. `gen`: our own signatures, written by `gen` from a game whose functions
   are already named:
   `psyq_sigs.py gen --exe SLUS_005.94 --names psyq.json --out mgs.gen.json`,
   then `match ... --gen-sigs mgs.gen.json`. `gen` masks jump targets, `lui`
   immediates and the low halves that pair with them.

Ambiguous matches and overlaps with a longer match are rejected. A
`status = "psyq"` name is a signature match, not a hand check.

## The signature set

The public set is lab313ru/psx_psyq_signatures, pinned by commit, size and
SHA-256 in `SOURCE.toml`. Its repository has no licence file, so the set is
fetched into a private folder and never committed here. Our `gen` files are
derived from game executables and stay private too.

## Results on the first testbeds (2026-11-28)

| Game | Named | obj | fn | near | gen gap fills |
|---|---|---|---|---|---|
| Alien Resurrection (SLES-02913, main EXE) | 184 | 118 | 54 | 12 | 0 |
| Metal Gear Solid (SLUS-00594, main EXE) | 317 | 308 | 4 | 5 | 0 |

Signatures generated from each game and applied to the other hit 70 and 71
functions, all already named by the public set. Alien Resurrection keeps most
of its game code, and its GTE calls, in overlays.

Tests: `python tools/tests/test_psyq_sigs.py` (synthetic words only).
