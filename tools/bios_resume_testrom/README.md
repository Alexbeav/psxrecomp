# BIOS resume-PC test ROM (T110)

A small PS-X EXE that takes interrupts while the BIOS kernel runs, then checks
that psx-runtime never publishes a resume PC it cannot dispatch. The background
and the publisher audit are in `docs/internal/T110_DISPATCH_PUBLISHERS.md`.

It runs ROM A0 routines (memset, memcpy, memcmp, rand, strlen) and
Enter/ExitCriticalSection syscalls under a root-counter-2 interrupt with an event
callback. `run.py` boots the same disc on psx-bresume and psx-beetle and passes
when:

- the timing-independent RESULT words match;
- the loop counts equal the generator's constants;
- both backends ran the interrupt callback at least 1000 times;
- psx-bresume recorded no unknown dispatch, and `publish_ring` has no entry
  that `psx_is_dispatchable` refused.

## Build and run

The disc needs `disc/license_data.dat` from a disc you own. It stays local and
is gitignored; see `../cycle_testrom/README.md` ("Boot disc") for how to extract
it.

```bash
cd tools/bios_resume_testrom
python gen_testrom.py bios_resume_testrom.exe          # EXE, .json metadata, seeds.txt
(cd disc && mkpsxiso -y bresume.xml)                   # disc/bresume.cue
../../recompiler/build/psxrecomp-game --config game.toml
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=ON \
      "-DPSXRECOMP_BIOS_STEMS=OpenBIOS;SCPH1001;SCPH5552"
cmake --build build --target psx-bresume
python run.py --runtime build/BIOS_Resume_Test_ROM.exe --beetle <psx-beetle.exe> \
      --bios ../../bios/EUR-PSX-SCPH5552.bin [--env PSX_PRECISE_SLICE=1] --report out.json
```

`game.toml` sets `bios_hle = false`: the test targets the recompiled kernel.
SCPH5500 (Japan) does not boot a disc built with SCEA license data.
