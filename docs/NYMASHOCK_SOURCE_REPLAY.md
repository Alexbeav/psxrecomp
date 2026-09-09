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
