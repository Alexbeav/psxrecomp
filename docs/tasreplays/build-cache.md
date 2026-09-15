# TAS setup build cache — design

Status: implemented 2026-09-15 (process-upgrade item 5) in `tools/tasreplays/build_cache.py`. Nothing here changes what a replay verifies.

## Why

A title `setup` (tekken3.py / pepsiman.py / biohazard.py) rebuilds everything on every call. Measured
on 2026-09-14 (Pegasus, WinLibs GCC 16.1.0, ccache on PATH, tools dir reused):

| Stage | Command | Bio Hazard | Tekken 3 (fresh native) |
|---|---|---|---|
| configure-tools + build-tools | cmake -S recompiler; cmake --build | ~82 s | ~1 s (up to date) |
| ctest | ctest --test-dir tools | ~28 s | ~27 s |
| census + generate BIOS + generate game | psxrecomp-toml/bios/game | ~3 s | ~3 s |
| configure-native | cmake -S tools/tasreplays (FetchContent extract) | ~68 s | ~68 s |
| build-native | cmake --build | ~105 s | ~208 s |

Nine setups on 2026-09-14 recomputed identical tools binaries five times and identical generated code
four times; the generated code turned out byte-identical across all of them (that is how the codegen
question was closed). The cache makes "same inputs" cost a hash and a copy, and records in the setup
receipt exactly what was reused.

## Ground rules

- Verification is untouched. Media, movie, reference and firmware hashes are checked on every setup as
  today; the Tekken codegen guard (`tekken3-codegen.json`) and the BIOS emitter fingerprint run on
  every setup, including cache hits. `run` is not involved at all.
- A cache hit is only ever taken when every input that can influence the stage's output is identical
  by content hash. Paths, mtimes and branch names are never inputs.
- Every reuse is recorded in `setup.json` (`build_cache` block: key, hit/miss, origin entry, original
  build time, source head of the producing setup). A receipt never hides that a stage was reused.
- A miss behaves exactly like today's setup, then stores its outputs. A corrupted or partial cache entry
  is a miss, never an error that blocks a build.

## Stages and keys

Keys are SHA-256 over a canonical JSON document (sorted keys, no whitespace) listing every input by
content identity. Git tree object ids (`git rev-parse HEAD:<dir>`) identify committed directories;
setup already requires a clean tree, so a tree id is a content hash of that directory.

### Stage 1 — tools (`psxrecomp-bios.exe`, `psxrecomp-game.exe`, `psxrecomp-toml.exe`, ctest)

The tools build is not only `recompiler/`: with `BUILD_TESTING=ON`, `recompiler/CMakeLists.txt` adds
`tools/tasreplays/tests`, which compiles `runtime/tests/*.c` plus `runtime/src/psx_sha256.c` and
friends, and runs the `tools/tasreplays/test_*.py` scripts. The key therefore covers all of it:

```
tools_key = H({
  "trees": {recompiler, runtime, tools/tasreplays, lib, cmake, third_party},   # git tree ids
  "cmake_args": [sorted -D flags except -DPython3_EXECUTABLE],                # generator, build type, compilers, CHD, testing
  "toolchain": {gcc --version[0], gcc -dumpmachine, g++ --version[0], cmake --version[0], ninja --version, python major.minor.micro}
})
```

Entry: `<cache>/tools/<key>/build/` (the CMake build dir) + `receipt.json` (inputs above, SHA-256 of the
three tool executables, SHA-256 of `test-tools.log`, ctest exit code, timestamp, source head).

Hit: the three executables exist and hash as the receipt says; configure/build/ctest are skipped and
the entry's logs are copied into the project as `configure-tools.log` etc. with a first line naming
the cache entry. Miss: configure, build and ctest run into the entry's `build/` exactly as today (a
failed ctest leaves no entry). An explicit `--tools-dir` keeps today's behaviour (always reconfigure,
rebuild and retest) and bypasses this stage of the cache.

### Stage 2 — generated set (BIOS C + game C)

```
generated_key = H({
  "tools": {bios_exe_sha, game_exe_sha, toml_exe_sha},
  "bios": {stem, rom_sha, seeds_sha, profile_template_blob (git blob id of bios/<stem>.toml),
           emitter_fingerprint (tools/bios_emitter_fingerprint.sh run over the path-normalized profile)},
  "game": {boot_exe_sha, game_toml_normalized_sha, seeds ("census" or the seeds file's sha)}
})
```

The fingerprint script hashes the profile bytes, which embed the project path, so the key runs it over the
normalized profile text (the real, path-bearing fingerprint is still what gets written to `<stem>.emitter.sha`).
`game.toml` and `bios.toml` embed absolute project paths (`exe =`, `seeds =`, `bios_config =`,
`out_dir =`, `rom =`); the normalized form replaces each path value by the SHA-256 of the file it names
(or the literal `out_dir`), so two projects at different paths with the same content key the same.
Generated C embeds no paths (checked 2026-09-15 on the Bio Hazard set), so the outputs are portable.

Entry: `<cache>/generated/<key>/bios/<stem>_*` (`_full.c`, `_dispatch.c`, `_skipped_functions.json`),
`<cache>/generated/<key>/game/*` (everything `psxrecomp-game` wrote), `census.toml` and `seeds.txt` when
census ran, plus `receipt.json` with a SHA-256 per file.

Hit: files are copied into `<root>/generated/` and `<project>/generated/`, re-hashed against the receipt,
then the emitter fingerprint is recomputed and written to `generated/<stem>.emitter.sha` and the Tekken
codegen guard runs, exactly as on a miss. Miss: census/generate run as today; outputs are stored.

### Stage 3 — native player

