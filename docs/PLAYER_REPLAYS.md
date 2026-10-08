# Player replays

A player replay records Player 1's pad once per frame, so a later run can feed
the same input and check that the game ends in the same state. There are two
kinds. Both use the PSXRTI3 file format ([INPUT_ROUTES.md](INPUT_ROUTES.md))
with the `.psxrpl` extension.

| | F11 replay (PS1B-191) | Power-on replay (PS1B-316) |
| --- | --- | --- |
| Start | F11 during play | the launcher's **Record replay** checkbox |
| Starts from | a save state taken when you press F11 (the anchor) | vblank 0 of a cold boot, BIOS intro included |
| Ends | F11 again, or exit | exit, or F11 |
| File | a replay slot, or `replays/` after an export | `replays/<title>-boot-<UTC time>.psxrpl` |
| Plays | from the F7 menu, or `--replay FILE` | only from startup: `--replay FILE` |
| Holds guest RAM | yes, in the anchor | no |

The rest of this page covers power-on replays, which are the input for
automated tests.

## Record a session

1. In the launcher, tick **Record replay** (bottom left, next to "Skip launcher
   on boot").
2. Press **PLAY** and play normally.
3. Quit the game (close the window or use the game's quit). The replay is
   written when the game exits.

The file goes to `replays/` inside the save folder: the folder that holds the
memory cards, which is `saves/` next to the exe in a release install. The name
is the game title, `-boot-`, and the UTC start time, for example
`Tekken_3-boot-20261001T123456Z.psxrpl`. An existing file is never overwritten:
a taken name gets `-2`, `-3` and so on.

The checkbox is **not saved** in `settings.toml`. It applies to the next boot of
the current launch only. A recording runs overlays interpreted and writes a
file every session, so a tick left on by mistake must not slow down and fill
every later session.

While a power-on replay records, the game window shows the blinking REC
indicator. Save states and rewind are off. F11 stops the recording early and
saves it.

### Crashes

Every 1,800 frames (30 seconds at 60 Hz) the runtime rewrites a complete replay
of everything so far to `<name>.partial.psxrpl`. If the game crashes, that
partial copy is left behind and plays like any other replay, up to its last
update. A clean exit writes the full replay and deletes the partial copy.

## Play it headless

A power-on replay starts at vblank 0, so it plays only when the process starts
with it. Use the same pin and the same disc image file and BIOS image. The exe
may be the same pin's build for another platform (see "Another platform"
below).

```powershell
$env:PSX_REPLAY_EXIT_AT_END = '1'
& .\Game.exe --headless --memcard-dir C:\scratch\replay-run --replay .\saves\replays\Game-boot-20261001T123456Z.psxrpl --replay-verdict C:\scratch\replay-run\verdict.json
```

The exit code is 0 in sync, 3 out of sync, and 4 when the replay did not play.
The verdict JSON gives the result, frames played, the first frame where the
state digests differed, and for the recording and the player: the exe SHA-256
(`recorded_exe_sha256`, `player_exe_sha256`), the platform
(`recorded_platform`, `player_platform`) and the codegen hash
(`recorded_codegen`, `player_codegen`). `cross_platform` is `true` when the
same pin played on another platform's build. `power_on` is `true` for a
power-on replay.

`--memcard-dir` keeps the run's own files (settings, disc hash cache, new
replays) out of the install. The replay brings its own memory cards, so the
player's card files are not read or written during playback.

To record headless (tests only), set `PSX_REPLAY_RECORD_BOOT=1`. It does what
the checkbox does. `PSX_REPLAY_RECORD_FRAMES=N` stops after N frames, and
`PSX_REPLAY_TEST_INPUT_SEED` drives Player 1 with scripted presses.
`runtime/tests/replay_determinism/run.py --power-on` runs the full check.

### Compare two builds

A power-on replay has no save state in it, so it does not depend on the save
state format. Record once, then play the same file on build A and build B and
compare the two verdicts. Two recordings of the same scripted input on one
build are byte-identical apart from the name entry
(`run.py` `same_replay`), which is the A/B "no behaviour change" check.

### Another platform

A replay recorded on the Windows build plays on the Linux and macOS builds of
the same pin, and the other way round. The exe differs, so the player says
"Replay from the same build on another platform (windows-x64)" and the verdict
has `cross_platform: true`; the result still comes from the state digests.

What has to match:

- The **disc image file**. The replay holds a SHA-256 of the file, so every
  machine must use the same file (the same CHD from one share, for example). A
  bin/cue of the same disc, or a re-compressed CHD, is refused.
- The **BIOS image**, by CRC-32. The file name may differ between machines.
  Only a replay or a build without a BIOS CRC falls back to the file name.
- The **boot mode** and the pin.

The codegen hash is recorded and shown in the verdict but not compared. It is
the same on every platform for one pin (Windows and Linux builds both carry
the same value), so a difference there means another build.

## What the replay contains

- Player 1's input for every frame: buttons and both sticks as the pad
  delivered them to the console.
