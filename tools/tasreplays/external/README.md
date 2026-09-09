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