```
native_key = H({
  "generated": {relative name: sha256 for every BIOS-stem file and every <project>/generated file},
  "trees": {runtime, cmake, third_party, assets, mods, host, bios, lib},      # git tree ids
  "tas_cmake": git blob id of tools/tasreplays/CMakeLists.txt,
  "cmake_args": [sorted -D flags with -DTAS_PROJECT_DIR, -DPSXRECOMP_BIOS_PROFILE and -D_psxrt_bash removed],
  "bios_profile_normalized_sha": as in stage 2,
  "toolchain": as in stage 1
})
```

`-DTAS_PROJECT_DIR` only locates the generated files (already keyed by content); the BIOS profile is
read at configure time for the staleness warning only; the bash path is a host detail.

**One binary input is a path today.** `tools/tasreplays/CMakeLists.txt` passes
`DEFAULT_GAME_CONFIG_PATH "${TAS_PROJECT_DIR}/game.toml"`, which `runtime.cmake` compiles into the
executable as `PSX_DEFAULT_GAME_CONFIG_PATH`. It is the only absolute path in the binary (checked by
string scan of the 2026-09-14 Bio Hazard candidate) and it is inert for the harness: `run_native.py`
always passes `--game`, and `main.cpp` consults the default only when `--game` is absent. It does make
two byte-identical builds in different project directories hash differently, which is why no two
setups have ever shared a binary hash. The design changes that define to the relative `"game.toml"`
(the form `runtime.cmake` documents, resolved beside the executable). After this the native output is a
pure function of the key above, and a cache hit can be *proven* equal to a fresh build by building the
same key twice into two project directories and comparing hashes; that proof is part of the
implementation's acceptance. Binaries built after the change will not reproduce the 2026-09-14 hashes;
that was never possible across project directories anyway, and replay receipts bind the binary by hash
at run time.

Two more facts bound what "reproducible" means, both measured on 2026-09-15. First, two fresh builds of the
same content differed in exactly 8 bytes: the PE header link timestamp, the PE checksum derived from it, and
the `__TIME__` literal in `runtime/src/crash_trace.c`. Every cached build stage therefore runs with
`SOURCE_DATE_EPOCH` fixed (`build_cache.SOURCE_DATE_EPOCH`), which GCC folds into `__DATE__`/`__TIME__` and
GNU ld into the PE timestamp; the value is part of the tools and native keys. Second, `runtime.cmake` compiles
`git describe --always --dirty --tags` into the same build id (`PSX_BUILD_REV`), so two commits with identical
trees produce binaries that differ in that string alone. The native key deliberately excludes the commit (a
content-identical tree is the point of the cache); a hit across commits reuses a binary whose embedded build
id and receipt `source_head` name the producing commit, and the fresh-equals-cached proof holds per commit.

Entry: `<cache>/native/<key>/<EXE_NAME>.exe` + `receipt.json` (inputs, exe SHA-256, configure/build log
hashes, timestamp, project it was built for). Hit: the executable is copied to
`<project>/native/<EXE_NAME>.exe`, re-hashed, and the logs are copied with a provenance first line; no
CMake build directory is created (`run` only needs the executable). Miss: configure and build as
today, then store.

## Cache location, layout, atomicity

- `--build-cache <dir>` on `setup`, else `PSX_TAS_BUILD_CACHE`, else `%LOCALAPPDATA%\psxrecomp\build-cache`.
  `--no-build-cache` runs every stage fresh and stores nothing (the 2026-09-14 behaviour).
- Entries are written to `<entry>.partial-<pid>` and renamed into place; a rename that loses to a
  concurrent writer discards its own copy and uses the winner after re-verifying hashes. A partial
  directory is never read. One setup per machine at a time is still the operating rule; the atomicity
  is so a crash leaves nothing half-written.
- No automatic pruning. `build_cache.py list` and `build_cache.py prune --keep-days N` are offered for
  the operator; nothing in setup deletes.

## Receipt

`setup.json` gains:

```
"build_cache": {
  "root": "<dir>",
  "tools":     {"key": "<hex>", "hit": true,  "entry": "<path>", "built_at": "<iso>", "source_head": "<sha>"},
  "generated": {"key": "<hex>", "hit": false, "entry": "<path>", "built_at": "<iso>", "source_head": "<sha>"},
  "native":    {"key": "<hex>", "hit": false, "entry": "<path>", "built_at": "<iso>", "source_head": "<sha>"}
}
```

`source_head` in each block is the commit whose setup produced the entry, so a reused stage always
names the tree it came from even when the current setup runs on another commit with identical trees.
Everything else in `setup.json` is unchanged; `executable_sha256` is the hash of the file in the
project, whichever way it arrived.

## Acceptance for the implementation

1. All existing python tests pass; new unit tests cover key canonicalisation (path normalisation of
   game/bios TOML, flag filtering), receipt verification, partial-entry rejection, and the copy-and-verify
   paths, without retail assets.
2. On this machine: Bio Hazard `setup` twice into two fresh project directories on the same commit.
   Second setup: tools hit, generated hit, native hit; `executable_sha256` identical between the two
   receipts. Then `setup --no-build-cache` into a third directory: identical `executable_sha256`
   (fresh build equals cached build).
3. Tekken 3 `setup` after the above: tools hit (same trees), generated miss (different BIOS/game),
   native miss; codegen guard still evaluated (visible in the log).
4. A runtime-only commit invalidates the tools and native keys and leaves the generated key unchanged;
   a recompiler-only commit invalidates all three. Demonstrated by key computation on scratch commits,
   not by building.
5. A replay of the cache-built Bio Hazard binary matches the reference through the 6,000-return ladder
   tier; the Tekken 3 full route passes on its cache-built binary.

## Out of scope

Distributed or shared caches, pruning policy, and caching anything of `run`. Save-state resume is a
separate mechanism and is not on the split branches.