- The identity the player checks before it starts: the framework pin, the disc
  serial and a SHA-256 of the disc image, the BIOS file name, the BIOS boot
  mode, and the product lines: the exe's SHA-256, the overlay codegen hash, the
  BIOS image's CRC-32, the renderer and the platform. A different disc, BIOS
  image or boot mode refuses to play. The BIOS is matched by CRC-32; its file
  name counts only when a CRC is missing. A different exe plays with a warning,
  unless it is the same pin's build for another platform.
- The settings that change guest timing and are not in the machine state:
  CD speed (the BIOS speed and the game's speed after boot), turbo loads,
  enabled mods, auto-skip FMV, idle skip, and both ports' connection, analog
  mode and DualShock capability. Playback switches to them and switches back
  afterwards.
- One line about the build itself, `game_entry_low_ram=kept`: see "A recording
  from an older build" below.
- Both memory cards as the console found them at power-on: which slots had a
  card, and each card's 128 KiB image.
- State digests every 60 frames and an end checkpoint (the cycle count and a
  SHA-256 of main RAM), so playback can report the first frame that differs.

### A recording from an older build

Builds before this one set the first 16 bytes of main RAM to zero when the game
started. The console does not do that: the BIOS leaves four words there and a
game can read them through a null pointer. This build leaves the bytes alone.

A replay stores hashes of main RAM, so a recording from an older build holds
the zeros. A recording says which kind it is in its settings: this build writes
the line `game_entry_low_ram=kept`, and a recording without that line comes
from a build that set the bytes to zero.

When a recording without the line plays, the state digests and the end
checkpoint read those 16 bytes as the older build held them: zero at the start
of the game, then every later store to them. The game itself still runs with
the real bytes. So the recording stays in sync when the game behaves the same,
and goes out of sync when the game reads those bytes and takes another path.
That is a real difference between the two builds, and the replay reports it.

The other direction has no such view. A power-on recording with the line,
played on an older build, goes out of sync when the game starts, because the
older build still sets the bytes to zero. That build says "Replay from a
different build: it may go out of sync" before it plays. An F11 recording whose
anchor was taken after the game started stays in sync on the older build: that
build loads the anchor's RAM and does not set the bytes to zero again.

### Memory cards

A replay from power-on depends on what is on the cards at boot: many games read
the card during their start-up. So the replay stores both card images, and
playback uses them instead of the player's cards. The guest's own saves during
playback stay in memory and are thrown away. The player can therefore record
with the cards they normally play with; blank cards are not needed.

The card images are save data. The replay holds no BIOS or disc data: the disc
and BIOS are named by hash only, and main RAM appears only as hashes.

### Multi-disc games

A replay names the disc that is in the drive when the recording starts, by its
serial and the SHA-256 of its image. It plays only while that disc is in the
drive.

- A power-on replay always names the disc the game was launched on.
- An F11 replay started after a disc change names the new disc. The same holds
  after a save state mounted its own disc. To play that replay in a later
  session, launch the game on that disc, or change to it first. On any other
  disc it is refused as "a different game or disc image".
- An F11 replay recorded after a disc change by a build older than this rule
  names the launch disc instead. Record it again.

A power-on recording does not start when a save state is loaded at boot
(`PSX_LOAD_SLOT`), because that state can mount another disc.

## What the replay does not contain

- **Player 2.** Port 2 gets no input while a replay records or plays, so its
  pad is idle for the whole session. Its connection state is recorded.
- **Mouse, neGcon and GunCon.** Replays carry pad input only. Recording refuses
  to start while either port holds one of these devices, and the game shows
  "Replay not recorded: port 1 needs a pad or the keyboard" (or port 2). Pick
  a pad or the keyboard for that port in the launcher first. PS1B-313 tracks
  peripherals in replays.
- **Disc changes.** Changing the disc from the in-game menu ends the recording
  there, and the replay is saved up to the change. A multi-disc game can be
  recorded up to its first disc swap. A save state that needs another disc
  does not load while a replay records or plays.
- **Pad type changes.** A pad keeps the type it had at boot for the whole
  recording; a Hybrid pad does not switch between digital and analog.
  Controller hotplug does not change the ports during a recording.
- **Your host settings that only change the picture or sound**, such as the
  renderer, window size or filters. The renderer is noted in the product lines.
  A game that reads video memory back can differ between OpenGL (a normal play
  session) and the software renderer (headless playback); the verdict then
  shows an `av` digest difference, which is reported but does not fail the
  replay.

## Limits

- A replay holds at most 1,000,000 frames (about 4 hours 37 minutes at 60 Hz)
  and 65,536 input changes. Constant analog stick movement can reach the input
  limit in about 18 minutes. At either limit the recording stops and is saved.
- Overlays run interpreted while a replay records or plays (the PS1B-191
  stopgap), so an overlay-heavy game can run slower.
- Netplay, a multitap, a route recording or a playing input route stop a
  recording from starting.

## Privacy

Replays are private test inputs. Never commit them to a repository. They
contain memory card saves.
