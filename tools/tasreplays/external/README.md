# External raw random-tape generator

`source_random_tape.py` is licensed **GPL-2.0-or-later**; see [COPYING](COPYING).
The Python adaptation was made during the psxrecomp TAS research in2026.
The arithmetic and fixed reset state are adapted from `MDFN_PseudoRNG` in
[Mednafen/Octoshock, retained by BizHawk](https://github.com/TASEmulators/BizHawk/blob/519e14aa1ad7a9d6df2edc7808c5ed687dfee046/psx/octoshock/psx/psx.cpp),
exact commit `519e14aa1ad7a9d6df2edc7808c5ed687dfee046` (BizHawk2.2.2).
The original Mednafen/Octoshock source and its authors retain their notices.

This standalone command reads no movie, disc, emulator state or game state:

```text
python source_random_tape.py output.psxrng --count 65536
```

The result is a fixed raw-word sequence consumed by a format-validating native
reader. Command timing consumes words as the simulated controller requests
them; the tape contains no command schedule or desired game outcome. The
utility is distributed separately from the framework's compiled sources and
retains its own license. No PSX CPU, GPU, sound or CD controller implementation
from the GPL reference core is incorporated by this utility.

# External DualShock axis conversion

`source_dualshock_axis.py` is licensed **GPL-2.0-or-later**; see [COPYING](COPYING).
The arithmetic is adapted from `InputDevice_DualShock::UpdateInput` in
[Mednafen, retained by BizHawk](https://github.com/TASEmulators/mednafen/blob/52c06fc2cfc1f7f0c9d3a5fcbcac3216e40384ca/src/psx/input/dualshock.cpp),
exact commit `52c06fc2cfc1f7f0c9d3a5fcbcac3216e40384ca` (BizHawk2.8); commit
`382ff1b8d293c9a862497706808cbb79b2cecbfb` (BizHawk2.10) is byte-identical.
The original Mednafen source and its authors retain their notices.

This standalone command reads no movie, disc, emulator state or game state:

```text
python source_dualshock_axis.py nymashock-dualshock-axis.bin --receipt axis.json
```

It writes one fixed 65,536-byte table: index = the u16 a frontend supplies for a
stick axis, value = the byte the emulated pad reports. `nymashock28_route.py`
consumes that table as data and contains neither the arithmetic nor a copy of
it, the same separation the random tape uses. The utility is distributed
separately from the framework's compiled sources and retains its own license.
