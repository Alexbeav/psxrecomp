# Failed state loads

This fixture links the actual device readers from a Windows GCC 16.1 consumer.
It runs no retail guest code. Its linked executable still contains generated
material, so keep the output directory on private scratch storage.

Raw and compressed version-10 controls must roundtrip successfully in the
normal and source-comparison profiles. The fixture omits and duplicates every
required section, truncates the stream, corrupts late semantic fields, and fails
allocation sites. It also exercises different MDEC FIFO sizes and reversed
section order. Rejected loads must leave the serialized machine byte-identical
to its pre-load state. A final valid load must still work.

Run `run.py --runtime-build BUILD --build-target TARGET --executable-output
TARGET.exe --output PRIVATE_NEW_DIRECTORY --repeat 1` with the configured
compiler, CMake and Ninja on PATH. The build must use this source checkout.
The runner records source and linked-input hashes, commands, logs and results.
This full-consumer integration check is separate from the recompiler CTest
suite: a green CTest run does not establish state-load atomicity.

The loader retains inflated sections, checks presence and content, and reserves
MDEC/bitmap storage before applying any guest state. Scheduler admission reads
the incoming RAM. Application follows dependency order regardless of wire order.
The repair retains the version-10 wire and rejects older versions.

This fixture checks represented machine state and allocation failure handling.
It does not establish retail gameplay, portability, or completeness of fields
absent from the snapshot format.
