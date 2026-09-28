# MDEC decode core

The scalar core in `runtime/src/mdec.c` decodes guest-supplied quantization,
scale and run-length data. It uses one transform for every block. There is no
DC-only or SIMD shortcut to qualify separately.

## Derivation

The interface follows PSX-SPX's **MDEC Data Format**, **MDEC Decompression**,
**set_scale_table**, **real_idct_core**, **decode_colored_macroblock** and
**y_to_mono** sections:
<https://psx-spx.consoledev.net/macroblockdecodermdec/>.
Authored input programs and observed output bytes constrain the integer
arithmetic. The implementation was written from these observations and an
approved interface skeleton, without reading a reference decoder or the
replaced decode bodies. This is a model matching the retained observations;
the tests do not establish unique hardware internals.

`floor_div` rounds toward negative infinity. Coefficients retain four fractional
bits until the first transform pass. For DC, let `p = level * quant[0]`;
the stored value is `16*p - 8*sign(p)`. For AC with nonzero quantization scale,
let `p = level * quant[index] * q`; the value is
`16*floor(p/8) - 8*sign(p)`. For AC with zero scale it is `32*level`.
Zero has sign zero. Values saturate to [-16384, 16383]. Both scale modes use
the documented zigzag placement. These observed rounding and placement rules
take precedence over a literal translation of the documentation's pseudocode.

Both transform passes multiply by `floor(guest_scale/8)` and produce
`floor((sum + 16384)/32768)`, transposing the index layout between passes.
The final samples wrap to signed nine bits and clamp to signed eight bits.
The largest eight-term sum, including the rounding bias, fits signed 32 bits.

Color conversion uses red `floor((359*Cr+128)/256)`, blue
`floor((454*Cb+128)/256)` and green
`floor((floor(-88*Cb/32)+floor(-183*Cr/32)+4)/8)`, added to luma.
Each result wraps and clamps before adding the unsigned offset.
RGB555 rounds unsigned channels by adding four before division by eight,
then saturates to 31. Four-bit mono adds eight before division by sixteen,
then saturates to 15. Signed output toggles the final channel sign bits;
RGB555's bit 15 comes from the command. The 24-bit order is R, G, B.

## Regression coverage

`runtime/tests/data/mdec_clean/manifest.json` binds the retained authored inputs
and observed outputs by SHA-256. There is no BIOS, retail stream, generated
retail code or decoder source in this data.

| Set | Rows | Purpose |
| --- | ---: | --- |
| M1–M5 | 54 | All output depths, signs, color, saturation, supplied tables and stream edges |
| M10 boundaries | 185 | 84 single-entry and 32 group-b residuals of a rejected integer model, plus zero and sign controls |
| M11 controls | 432 | Summing-column, single-coefficient and explicit-zero controls around pass-rounding boundaries |

Every row is checked through the public command/DMA FIFO and through the
production source decode/table callback seam. The second adapter assembles
callback blocks into raster order. It does not exercise the source scheduler
or establish pipeline timing. Expected bytes come from observations, never
from a second copy of the implementation.

After configuring the runtime with its normal build prerequisites:

```sh
cmake --build build --target mdec_clean_fixture_probe mdec_output_lifetime_test
ctest --test-dir build -R 'mdec_clean_fixtures_test|mdec_output_lifetime_test' --output-on-failure
```

A standalone compiler also suffices (use the platform's shared-library suffix):

```sh
gcc -std=c11 -O2 -shared -fPIC -I runtime/include runtime/tests/mdec_clean_fixture_probe.c -o probe.so
python runtime/tests/test_mdec_clean_fixtures.py --library ./probe.so
```

The replay accepts additional TSV paths and an optional `--receipt result.json`.
It rejects an empty input set. With no paths supplied, it verifies the retained
fixture hashes before running 1,342 entry checks. Both O0 and O2 pass.
Removing the half-unit bias from a scratch copy of the new core causes 684
of these checks to fail; the production core passes without that mutation.

## Integration requirements

The scale table now stores raw signed guest halfwords. Snapshot field sizes
are unchanged, but the saved meaning is different. The integrator must bump
the boot-state format version and rebuild resume cohorts before landing.
Old snapshots must not be interpreted in this representation.

The existing output-lifetime test and timing fixture pass. The inherited
source pipeline and DMA contract runners fail at the first AC/color cases
at O0 and O2. Their golden values were not read or edited by this worker;
independent review must resolve those failures before integration. Full
runtime route, retail, performance and release gates remain separate.
