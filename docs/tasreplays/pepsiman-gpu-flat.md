# Pepsiman flat polygon flags

CLAIM: after the sprite correction, Pepsiman reaches another source-model
coverage boundary: the flat semi-transparent quad command `0x2A`. The existing
flat polygon renderer and work estimator support its behavior, but the command
predicate omitted flat blended triangles/quads and their ignored raw-bit aliases.

DERIVED-FROM: both fresh qualification05 attempts at `ef2281ba` matched all RAM
pages and master clocks through return 1,422. They then rejected `2AFFFFFF`
at master clock `804779008`, before return 1,423. This is a missing source-model
operation, not a preceding guest RAM divergence or a hardware defect claim.

ORACLE: the exact unmodified stock BizHawk2.3 DLL used for the original route;
SHA256 `749d6dd58430d010e46ae97c628e113adad5f420c05c2ada0ad35e58191781c0`.
The original [polygon implementation](https://github.com/TASEmulators/BizHawk/blob/2.3/psx/octoshock/psx/gpu_polygon.cpp)
and [command/pixel contracts](https://github.com/TASEmulators/BizHawk/blob/2.3/psx/octoshock/psx/gpu_common.inc)
define triangle/quad and blend flags and ignore the raw-texture bit for flat
untextured polygons. The corpus C11 limited GPU scope remains applicable.

The correction admits the complete flat untextured family: `20–23` and
`28–2B`. It changes no rasterization, clipping, work formula, mask behavior,
input sequence or title code. Unsupported line commands still fail closed;
the old negative fixture now tests `0x40` rather than the newly qualified quad.

REPRODUCE: CTest `tas_gpu_flat_blend_O0` and `tas_gpu_flat_blend_O2` each make
2,817 checks. Run either executable with `--dump` from a fresh directory to
emit the 128 authored full-VRAM results. Run
`tools/tasreplays/source_flat_oracle.py --core <stock DLL> --native-dumps <directory> --output <fresh directory>`
to compare them against ordinary guest commands in a fresh stock source core
for each case. It freezes its actual driver, retains synthetic ROM/RAM and
proves that each program reaches its authored wait loop.

RESULT: the old predicate rejects the new family fixture. The new O0/O2
fixtures pass; all 128 cases at each optimization match the stock source's
entire 1 MiB VRAM. Cases exercise eight command variants, four blend modes
and four mask settings, plus packet completion and split-quad work. Existing
command/service-clock fixtures pass 14 runs at O0/O2. All94 enabled tool
CTests pass; three preexisting disabled tests remain reported. This pixel
oracle does not claim clock equivalence; full native title RAM/clock routes
and the Tekken shared-change regression remain required.

BINDS: the pixel receipt records the exact DLL/driver, every authored packet,
synthetic input hashes, traced wait-loop endpoint and source/native VRAM
hashes. Candidate build and title suite receipts bind source, generated code,
executable, movie, retail BIOS and disc separately.
