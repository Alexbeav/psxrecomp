# Building psx-beetle on macOS

`psx-beetle` is the Beetle PSX (Mednafen) oracle. It speaks the same TCP debug
protocol as `psx-runtime`, so the comparison tools (`tools/cycle_compare.py`,
`tools/cycle_testrom/measure.py`, the parity and device-trace diffs) can point
at either backend.

Verified 2026-09-29 on Apple Silicon (macOS 27, Apple clang 21, Homebrew SDL3),
starting from a fresh clone of the official Beetle repository:

- psx-beetle boots psxrecomp's bundled OpenBIOS to its shell, headless;
- `ping` answers `backend=beetle`, and `guest_cycles` advances about 565,050
  cycles per frame (33.8688 MHz at 59.94 Hz is 565,045);
- ruler #2 (`tools/cycle_testrom`) reproduces the oracle values recorded in
  `internal/FAITHFUL_TIMING_PLAN.md` on all 13 loops of the 13-loop ROM
  (`icache_miss +14`) and on all 15 loops of the current ROM, where
  `icache_miss` reads +16 (see step 6).

The Linux recipe is in `beetle-linux.md`. The Beetle side of both is the same
patch set.

## Prerequisites

- Xcode Command Line Tools (clang, make, ar).
- Homebrew: `brew install cmake ninja sdl3`.
- Python 3 for the measurement tools.

## 1. Beetle checkout and hooks

Keep the checkout outside the psxrecomp tree and symlink it in. The symlink
`beetle-psx` at the psxrecomp root is gitignored.

```bash
git clone https://github.com/libretro/beetle-psx-libretro.git ~/dev/beetle-psx
cd ~/dev/beetle-psx
git checkout 5759277b      # last C++-tree base; newer upstream is plain C
PSXRECOMP=<path to psxrecomp>
for p in wtrace_hook sio_trace_hook cdcmd_trace_hook rtrace_irq_hook \
         guest_cycles_hook cdc_dma_peek_hook spu_event_hook; do
    git apply "$PSXRECOMP/docs/beetle_$p.patch"
done
```

Apply the patches in this order: each one is a diff on top of the one before.

| patch | adds | psxrecomp user |
|---|---|---|
| `beetle_wtrace_hook` | store callback, J/JAL/JR/JALR callback | `wtrace_*`, `fntrace_*`, parity ring |
| `beetle_sio_trace_hook` | `FrontIO::SetSIOTraceCallback` | `sio_trace` |
| `beetle_cdcmd_trace_hook` | CD command callback | `cdrom_cmd_dump` |
| `beetle_rtrace_irq_hook` | CPU load callback; IRQ rising-edge callback | `rtrace_*`, parity reads, device trace |
| `beetle_guest_cycles_hook` | absolute guest clock; per-instruction PC hook | `ping.guest_cycles`, `cyc_watch*` |
| `beetle_cdc_dma_peek_hook` | CD INT type, DMA completion flags, CD decode volume | device trace detail, `beetle_cdc_decode_volume` |
| `beetle_spu_event_hook` | KEYON/KEYOFF/END_STOP/END_LOOP events | `spu_events` |

`runtime/src/beetle_libretro.cpp` and `beetle_debug_server.c` link against
every hook, so a tree missing any of them fails to link. Two limits:

- `beetle_core_get_guest_cycles()` returns the clock at the last frame
  boundary. That is exact where the debug server samples it (between frames),
  but device-trace and parity stamps taken inside a frame carry the
  frame-start value. `cyc_watch` uses the exact per-instruction clock.
- `exc_ring` answers an empty ring. Its core hook was never published and no
  tool reads it.

## 2. Static library

```bash
cd ~/dev/beetle-psx
make platform=osx STATIC_LINKING=1 HAVE_LIGHTREC=0 -j"$(sysctl -n hw.ncpu)"
# With STATIC_LINKING=1 the output is an ar archive, whatever the name says.
cp mednafen_psx_libretro.dylib libmednafen_psx.a
ln -s ~/dev/beetle-psx "$PSXRECOMP/beetle-psx"
```

The build takes about 20 seconds on an M-series Mac.

## 3. psx-beetle

```bash
cd "$PSXRECOMP"
# The runtime configure needs these submodules even for psx-beetle
# (or configure with -DPSX_REWIND=OFF).
git submodule update --init lib/recomp-net lib/retcomm-rbengine
cmake -S runtime -B runtime/build-beetle -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPSX_RECOMP_UI=OFF -DPSX_DEBUG_TOOLS=ON -DPSXRECOMP_ALLOW_NO_BIOS=ON
ninja -C runtime/build-beetle psx-beetle
```

Configure must print `psx-beetle: found .../beetle-psx/libmednafen_psx.a`;
otherwise the target is skipped silently. `PSXRECOMP_ALLOW_NO_BIOS=ON` lets the
runtime configure without recompiled BIOS C. psx-beetle links no recompiled
code; drop the flag once `tools/regen_bios.sh` has run.

## 4. BIOS: OpenBIOS under a retail name

psx-beetle uses the directory of the BIOS path it is given as Beetle's system
directory. Beetle picks the BIOS from there by disc region: `scph5500.bin`
(JP), `scph5501.bin` (NA) or `scph5502.bin` (EU). A file under the right name
with an unexpected SHA-1 gets a warning and is used anyway. If none is found,
Beetle quietly boots **its own embedded OpenBIOS**, a different build (SHA-1
`bfd08f9d5c3def7b5b3662f9ec44af316f95aada` at 5759277b) from the one the native
runtime runs. So put psxrecomp's image under all three names:

