# Bio Hazard independent source admission

`tools/tasreplays/nymashock_admission.py` admits the exact Japanese Director's
Cut movie after a completed stock replay and a separate complete observer
replay. It does not consume a native run as an expected result.

Both source runs must consume all 227,202 unchanged original inputs, use the
same declared neutral tail, exit cleanly, and retain a witnessed ending image.
The role, original movie, BIOS, complete stock host closure, loaded core,
reviewed read-only helper, frozen launcher/Lua and run-specific command are
verified. The closure is rechecked against the pinned release archive. Empty
sync maps are resolved through the source: one P1 DualShock, only card1 enabled,
fixed RTC, and the admitted Nymashock defaults. The initial card bytes and
isolated writable directory are part of that identity.

The stock and observer must match at every full-RAM SHA-256, accumulated
master clock and lag observation, and every required screenshot. The observer
adds 512 page hashes per completed return and one terminal raw 2 MiB RAM dump.
Page clocks must match the independent stock record; the terminal dump must
match all 512 page hashes and the independent full-RAM SHA-256. Missing rows,
gaps, invalid clocks, mismatched endings/settings or existing output reject
reference creation. Both cleanly exited source runs must also retain identical
persisted128KiB card1 files; a missing, malformed or differing card rejects
admission even when every RAM/clock row matches.

```text
python tools/tasreplays/nymashock_admission.py STOCK_RUN OBSERVER_RUN NEW_REFERENCE.json
```

The output is `biohazard-independent-source-v1`. It binds the complete
source artifacts, terminal RAM, page index, initial and persisted terminal card,
and ending evidence.
Native input, clocks, RAM, repeated playback and progression-save gates remain
separate; a completed source replay is not a native gameplay pass.

The source's fixed cold PRNG sequence is separately reproducible with
`tools/tasreplays/external/collect_nymashock_random.py`. That tool compiles
the exact generator type from clean pinned Mednafen source at O0/O2 and
compares all 65,536 raw words to the existing external generator. Its output
tape is identical, but CD range rejection, command ordering, random-word
consumption and deadlines still need independent source/native comparison.
The external generator remains separate from the generic native tape reader.

Progression saves require fresh per-run raw-card copies, in-game loads and
save/reload checks on both source and native. Emulator states are not raw
cards and cannot satisfy those gates.

`biohazard.py setup` requires that admitted reference plus the exact owned
disc, BIOS, movie and Nymashock raw-generator qualification receipt. It creates
a fresh candidate from clean source, freezes the original input and initial
card, and records generated-code/build identities. No installed old title
binary is used. `biohazard.py run` compares every declared source/native RAM
page and clock, and full playback additionally compares terminal RAM bytes and
the candidate's persisted card after clean exit. Native normal initialization
registers `memcard_flush_all` with `atexit`, so the observation-end exit follows
the existing persistence path. Terminal evidence errors produce a failed
comparison receipt; they cannot leave an apparent successful playback.
An optional diagnostic cutoff retains only an unchanged original prefix;
it cannot qualify full playback or replace the original movie.

The candidate explicitly selects available runtime comparison models, including
the separately qualified Nymashock controller/card models. Older profile names
on other devices describe implementations being compared; they do not assert
automatic compatibility with Nymashock. Native timing remains unqualified
until the independent comparison passes. Source endpoint N needs one extra
neutral native input boundary because native completion precedes its final
return observer. Full runs keep the original input count intact and record
that declared tail. The per-run storage bound is3GiB for full page/CPU checks.


## Experimental Bio Hazard drive comparison

`tools/tasreplays/biohazard.py` selects `--cd-drive-model nymashock-1.29.0`,
`--mdec-source-model nymashock-1.29.0` and `--syscall-model guest-exception`.
The drive model uses both logical-seek pipeline slots, tracks the physical
head during Pause/standby and ReadTOC, consumes the active-Pause random draw,
and separates Reset travel/header phases from its command-reply deadline.
During an established read, GetlocP reports the last physical sector's sub-Q
position, one sector ahead of the next data delivery.
The source clock tape is required. The MDEC option uses 512 clocks per block;
the older Octoshock option keeps 474. Guest syscalls enter the installed BIOS
exception handler, including thread switches. In this source profile,
interpreted calls return their target to the dispatcher rather than retaining
native call frames across guest thread switches.

Source-profile CD register reads sample stored device state before servicing
events crossed by their width-dependent bus wait. The load still consumes its
full duration. `runtime/tests/test_cd_read_sample_order.py` checks event ordering,
byte/half/word reads, LWC2 and address aliases at O0/O2 without retail assets.

