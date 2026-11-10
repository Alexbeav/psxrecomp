# Player replay recording

The launcher checkbox records one session from power-on. It keeps the choice
for this process, without saving it to settings.toml. Recordings go under the
game's writable save root in replays/. The default replay hotkey is F12;
config.ini [KeyMap] ReplayRecord overrides it, including an explicit None.
The hotkey starts an anchored recording during play, or stops a recording.
Anchors use the modern save-state safety, mod scope and rollback checks and
require the HLE thread scheduler. This is separate from the choice of BIOS
implementation. Unsafe anchors wait for a safe boundary rather than exporting
an incomplete state.

Queued save/load and replay-anchor requests also wait while guest time is
temporarily frozen by a host scope. They retain their payload until the scope
ends; provider readiness and the existing host-call/resume-PC checks still
apply. This is not the ordinary UI pause flag and does not change trusted raw
transaction snapshots.

The recorded format is PSXRTI3 with player-replay metadata. It contains P1 pad
buttons/sticks, host timing settings, disc/BIOS/build identity, state digests
and an endpoint RAM/cycle check. A power-on recording also contains the two
raw memory-card images and their insertion mask. An anchored recording holds
a saved machine state. A complete prefix of a long power-on recording is
saved periodically; a clean stop removes that partial copy.

This format records P1 only. P2 remains neutral during recording/playback;
normal multiplayer sampling resumes afterwards. Mouse, neGcon and GunCon
remain available as live devices but are not recorded. Either such port, a
multitap or an active netplay/self-check session refuses recording. Active
extended-card codecs or opaque card tails also refuse raw-card replay, so the
format cannot silently discard their data. Playback uses RAM-only cards and
restores the player's original bindings; it never flushes replay writes to
the player's card files.

Native overlay execution is held off while a replay owns the interpretation
tier, with counted ownership that retains the independent self-check freeze.
Save-state menu requests and the keyboard rewind hotkey are refused while
recording/playback is active.
A new frontend session clears the consumed start choice and frame counter;
shutdown finishes from the last complete boundary before device teardown.

For scripted player tests, --replay FILE starts playback and
--replay-verdict FILE writes its JSON result. --replay-fast changes host pacing
while playing. PSX_REPLAY_RECORD_BOOT=1 requests the launch checkbox's behavior;
PSX_REPLAY_RECORD_FRAMES limits a scripted recording. PSX_REPLAY_EXIT_AT_END=1
exits after saving or playing: zero means saved/in-sync, three diverged, four
not played/taken over and five not recorded. An explicit playback request wins
over the recording checkbox. PSX_REPLAY_RECORD_FILE and PSX_REPLAY_RECORD_AT
request an anchored recording at a specific vblank.

The JSON verdict distinguishes in-sync, diverged, taken over and failed runs.
Old build/settings differences are reported. Recordings made by the old host
that cleared RAM 0..15 at game entry use a separate comparison view; guest RAM
and guest stores remain unchanged. Older dropped-store semantics are reported
as a settings difference, not emulated. Matching one's own recording
checks this recorder and host wiring. It does not prove console accuracy or
replace the programme's oracle-based replay qualification.

The Windows consumer exercise uses a verified local Ape Escape disc and retail
BIOS, and checks 600-frame power-on and frame-120 anchored recording/playback. Focused
fixtures also cover the recorder engine, anchor success/failure rollback,
RAM-card isolation, extended-card preservation, session reset and keybindings.
The generic scripted anchored path can leave product identity metadata empty;
its test receipts retain the executable hash separately. GUI return/rematch,
physical pointer/wheel gameplay, arbitrary anchors, other operating systems
and long recordings have not been qualified here.
The existing legacy TAS route transport is separate from this player recorder.
