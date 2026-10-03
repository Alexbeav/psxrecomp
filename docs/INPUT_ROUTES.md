# Input routes: record in diagnostic mode, replay in any product

An input route is a frame-exact list of controller inputs, one per guest vblank
from boot. You record a route by playing the diagnostic product. Any product,
release or diagnostic, on Windows, Linux or macOS, can then replay it and check
that the game reached the same state.

A route is valid only for the product it was recorded on: the same psxrecomp
commit, disc, BIOS and boot mode. Replay refuses anything else before the first
guest instruction runs.

## Record a route (diagnostic product)

Set `PSX_INPUT_ROUTE_RECORD` to a new file name and start the diagnostic
product. The file must not exist yet; the recorder never overwrites a route.

```bash
PSX_INPUT_ROUTE_RECORD=pe-title-to-gameplay.psxrti3 build-diagnostic/Parasite_Eve --disc "disc/Parasite Eve (USA) (Disc 1).cue" --bios SCPH1001.BIN
```

- Play with the keyboard or a controller. P1 is recorded as a digital pad: the
  button word only, no sticks. Recording declares one digital pad on port 1 and
  nothing on the other ports, whatever controllers are plugged in.
- Press **Shift+F11** to drop a MENU marker and **F12** to drop a GAMEPLAY
  marker. (Plain F11 is the player replay recorder.)
  The marker lands on the next frame boundary. At each marker the recorder
  stores the guest cycle count, the SHA-256 of main RAM and a hash of every
  4 KiB RAM page.
