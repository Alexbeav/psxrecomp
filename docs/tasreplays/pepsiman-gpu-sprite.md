# Pepsiman textured rectangle admission

CLAIM: the limited source-comparison GPU path rejected Pepsiman's GP0 `0x66`
textured semi-transparent rectangle. This is missing source-model coverage,
not evidence of a physical PS1 defect. The correction admits that packet to
the existing sprite timing and software pixel kernels.

DERIVED-FROM: two fresh native attempts at framework `ef6c888d` matched all
512 RAM pages and master clocks for 1,420 completed source returns. Both then
stopped before return 1,421 on word `66808080` at master clock `803618048`.
The projection's packet and feedback predicates and the source software
renderer excluded `0x66`; its existing blend/mask work and pixel calculations
already express the operation. Original movie and device settings are retained.

ORACLE: unmodified BizHawk 2.3 `octoshock.dll`, SHA256
`749d6dd58430d010e46ae97c628e113adad5f420c05c2ada0ad35e58191781c0`.
Primary source at tag 2.3:
[gpu_sprite.cpp](https://github.com/TASEmulators/BizHawk/blob/2.3/psx/octoshock/psx/gpu_sprite.cpp)
and [gpu_common.inc](https://github.com/TASEmulators/BizHawk/blob/2.3/psx/octoshock/psx/gpu_common.inc).
The sprite table specifies four packet words and three feedback words. Its
dispatch and rectangle work retain setup, clipping, aligned blend/mask costs,
CLUT loads and texture-cache misses. The corpus's C11 GPU coverage notes already
classify this model as limited source compatibility.

REPRODUCE: configure/build the recompiler with testing enabled and run CTest's
`tas_gpu_sprite_blend_O0` and `tas_gpu_sprite_blend_O2`. Each fixture makes
786 checks, including incomplete-packet/debt boundaries and 96 pixel cases.
Run either test executable with `--dump` in a fresh directory to emit full
authored VRAM snapshots. Then run `tools/tasreplays/source_sprite_oracle.py`
with `--core <stock2.3 DLL> --native-dumps <directory> --output <fresh directory>`.
It creates a fresh stock core for each case, sends ordinary guest GPU writes,
verifies entry into the authored wait loop, and compares the entire 1 MiB VRAM.
Synthetic ROM/RAM are fixture outputs, never retail playback inputs.

RESULT: the old production predicate fails the new packet-length regression.
The correction passes all 786 checks at O0 and O2. All 96 full-VRAM cases at
each optimization match stock for three texture formats, four blend modes,
four mask settings, neutral and non-neutral modulation, transparent texels,
and opaque texels inside a blended command. Cache miss/hit work is checked by
the native fixture. The stock pixel oracle makes no clock claim; full title
RAM/clock comparison and Tekken regression remain separate required gates.

BINDS: the source pixel receipt records the stock DLL, driver, all authored
packet words, synthetic ROM/RAM hashes, wait-loop observation, and both full
VRAM hashes. The title build and suite receipts separately bind the exact
framework, generated code, executable, unchanged movie, disc and BIOS.
