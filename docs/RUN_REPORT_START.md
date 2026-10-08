# Why a start ended before the first frame, in the run report

Every run writes `psx_last_run_report.json` when it exits. A start that ends
before the game runs has `frame` 0. Three fields say why it ended.

```json
"exit_origin": "start_refused",
"start_refused": {"kind": "no_bios", "title": "BIOS Mismatch",
                  "message": "That BIOS (524288 bytes, CRC32 318178BF) is not an image this build was compiled from. ..."},
"launcher_status": {"bios": "CRC32 318178BF (this build expects SCPH-5552, CRC32 D786F0B9).",
                    "disc": "verdict=ok serial=SLES-01156 expected=SLES-01156 tracks=1"}
```

| Field | Meaning |
| --- | --- |
| `exit_origin` | `start_refused`: the program refused to start the game. `launcher_closed`: the player closed the launcher without Play. `setup_relaunch`: setup finished and starts the game. |
| `start_refused` | `null`, or the refusal: `kind` for tools, and the `title` and `message` of the box the player was shown. |
| `launcher_status` | `null`, or what the launcher's BIOS row and disc row said last. A red row blocks Play, so a start that ends as `launcher_closed` has its reason here. |

Once the game runs, `start_refused` and `launcher_status` are `null`.

`disc_warning` is `null`, or a nonfatal known-disc missing-SBI sentence. It uses
the mounted disc's detected serial and basename, not the expected config ID.
It persists while the game runs, so a later black screen or crash report still
contains the warning. A successful disc change or committed save-state disc
mount refreshes it from the active reader; a refused change keeps the old value.
A loaded SBI removes it; this field does not qualify the
disc pressing, companion identity or protection/gameplay result.

## File names

The report names a file by its **base name** only, never by its folder.
Players paste this file into a chat, and it held no path before. The box,
which only the player sees, may show the full path. Text that is not UTF-8 (a
Windows file name in the system code page) is written as `\u00XX`, so the
report always loads as UTF-8 JSON.

## Network addresses

The report holds **no network address**: players send this file to other
people, and the other player's address is another person's. A failed netplay
start (`netplay_start`) is the one refusal whose sentence names an address.
The report gets the same sentence with each address as the fixed word
`(address)`: the listen address, the other player's address, and a listen
text that is not an address at all. The listen port stays when it is a plain
number ("UDP port 47810 on (address)"). The box and the log line keep the full
sentence.

## What a player typed that can still reach the report

A file's base name (a disc image called after its owner keeps that name), a
mod's name, and a line of `game.toml` that the TOML reader quotes in its
error (a full path in that line is cut to its base name like any other). No
host name, no address and no folder.

## Kinds

| `kind` | When |
| --- | --- |
| `config_unreadable` | `game.toml` cannot be read. |
| `memcard_dir` | The folder given with `--memcard-dir` cannot be created. |
| `overlay_cache` | The cache of compiled game code cannot be set up. |
| `no_disc` | No usable disc: the image does not open, its `.sbi` is missing, or the picker was cancelled. |
| `mods` | The selected mods cannot be applied, or cannot be switched off for a netplay match. |
| `no_bios` | No usable BIOS: the file is not an image this build was compiled from, the bundled image is missing, or the picker was cancelled. |
| `setup_program` | The program is a setup program: no game and no BIOS code is linked into it. Run Generate and rebuild; the game is in `build-release`. |
| `bios_mismatch` | The BIOS could not be made active when the session started. |
| `disc_not_mounted` | The image verified but the drive cannot mount it. |
| `already_running` | Another program of the same set runs from the folder. |
| `video_init` | The video system, the window or the picture output cannot be created. |
| `build_defect` | The build's table of BIOS routines is larger than the program holds. |
| `netplay_session_bios` | A match from the launcher needs a retail BIOS that is not on this computer. |
| `netplay_disc` | The disc image is not valid for online play, or none is verified. |
| `netplay_address` | A match was started with no address to listen on. |
| `netplay_seat` | A command-line match was started from a seat that is not a pad. |
| `netplay_start` | A command-line match could not be started: the port is in use or not allowed, the address is not valid, or the build has no netplay. The message is the sentence the start itself gives (PS1B-386), with each address as `(address)`. |

A match started from the lobby that fails in one of the last two ways is not a
refused start: the player returns to the room, where the status line shows the
sentence.

Not recorded: the developer gates that exit with code 2 (input routes, TAS
state files).

## Boxes

The refusals that printed only a line (no disc or BIOS after a cancelled
picker, mods, the cache, the window, a faulty build, the netplay refusals
above) open a box when a person started the game. A scripted start gets no
new box: `--headless`, `PSX_HEADLESS`, `--no-launcher`, `PSX_NO_LAUNCHER`, a
replay launch, or `[launcher] skip_launcher = true` in `settings.toml`. It
gets the old line on stderr and one more: `psxrecomp: start refused (<kind>)`.