```bash
mkdir -p ~/psx-oracle/bios && cd ~/psx-oracle/bios
for n in scph5500 scph5501 scph5502; do cp "$PSXRECOMP/bios/openbios.bin" $n.bin; done
# No-disc (shell) boots load <system dir>/dummy.cue and then eject it:
head -c $((2352 * 300)) /dev/zero > dummy.bin
printf 'FILE "dummy.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' > dummy.cue
```

The 2026-09-29 run used `bios/openbios.bin` with SHA-1
`95419841b5104d552b14810b1ecbe6c1358bcdf1`. Confirm Beetle loaded it: its log
prints `Obtained SHA1: 95419841...`, and a `read_ram` of `0x1FC00000`, length
`0x80000`, must hash to the same value. No retail BIOS is needed for anything
on this page.

## 5. Run

```bash
cd ~/psx-oracle            # memory cards and relative screenshot paths land here
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
    "$PSXRECOMP/runtime/build-beetle/psx-beetle" ~/psx-oracle/bios/scph5501.bin \
    --port 4382 [--disc <game>.cue]
```

The two variables make it headless: SDL gives environment hints precedence
over psx-beetle's `opengl` render hint, and the dummy video driver opens no
window. Without them psx-beetle opens a 640x480 window. Both ways it paces to
59.94 Hz (in the window, hold Tab for turbo). The default port is 4382.

Smoke test:

```bash
python3 tools/debug_client.py --port 4382 ping          # "backend": "beetle"
python3 tools/debug_client.py --port 4382 screenshot /abs/path/shell.png
```

With no disc the screenshot is the OpenBIOS shell: a rotating shaded cube
whose colours cycle.

## 6. Self-check: ruler #2

Before trusting a new oracle build, run the cycle micro-benchmark ROM on it.
The steps (ROM, boot disc without mkpsxiso, native build) are in
`tools/cycle_testrom/README.md`. Beetle gives, per loop iteration:

| baseline | alu | load | load2 | load_use | div | div_spaced | mult |
|---|---|---|---|---|---|---|---|
| 3 | 4 | 8 | 14 | 8 | 41 | 41 | 18 |

| gte_rtps | gte_nclip | gte_read_use | ld_div | mmio_timer | mmio_spu | icache_miss |
|---|---|---|---|---|---|---|
| 14 | 7 | 14 | 49 | 6 | 41 | 19 |

These are the recorded oracle values. `icache_miss` is 19 (+16) on the current
ROM and 17 (+14) on the 13-loop ROM of 2026-06-27, because the loop moved
within its cache line (see the README). If a build disagrees, fix the oracle
before comparing anything against it.

## 7. Boot anchors: arm cyc_watch from power-on

Reset, the kernel copy, the first A0/B0/C0 calls and the shell pass within the
first frames, before a TCP `cyc_watch` arm can land. Both backends therefore
read the watch from their environment at start-up:

| variable | meaning |
|---|---|
| `PSX_CYC_WATCH` | anchor PC, or `<pc>-<end>` for region mode (each hit is one pass's cycle count) |
| `PSX_CYC_WATCH_N` | hits to record (default 16) |
| `PSX_BEETLE_UNPACED=1` | psx-beetle only: skip the 59.94 Hz wall-clock pacing. Guest timing is unchanged; headless boots just finish sooner |

Read the rings with `tools/cycle_compare.py <pc> --no-arm` (or `cyc_watch_dump`
on each port). For an LLE boot on the native side, set `PSX_BIOS_HLE=0`.

Anchor keys can differ between the two sides. A BIOS copy window declared with
`dispatch_key = "rom"` keeps its ROM address as the native key; Beetle sees the
PC the code runs at. Both `bios/OpenBIOS.toml` and `bios/SCPH1001.toml` declare
the shell that way:

| anchor | Beetle | native |
|---|---|---|
| OpenBIOS shell entry | `0x80030000` | `0xBFC0A500` |
| SCPH-1001 shell entry | `0x80030000` | `0xBFC18000` |

A native watch on `0x80030000` records nothing. Kernel windows
(`dispatch_key = "ram"`) and ROM code use the same PC on both sides.

Example, OpenBIOS shell entry (the cycle-test disc from step 6 inserted):

```bash
PSX_CYC_WATCH=0x80030000 PSX_CYC_WATCH_N=1 PSX_BEETLE_UNPACED=1 \
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
    "$PSXRECOMP/runtime/build-beetle/psx-beetle" ~/psx-oracle/bios/scph5501.bin \
    --port 4382 --disc disc/cyctest.cue &
PSX_CYC_WATCH=0xBFC0A500 PSX_CYC_WATCH_N=1 PSX_BIOS_HLE=0 \
    build/Cycle_Test_ROM --no-launcher --headless --game game.toml \
    --bios ../../bios/openbios.bin --disc disc/cyctest.cue --debug-port 4600 &
python3 ../cycle_compare.py 0x80030000 --no-arm --hits 1 --native-port 4600
```

(run from `tools/cycle_testrom`). Beetle records its first hit at guest cycle
5,015,670 with `bios/openbios.bin` (SHA-1 `95419841…`). `cycle_compare.py`
warns that `anchor_phys` differs between the backends; for a ROM-keyed window
that is expected.
