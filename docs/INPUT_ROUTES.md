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
- Press **F11** to drop a MENU marker and **F12** to drop a GAMEPLAY marker.
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

Identity tags are all-or-nothing and unique. Marker and checkpoint frames are
boundaries: frame N is after record N was supplied and before record N+1, and
frame 0 is before the first record. Frames strictly increase, are at most
`frame_count`, and every checkpoint has a marker at the same frame. The runtime
reader refuses unknown `0x800001xx` tags so that a newer identity field is never
silently ignored.
