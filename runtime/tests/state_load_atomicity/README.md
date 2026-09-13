# Failed state loads

This fixture links the actual device readers from a Windows GCC 16.1 consumer.
It runs no retail guest code. Its linked executable still contains generated
material, so keep the output directory on private scratch storage.

The raw and compressed version-7 controls must roundtrip successfully. Each
required section is then omitted or duplicated, a late MDEC version is
invalidated, and each actual allocation site is failed. Rejected loads must
leave the serialized machine byte-identical to its pre-load state. A final
valid load must still work. The guest clock starts above zero so the initial
MDEC age is representable after restoration.

Run `run.py --runtime-build BUILD --build-target TARGET --executable-output
TARGET.exe --output PRIVATE_NEW_DIRECTORY --repeat 1` with the configured
compiler, CMake and Ninja on PATH. The runner records source and linked-input
hashes, commands, logs and the result. It does not establish retail gameplay,
save-file portability, or completeness of fields absent from version 7.

The runner and preparation approach reuse the earlier full-machine fixture
and fix from `f3efccb43`. Scheduler and extended CPU-wire changes from that
source line are excluded. The current state wire remains version 7.
