# GunCon

The runtime can present a Namco GunCon (G-Con 45, NPC-103) light gun on a
controller port. The host mouse pointer aims it. Tracking: PS1B-305 (under
PS1B-279).

The device behaviour follows the clean-room spec in `recomp-corpus`,
`references/ps1/PERIPHERAL-GUNCON-SPEC.md`, which is written from PSX-SPX only.
The Konami Justifier (IRQ10 type) is a different gun and is not emulated.

## Turning it on

In the launcher, pick **GunCon** as the player's input source.

Without the launcher, set the port's device in `settings.toml` next to the
executable:

```toml
[controller]
p1_device = "guncon"
```

GunCon games include Time Crisis, Time Crisis: Project Titan, Point Blank 1-3,
Resident Evil Survivor (PAL/Japan), Die Hard Trilogy 2 and Elemental Gearbolt.
Most have a gun calibration screen ("shoot the target"). Run it first: it
absorbs any small offset between the cursor and where the game thinks you
aimed.

## Controls

| GunCon | Host (default) | keybinds.ini `[guncon]` |
|---|---|---|
| Aim | the mouse pointer over the game image (a crosshair cursor shows) | |
| Trigger | left mouse button | `trigger` |
| A (left side button) | right mouse button, or A | `a` |
| B (right side button) | middle mouse button, or D | `b` |
| No light (aim off-screen, e.g. reload) | hold mouse button 4 (the first side button), or aim outside the game image | `no_light` |
| Off-screen shot (no light + trigger) | hold W | `offscreen_shot` |

Resident Evil Survivor walks on an off-screen shot and turns with A and B, so
the defaults give W to walk and A / D to turn. If A turns right in a game,
swap the `a` and `b` keys.

The controls are rebindable in `keybinds.ini` next to the executable, section
`[guncon]`. Each takes a key or `Mouse1`..`Mouse5` (1 left, 2 middle, 3 right,
4/5 side), plus an optional second binding after a comma:

```ini
[guncon]
trigger        = Mouse1
a              = Mouse3, A
b              = Mouse2, D
no_light       = Mouse4
offscreen_shot = W
```

A `keybinds.ini` without the section uses these defaults; a new file is
written with it. The keys share the keyboard with any keyboard player's pad
binds, so rebind one of them if both use the same key.

The pointer is never captured. Outside the game image (the black bars), with
the window unfocused or with a host menu open, the gun sees no light.

## Limits

- The gun's X offset is an open point in PSX-SPX. The runtime uses the
  standard display range less 11 clocks. Calibrate in the game.
- The aim follows the letterboxed image at the configured aspect ratio (4:3,
  16:9 or 21:9). Presentation mods that move or split the image are not
  followed.
- Brightness is not modelled: dark areas still register a hit.
- A PS1 Mouse and a GunCon share the host mouse. Use one at a time; while a
  PS1 Mouse holds the pointer captured, the gun sees no light.
- Netplay, replays, input routes, TAS runs and debug-server input carry pads
  only.
- A launcher without the GunCon entry (recomp-ui before the PS1B-305 change)
  shows a GunCon seat as None; leaving it at None keeps the GunCon.
- The launcher's Controls page does not show the `[guncon]` controls yet;
  edit `keybinds.ini`. A launcher before the PS1B-305 keybinds change drops
  the `[guncon]` section when it saves a pad rebind.

## Tests

- `sio_guncon_p2_test`, `sio_guncon_p5_test`: the byte protocol, including a
  multitap seat.
- `sio_port_device_identity_p{2,5}_test_{0,1,2}_guncon_roundtrip`: pad and
  memory-card traffic stays byte-identical to pin F after a port was a GunCon
  and went back to a pad.
- `guncon_map_test`: pointer to X/Y, from the spec's host-mapping vectors,
  and the host controls to the gun's buttons and "no light".
- `guncon_keybinds_test`: the `[guncon]` defaults, loading, saving, and a file
  without the section.
- recomp-ui `recomp-ui-psx-binds-sections`: a launcher rebind keeps `[guncon]`.
- `launcher_device_roundtrip_test`: the GunCon source at the launcher seam.
