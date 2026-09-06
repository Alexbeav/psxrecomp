# Release pipeline candidate

This child starts from the exact private Wave 3 source `63446a28111a21a0cb7e8b5106c3614c898fc4b5`.
It does not replace the accepted campaign or shared development pin.

The setup host and Python CLI now use native Unix tools and ignore the Windows portable cache on Unix.
These are the previously tested fixes from `a4a5427e`, `153f5442`, `258c2cbb`, and `00ef8d30`.
The backport preserves newer Windows resource-compiler and source-path fixes.
Windows compilation of the setup host before and after the change produces identical object bytes:
`A9F8379905ED1DD38F6E095BD4454898D60E9268E684385EA612EC3108A945B7`.
The POSIX source also passes native Linux compilation checks.

The package wrapper includes the project manifest and public Mods catalog.
Optional launcher assets enter the package only when present.
The packager removes UI test data before and after SDK staging.
It preserves the existing private-path and retail-payload guards.

The [CI guide](ci/README.md) describes the corrected build-only template.
Its four native jobs audit exact ZIPs before upload.
The macOS jobs use one deployment target for the host and generators and select pinned static SDL.
The new archive gate rejects the old Homebrew dependency and macOS 15 generator floor.
Native Mac compilation and execution remain pending.
The first Xena canary exposed a Bash 3.2 empty-array failure in SDL extraction.
The shared template now uses its pinned inline extraction path.
It also records the explicit owned Git paths for later Linux container audit steps.
The source-specific fixes require a repeat native canary before acceptance.
The Windows control also exposed checkout line-ending conversion in all four recipe files.
The template preserves committed bytes, and the source gate rejects converted or edited recipes.

No runtime device, CPU, rendering, or recompiler implementation changes in this candidate.
The code-selection tests cover Linux, macOS, missing native tools, and the unchanged Windows portable route.
The source, package, and platform-copy tests pass.
This candidate is local and unpushed.
