# Overlay tier witness

PS1B-238(a) records the runtime loader's execution tier, meaning native dynamic
overlay code or the interpreter. It is host evidence, not guest save-state data.
It does not select deterministic compilation checkpoints or retain a prepared
image discarded during a freeze. Those are separate PS1B-238(b) decisions.

Every final `psx_last_run_report.json` includes `overlay_tier`. The TCP command
`overlay_tier_witness` returns the same object while the process runs. A missing
or null object cannot qualify a measurement. Set `PSX_TIER_WITNESS_ID` to the
measurement manifest's route/replay identifier before launch. It accepts at most
96 ASCII letters, digits, dots, colons, underscores and hyphens. An absent,
invalid or overlong identifier is an empty string, not an invented run identity.

`source` is the full framework commit and `tree` is its Git tree. Git archives
substitute both `runtime/FRAMEWORK_PIN` and `runtime/FRAMEWORK_TREE`; the staging
tool preserves them too. A source build resolves the tree for its actual
framework pin. A build without a tree can supply `PSX_FRAMEWORK_TREE` at configure
time. Empty identity is unbound. The measurement manifest must independently
bind the source, compiled file bytes, executable hash, route/replay identity and
terminal state. Embedded labels alone do not prove that a dirty build used those
bytes. Keep diagnostic no-pin settings in that manifest too.

Replay takes a counted holder before requesting or loading its anchor. Failed
requests release only a newly acquired holder. Recording from power-on and
netplay use the same protection. A hold turns native dynamic-overlay execution
off and freezes new admission; the last release restores prior native execution
and removes only its own load protection. The independent selfcheck freeze keeps
its own requested state, including a selfcheck beginning or ending during a hold.
Nested replay/netplay holders and repeated shutdown cannot unfreeze one another.
While held, flag setters cannot remove effective protection. The existing resimulation
gate and the existing frozen prepared-image discard remain in effect.

The admission checks reject loading before callback wiring and candidate
registration. Dispatch checks reject native loader execution, including shadow
execution and continuations. Counts are gate visits, not unique files or guest
instructions. `candidates` means the total candidate table, unlike the older
`registered`/`valid_count` fields, which decrease when RAM invalidates entries.
`dlls` counts admitted DLL owners, not every speculative host library mapping.
`native_entries` counts loader activations, not retired instructions or direct
static-native edges.

`begin` and `end` describe the latest protected interval, with guest frame/cycle
and candidate/DLL/native-entry totals. An active interval's end is the live
snapshot; later unprotected growth does not replace a completed interval's end.
`intervals` and the admission/dispatch/flag counters accumulate over the process.
`violations` retains broken live invariants across intervals: bit 1 is flags,
2 candidate growth, 4 admitted DLL growth, 8 native entry growth. `first_violation`
retains the first guest frame/cycle/physical PC even after another fault. A zero
PC denotes a non-dispatch check. `overflow=1` makes the witness incomplete;
counters saturate rather than wrapping, and a saturated holder refuses another
acquisition. There is no bounded event ring to silently overwrite evidence.

A protected route requires bound identities, the intended holder before its
anchor, native flag 0, load freeze 1, zero violations/overflow and unchanged
protected totals. An ordinary unprotected route can contain native entries and
admissions; its witness describes them without claiming a freeze. Diagnostic
`PSX_REPLAY_NO_OVERLAY_PIN` and `PSX_NETPLAY_NO_OVERLAY_PIN` remain available and
produce no corresponding holder. A terminal flag snapshot alone is insufficient.

Authored controls in `runtime/tests/test_overlay_tier_witness_runtime.py` execute
the real loader with synthetic DLLs at O0/O2. They exercise nesting, prior native
off/load frozen flags, selfcheck begin/end during a hold, attempted flag changes,
frozen publication/rescan, native
dispatch, first-fault retention, post-release growth and explicit saturation.
They contain no BIOS or retail bytes and establish no title or hardware accuracy.