- Quit normally (close the window, or the debug server's `quit`). The route is
  written at exit and read back before the recorder reports
  `input_route_recorded: path=... frames=... steps=... markers=...`.
- Save states, the save-state menu and rewind are off while recording, because
  loading an earlier state would make the route unreplayable.

An agent can record headless (`PSX_HEADLESS=1`) and drive input through the
debug server (`press`, `set_input`). Markers come from these commands:

| Command | Reply |
| --- | --- |
| `{"cmd":"route_record_marker","kind":"menu"}` (or `"gameplay"`) | `ok`, or an error when not recording |
| `{"cmd":"route_record_status"}` | `recording`, `path`, `frames`, `steps`, `markers`, `pending_markers`, `truncated` |

The recorder stops adding input at the format caps (`INPUT_ROUTE_MAX_FRAMES`,
`INPUT_ROUTE_MAX_STEPS`) and says so; the route written is still valid up to
that frame.

## Replay a route (any product)

```bash
PSX_INPUT_ROUTE_FILE=pe-title-to-gameplay.psxrti3 PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS=1 PSX_HEADLESS=1 build-release/Parasite_Eve --disc "disc/Parasite Eve (USA) (Disc 1).cue" --bios SCPH1001.BIN
```

- With `PSX_INPUT_ROUTE_FILE` unset, nothing in this path runs.
- The product prints `input_route_identity: match ...`, or refuses with one
  line per differing field (`pin`, `disc`, `bios`, `boot`, `disc hash`) and
  exits with status 2.
- At every marker it prints
  `input_route_marker: frame=N kind=menu|gameplay cycle=C ram_sha256=H checkpoint=match|mismatch`.
  On a mismatch a second line lists the RAM pages that differ.
- `PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS=1` exits after the last marker: status 0
  when every checkpoint matched, 3 otherwise.
- After the last input the route holds all buttons released; physical input
  never takes over P1.
- The release product also replays PSXRTI1 and PSXRTI2 routes, with no identity
  check. The diagnostic product keeps its TAS replay path and evidence
  observers for those formats unchanged.

## Pictures during a replay (any product)

Set `PSX_INPUT_ROUTE_CAPTURE_DIR` to a new, empty folder next to
`PSX_INPUT_ROUTE_FILE`. The product then writes into that folder:

| File | Written | Content |
| --- | --- | --- |
| `frame-NNNNNN.png` | at each picture | the guest display at boundary N (before input N+1) |
| `checkpoints.jsonl` | one line per picture | RAM SHA-256, hash of the delivered input, display size |
| `input-end.json` | at the route's last input | the hash of every delivered input |
| `complete.json` | at the route's last input, last of the capture's own files | frame count, input hash, stop reason |
| `slice-diag.json` | after `complete.json` | counters of the precise-slice interpreter |
| `initial-cards.json` | at admission, DualShock routes only | which memory cards were inserted |

```bash
PSX_INPUT_ROUTE_FILE=boot-to-menu.psxrti PSX_INPUT_ROUTE_CAPTURE_DIR=shots PSX_INPUT_ROUTE_CAPTURE_EVERY=300 PSX_HEADLESS=1 build-release/Parasite_Eve --disc "disc/Parasite Eve (USA) (Disc 1).cue" --bios SCPH1001.BIN
```

- A picture is written at boundary 0, every `PSX_INPUT_ROUTE_CAPTURE_EVERY`
  boundaries (1 to 10000, default 300) and at the route's last input. There
  the product prints `input_route_complete: frames=N ...` and exits with
  status 0. Without the folder a replay runs on after the route, as before.
- **A picture run is never the run that proves a route.** Prove the route
  with a run without the folder (its marker lines and exit status), and take
  the pictures in a second run. To write a picture the product copies the
  renderer's frame buffer into the CPU copy of video memory earlier than the
  game would cause it. One run shows equal marker lines with and without the
  folder: Kula World (Europe), 3,600 frames, started plain, with pictures
  and plain again on one Windows host (PS1B-404, evidence folder
  `pegasus-marker-equality-kula-world-20261003`). The other renderer and a
  24-bit movie are not shown, so the rule stays.
- Status 0 means the pictures were written. It does not mean the markers
  matched: without `PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS=1` a marker mismatch
  is printed and the run still ends with 0.
- `PSX_INPUT_ROUTE_EXIT_AFTER_MARKERS=1` still exits at the last marker.
  Pictures after that marker are not written.
- The folder is never written twice. A file that already exists stops the
  product with status 3, and a folder that holds a `complete.json` refuses
  the route before the game starts.
- Each input is checked against what the pad port received. A difference
  stops the product with status 3 (`input route observation failed: ...`).
  - A digital route (PSXRTI1, or PSXRTI3 with digital records) needs a
    connected pad on port 1 that is not in analog mode. A product whose pad
    is in analog mode stops with status 3 at the boundary after the first
    input.
  - A DualShock route (PSXRTI2) needs a DualShock on port 1 and no memory
    card in either slot. A kit's product creates card 1 by default, so
    switch both cards off for the picture run: `enable1 = false` and
    `enable2 = false` under `[memcard]` in the product's `settings.toml`.
- A release product names its exit in the run report
  (`psx_last_run_report.json`, `exit_origin`): `input_route_capture_complete`
  when `complete.json` was written, `input_route_capture_failed` for a stop
  with status 3.
- With the folder unset a release product does none of this: a loaded route
  costs one more branch per frame, and no route costs nothing.
- The release product honours these two variables only. The other observer
  options (`PSX_INPUT_ROUTE_WATCH_U16`, `PSX_INPUT_ROUTE_CARD1_SHA256`,
  `PSX_INPUT_ROUTE_NEUTRAL_TAIL`, `PSX_INPUT_ROUTE_CPU_STATE`,
  `PSX_INPUT_ROUTE_VIDEO_STATE`, `PSX_INPUT_ROUTE_TRACE`) belong to the
  diagnostic product. Set together with the folder, one of them refuses the
  route before the game starts and names the variable.
- The picture is the guest display area, not the window: no upscaling, no
  widescreen surface, no on-screen messages.

## Route identity

| Field | Value |
| --- | --- |
| pin | full 40-hex psxrecomp commit the product was built from (configure time) |
| disc | boot serial read from the mounted disc, e.g. `SLUS-00662` |
| disc hash | `.cue`: SHA-256 over the hex SHA-256 of the cue file and each FILE track in cue order, joined by `\n`, no trailing newline. Other images: SHA-256 of the file. Same definition as `tools/tasreplays/run_native.py` `checkpoint_asset_digest` |
| bios | BIOS file stem, compared without case (`SCPH1001`) |
| boot | `lle`, `hle` (kernel-call HLE and boot skip), `hle-calls` or `hle-boot` |

A `.cue` and a `.chd` of the same disc have different disc hashes; replay the
route with the same kind of image it was recorded with.

## File format: PSXRTI3

PSXRTI1 and PSXRTI2 stay byte-frozen. PSXRTI3 carries the same records behind a
tagged extension block. The reader is `runtime/include/input_route_v3_file.h`;
`tools/input_route_v3.py show ROUTE` prints a route's identity and markers.

```
header   u8[8] "PSXRTI3\0", u32 version=3, u32 record_size, u32 frame_count,
         u32 flags=0, u32 ext_bytes              (little-endian, 28 bytes)
entries  ext_bytes of: u32 tag, u32 length, payload, zero padding to 4 bytes
records  frame_count records of record_size, then end of file
```

- `frame_count` is 1..1,000,000. `ext_bytes` is a multiple of 4 and at most
  16 MiB. The file size must equal `28 + ext_bytes + frame_count * record_size`.
- `record_size` 8 is a PSXRTI1 record (u32 one-based sequence, u16 active-low
  buttons, u16 0); 12 is a PSXRTI2 record. A PSXRTI3 digital route declares one
  digital pad on port 1.
- Tag bit 31 marks an entry that does not change replay. Readers skip unknown
  bit-31 tags and refuse any other unknown tag.

| Tag | Owner | Payload |
| --- | --- | --- |
| `0x80000101` | T101 | pin, 40 lowercase hex |
| `0x80000102` | T101 | disc boot serial |
| `0x80000103` | T101 | u8 kind (1 cue, 2 file), u8[3] 0, u8[32] disc SHA-256 |
| `0x80000104` | T101 | BIOS stem |
| `0x80000105` | T101 | boot mode |
| `0x80000110` | T101 | marker: u32 frame, u8 kind (1 MENU, 2 GAMEPLAY), u8[3] 0 |
| `0x80000111` | T101 | checkpoint: u32 frame, u32 0, u64 guest cycle, u8[32] RAM SHA-256, u64[512] FNV-1a 64 page hashes |
| `0x000002xx` | T98 | port layout, console events, disc set (mandatory; not read yet) |
| `0x00000301` | PS1B-191 | player replay anchor: the save state the replay starts from |
| `0x00000302` | PS1B-191 | player replay settings: ASCII `key=value` lines |
| `0x00000303` | PS1B-191 | player replay state digests: u32 count, then u32 frame and four u32 digests each |
| `0x80000304` | PS1B-191 | player replay thumbnail: u16 width, u16 height, ARGB pixels |
| `0x80000305` | PS1B-191 | player replay name, UTF-8 |
| `0x00000306` | PS1B-316 | power-on start: u32 vblank, always 0 (a replay has this or an anchor, never both) |
| `0x00000307` | PS1B-316 | memory cards at power-on: u32 mask (bit 0 card 1, bit 1 card 2), then 128 KiB per inserted card |
| `0x80000308` | PS1B-316 | product lines: `exe_sha256`, `codegen`, `bios_crc32`, `renderer`, `input_seed` |

Only the player replay reader (`input_route_v3_read_ex` with a replay
argument) admits the `0x3xx` tags. A route reader refuses a replay, so a replay
is never played as a route from power-on. See [PLAYER_REPLAYS.md](PLAYER_REPLAYS.md).

Identity tags are all-or-nothing and unique. Marker and checkpoint frames are
boundaries: frame N is after record N was supplied and before record N+1, and
frame 0 is before the first record. Frames strictly increase, are at most
`frame_count`, and every checkpoint has a marker at the same frame. The runtime
reader refuses unknown `0x800001xx` tags so that a newer identity field is never
silently ignored.
