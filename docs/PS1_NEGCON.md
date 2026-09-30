# neGcon

The runtime can present a Namco neGcon twist controller (NPC-101 / SLEH-0003)
on a controller port. The seat's own pad or keyboard drives it.

The device behaviour follows PSX-SPX, "Controllers - Racing Controllers".

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

Many racing games support the neGcon. Several have a neGcon calibration screen
in their controller options, which is the quickest check.

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

The twist reads the stick's left/right position only. Moving the stick up or
down does not change it, so a full-lock turn stays full lock on a diagonal.
The stick's deadzone setting applies to the twist, measured on left/right
alone. A pad whose triggers are on/off only (for example Switch controllers)
gives full I and II.

## Limits

- The twist direction (which end reads `00h`) and the released value of I, II
  and L are open points in PSX-SPX. The runtime uses `00h` = full left and
  `00h` = released. A game's calibration screen shows if either is reversed.
- PSX-SPX documents no reply to commands other than `42h`. The runtime answers
  them like a plain digital pad: hi-z and no /ACK.
- Netplay carries pads only. A neGcon seat plays as a pad in netplay.
- Replays and debug-server input drive pads only.
- game.toml modes stay `analog` or `digital`; the neGcon is a player choice.
- A title with `lock_mode` keeps its locked pad type; the launcher hides the
  pad-type row for it.
- A launcher without the NeGcon entry (a recomp-ui that does not define
  `RECOMP_LAUNCHER_HAS_NEGCON_MODE`) shows a neGcon seat as Analog; leaving it
  there keeps the neGcon.

## Tests

- `sio_negcon_p2_test`, `sio_negcon_p5_test`: the byte protocol, including a
  multitap seat; a read keeps the state it started with; every twist value
  and the pressure ends; hi-z with no /ACK for every command byte except 42h.
- `psx_stick_axis_test`: the twist's one-axis stick transform (exact ends and
  centre, never decreasing, the same as before for a straight left/right push).
- recompiler `negcon_twist_wiring`: the neGcon sampler takes the twist from
  that transform, not from the DualShock stick transform.
- `sio_pad_bus_identity_p{2,5}_test_kinds`: pad and memory card traffic stays
  byte-identical to the recorded goldens after every port was a neGcon and
  went back to a pad.
- `launcher_device_roundtrip_test`: the pad type at the launcher seam.
- recompiler `negcon_settings_test`: `pN_mode = "negcon"` in settings.toml.
- recomp-ui `recomp-ui-launcher-negcon-mode`: the launcher's NeGcon pad type.