These are cold diagnostic options. This integration exposes no TAS checkpoint
launcher; the source memory-card/DualShock profile also remains
unsupported for checkpoints. Device state encoding does not qualify TAS
continuation. Existing default model options retain their prior behavior.

On historical replay revision `7a0d0ff4`, the measured Bio Hazard RAM-page/clock prefix advanced from return 463
to 233,567, with the first state/clock difference at 233,568 in the neutral
ending tail. That difference is closed by timing an explicit seek from the
physical read head: an established read keeps the source's CurSector two
sectors ahead of the sector handed to the guest, so a mid-read SeekL timed
from the delivery cursor travelled two sectors too far and completed 209
cycles late, pushing the driver's following ReadS past the return boundary.
A cold full-route replay carrying that correction matches the admitted stock
reference on all 239,202 returns with no divergence, and terminal RAM and the
persisted memory card both match.
All forty-six raw RAM snapshots at 5,000-return intervals match the admitted stock
SHA-256. The flat-line and shaded-line stops previously encountered before
returns 20,372 and 20,387 are passed by this replay. These progress captures
are not resumable emulator states.
A prior transient mismatch at return 1645 is removed by the CD read-order
correction; paired raw snapshots at 1644, 1645 and 1646 also match.
The flat-call correction removes the prior recursion failure, and Reset
during an established data read is included in this bounded comparison.
GPU upload history now stops capturing when its 128 retained entries are full.
Previously later uploads kept incrementing the last entry until its signed
counter overflowed and indexed outside the history buffer. The regression
fixture exercises the production capture code at capacity and the maximum
legal upload size at O0/O2. The full replay passes the former crash at 214,410.
Full Bio Hazard playback remains unqualified. No release or accepted pin
changes. Logical header retry/error behavior and Reset during audio or an
unfinished data seek remain outside this bounded drive comparison.

The source GPU comparison path now handles flat and shaded two-vertex lines (GP0
0x40-0x47 and 0x50-0x57), including degenerate single-pixel lines, dithering, blending,
mask evaluation, clipping and interlaced row skipping. Timing charges two
dispatch clocks, sixteen setup clocks, and twice the unclipped major-axis
length; oversize lines pay only dispatch and setup. Shaded lines interpolate each endpoint color with twelve fractional bits,
including endpoint reversal and half-unit color bias. Polylines remain
unsupported. The new path has 512 authored component cases comparing
full VRAM SHA-256 and timing with extracted, unedited Nymashock line functions.
`tools/tasreplays/source_line_oracle.py` records source and harness identities;
`runtime/tests/test_source_gpu_line_contracts.py` checks the retained fixtures
at O0/O2. This is component evidence, not an unmodified-core replay pass.

The source sprite path also admits raw semi-transparent rectangles (GP0 0x67),
using the existing raw texture and blend implementation. Like 0x66, this is a
four-word packet with three-word FIFO feedback; raw colour bypasses modulation
and leaves sprite timing unchanged. The sprite fixture checks both opcodes at
O0/O2. The stock Octoshock2.3 pixel oracle compares 192 full VRAM images across
texture depths, blend modes, mask modes and command colours. Nymashock
`gpu_sprite.cpp` and `SPR_HELPER_SUB` select the same raw/blend semantics.
The 150,000-return replay passes the former 0x67 stop at return 122,977.
The launcher and RAM probe accept up to 64 selected raw snapshots, enough
for the full Biohazard route at 5,000-return intervals plus terminal diagnostics.
The observer tests cover the 64-snapshot boundary and reject a 65th entry.

Source basis: BizHawk 2.9.1's Mednafen 1.29.0 `psx/cdc.cpp` (`HandlePlayRead`,
`CalcSeekTime`, `Command_SeekL`, `Command_Pause`, `Command_Reset`,
`Command_ReadTOC`), `psx/mdec.cpp` (`WriteImageData`, `MDEC_Run`), and guest
CPU exception entry. Passive observers reading the exact stock Waterbox
image reproduced all 6,000 reference return RAM hashes, clocks and lag counts
before their CD/CPU state was used to diagnose divergences. Raw CPU register
snapshots are diagnostic data, not a claim of CPU-register equality.

Source admission binds the exact bytes of its admission tool. The repository
keeps that Python file in LF form through .gitattributes so Windows checkout
conversion does not invalidate an otherwise unchanged reference.
