# Failed state loads

This fixture links the actual device readers from a Windows GCC 16.1 consumer.
It runs no retail guest code. Its linked executable still contains generated
material, so keep the output directory on private scratch storage.

Raw and compressed version-10 controls must roundtrip successfully in the
normal and source-comparison profiles. The fixture omits and duplicates every
required section, makes each section one byte short, truncates the stream,
corrupts late semantic fields, and fails allocation sites. It also exercises
different MDEC FIFO sizes and reversed section order. Rejected loads must leave
the serialized machine byte-identical to its pre-load state. A final valid load
must still work.

The runtime CTest suite runs the fixture as `state_load_atomicity_default` and
`state_load_atomicity_source`. They link every `psx-runtime` object except the
front end, so they check the readers of that build, on Windows and Linux. They
are registered only for GCC with GNU ld (`--wrap` fails chosen allocations) and
a `PSXRECOMP_ALLOW_NO_BIOS` build with no BIOS backend linked; configure prints
a line when they are not. Each writes its own zero BIOS image into the build
directory, because no guest code runs.

For a title's build, run `run.py --runtime-build BUILD --build-target TARGET
--executable-output TARGET.exe --output PRIVATE_NEW_DIRECTORY --repeat 1` with
the configured compiler, CMake and Ninja on PATH. The build must use this source
checkout. The runner records source and linked-input hashes, commands, logs and
results.

The loader retains inflated sections, checks presence and content, and reserves
MDEC/bitmap storage before applying any guest state. Scheduler admission reads
the incoming RAM. Application follows dependency order regardless of wire order.
The repair retains the version-10 wire and rejects older versions.

This fixture checks represented machine state and allocation failure handling.
It does not establish retail gameplay, portability, or completeness of fields
absent from the snapshot format.
