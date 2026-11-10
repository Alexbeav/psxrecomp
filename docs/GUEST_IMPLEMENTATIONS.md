# Build-selected guest implementations

`psxrecomp_select_guest_implementation()` binds native HLE replacements to
generated guest functions. Choose HLE or LLE at build time. Both must satisfy
the title's documented caller contract; HLE may remove internal instruction
work and approximate timing. Preserve normal gameplay pacing and a functioning
LLE build. No runtime selector or cross-build state conversion is required.

After creating the runtime target and its optional PGXP sibling:

```cmake
set(GEOMETRY_IMPL "HLE" CACHE STRING "HLE or LLE")
set(geometry_targets psx-runtime)
if(TARGET psx-runtime-pgxp)
    list(APPEND geometry_targets psx-runtime-pgxp)
endif()
file(GLOB geometry_shards "${CMAKE_CURRENT_SOURCE_DIR}/generated/GAME_full_*.c")
psxrecomp_select_guest_implementation(
    NAME geometry IMPLEMENTATION ${GEOMETRY_IMPL}
    TARGETS ${geometry_targets}
    GENERATED_SOURCES ${geometry_shards}
    SYMBOLS func_80000100 func_80000200
    HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/geometry_hle.c"
    REFERENCE_PREFIX geometry_lle_
    SNAPSHOT_TAG 0x47454F01)
```

The HLE source supplies the original function names/signatures. The helper
finds exactly one definition of each symbol, renames that defining shard's
original symbol (for example `geometry_lle_func_80000100`) and leaves dispatch
declarations bound to HLE. The reference bodies can be used in isolated test
builds. Product HLE does not need to execute them. LLE compiles the originals
without renaming or HLE sources. Do not edit generated code to install HLE.

All targets using the same generated source files must select the same
implementation in that CMake build. Use separate build directories for HLE
and LLE. Call the helper once per routine family, listing every runtime variant.

Each HLE family needs a stable, distinct, nonzero 32-bit `SNAPSHOT_TAG`.
Change its tag when saved private execution state becomes incompatible.
Selected family tags are XOR-composed into `PSX_SAVESTATE_IMPL_TAG`, which the
snapshot loader checks before applying state. All-LLE uses zero. Memory-card
formats are unaffected. Tags identify compatible execution boundaries rather
than transferring state between implementations.

The helper records selections in target properties, compile definitions,
startup output and crash-report build identity. The build's CMake cache
records the title's developer-facing selection. Snapshot tests cover tagged
and untagged serialization; `test_guest_implementation.py` compiles and runs
HLE/LLE dispatch for two runtime variants and verifies retained references,
combined family tags, missing symbols and duplicate definitions.

Shared GTE/device math remains available to native routines. Title-specific
records, pointer ownership, custom register dependencies, animation and packet
layouts belong in the title adapter unless several games establish the same
contract. Tomba's F4/GT3/GT4 geometry batches are the first consumer of this
helper, retaining the complete enhanced-renderer widescreen scene.
