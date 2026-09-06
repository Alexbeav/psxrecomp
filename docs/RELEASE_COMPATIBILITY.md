# Setup package compatibility

The SDK stage reads the title's `game.toml` before accepting BIOS assets.
An explicit `runtime.openbios = false` requires the selected retail profile.
The player supplies the corresponding retail BIOS image during setup.
If the recipe omits a retail profile, the existing CLI default is SCPH1001.
OpenBIOS-enabled recipes still require `OpenBIOS.toml`, `openbios.bin`, and its license.
Missing recipes, missing profiles, external profile paths, and missing required assets fail the stage.

Run `python runtime/tests/test_setup_bios_assets.py` from the framework root.
This test uses synthetic files and exercises the production asset gate.

The overlay candidate retry label carries an empty C statement before declarations.
This preserves its control flow on the release's GCC9/C11 build route.
Native platform builds remain required after a source change.

The C++ runtime uses `fntrace.h` as the only declaration of the C game-start function.
Redundant block-scope declarations fail the GCC9 link, as recorded by PSX-BUILD-024.
Run `python runtime/tests/test_fntrace_c_linkage.py` for the source guard.
The corresponding native Linux link is the decisive platform test.
