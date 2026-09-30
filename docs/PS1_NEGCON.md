# neGcon

The runtime can present a Namco neGcon twist controller (NPC-101 / SLEH-0003)
on a controller port. The seat's own pad or keyboard drives it. Tracking:
PS1B-304 (under PS1B-279).

The device behaviour follows the clean-room spec in `recomp-corpus`,
`references/ps1/PERIPHERAL-NEGCON-SPEC.md`, which is written from PSX-SPX only.

## Turning it on

In the launcher, pick **NeGcon** in the player's pad-type row, next to Analog
and D-Pad. Hover it to see the button layout.

Without the launcher, set the port's mode in `settings.toml` next to the
executable:

```toml
[controller]
p1_device = "gamepad"   # or a pad GUID, or "keyboard"
p1_mode = "negcon"
```

Many racing games support it: Wipeout, Wipeout XL, Gran Turismo, Destruction
Derby 2, Colin McRae Rally 2.0, Rollcage and others. Wipeout XL and Gran
Turismo have a neGcon calibration screen, which is the quickest check.

## Controls

| neGcon | Pad | Keyboard |
|---|---|---|
| Twist | left stick left/right | the left-stick binds |
| I (accelerate) | right trigger, analog; Cross gives full I | the Cross key |
| II (brake) | left trigger, analog; Square gives full II | the Square key |
| L | L1 (full on or off) | the L1 key |
| A | Circle | the Circle key |
| B | Triangle | the Triangle key |
| R | R1 | the R1 key |
| Start, D-pad | Start, D-pad | the same keys |

The stick's deadzone setting applies to the twist. A pad whose triggers are
on/off only (for example Switch controllers) gives full I and II.

## Limits

- The twist direction (which end reads `00h`) and the released value of I, II
  and L are open points in PSX-SPX. The runtime uses `00h` = full left and
  `00h` = released. A game's calibration screen shows if either is reversed.
- Netplay carries pads only. A neGcon seat plays as a pad in netplay.
- Replays, input routes, TAS runs and debug-server input drive pads only.
- game.toml modes stay `analog` or `digital`; the neGcon is a player choice.
- A title with `lock_mode` keeps its locked pad type; the launcher hides the
  pad-type row for it.
- A launcher without the NeGcon entry (recomp-ui before the PS1B-304 change)
  shows a neGcon seat as Analog; leaving it there keeps the neGcon.

## Tests

- `sio_negcon_p2_test`, `sio_negcon_p5_test`: the byte protocol, including a
  multitap seat.
- `sio_port_device_identity_p{2,5}_test_{0,1,2}_negcon_roundtrip`: pad and
  memory-card traffic stays byte-identical to pin F after a port was a neGcon
  and went back to a pad.
- `launcher_device_roundtrip_test`: the pad type at the launcher seam.
- recompiler `negcon_settings_test`: `pN_mode = "negcon"` in settings.toml.
- recomp-ui `recomp-ui-launcher-negcon-mode`: the launcher's NeGcon pad type.
