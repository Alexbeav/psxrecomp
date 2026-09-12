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
The source clock tape is required. The MDEC option uses 512 clocks per block;
the older Octoshock option keeps 474. Guest syscalls enter the installed BIOS
exception handler, including thread switches. In this source profile,
interpreted calls return their target to the dispatcher rather than retaining
native call frames across guest thread switches.

Source-profile CD register reads sample stored device state before servicing
events crossed by their width-dependent bus wait. The load still consumes its
full duration. `runtime/tests/test_cd_read_sample_order.py` checks event ordering,
byte/half/word reads, LWC2 and address aliases at O0/O2 without retail assets.

These are cold diagnostic options. The drive launcher rejects checkpoint
capture and resume; the source memory-card/DualShock profile also remains
unsupported for checkpoints. Device state encoding does not qualify TAS
continuation. Existing default model options retain their prior behavior.

The measured Bio Hazard RAM-page/clock prefix has advanced from return 463
to 3846. The controlled 5,999-return run completes with its first mismatch at
return 3847. A prior transient mismatch at return 1645 is removed by the CD
read-order correction; paired raw snapshots at 1644, 1645 and 1646 also match.
The flat-call correction removes the prior recursion failure, and Reset
during an established data read is included in this bounded comparison.
Full Bio Hazard playback remains unqualified. No release or accepted pin
changes. Logical header retry/error behavior and Reset during audio or an
unfinished data seek remain outside this bounded drive comparison.

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
