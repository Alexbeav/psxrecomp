# PS1 Mouse

The runtime can present the Sony PlayStation Mouse (SCPH-1030 / SCPH-1090) on a
controller port. The host mouse drives it. Tracking: PS1B-279.

The device behaviour follows the clean-room spec in `recomp-corpus`,
`references/ps1/PERIPHERAL-MOUSE-SPEC.md`, which is written from PSX-SPX only.

## Turning it on

In the launcher, pick **PS1 Mouse** as a player's input source. In a
one-player game, the Player 1 card then owns both console ports:

- **Mouse in port 1 / port 2** chooses the mouse's port.
- **Controller in port N** puts a pad, or the keyboard, in the other port for
  the same player. Many mouse games expect one: Syndicate Wars keeps pause and
  view rotation on the pad, and Quake II moves on the pad and aims with the
  mouse.

A game with a Player 2 card sets each port on its own card. A launcher
without the PS1 Mouse entry (recomp-ui before bc4ea2ea) shows a mouse port as
**None**; leaving it at None keeps the mouse.

Without the launcher, edit `settings.toml` next to the executable. Set the
port's device to `mouse`:

```toml
[controller]
p1_device = "mouse"         # the mouse in port 1
mouse_sensitivity = 1.0     # 0.05 .. 10.0; host pointer counts per mouse count
mouse_capture = true        # capture the pointer when the game starts
```

Some games want a pad in port 1 and the mouse in port 2 (the pad moves, the
mouse aims). Use `p1_device = "keyboard"` (or a pad) and `p2_device = "mouse"`.

A pad next to the mouse is read even in a one-player game: while a port holds
the mouse, the runtime samples both ports.

`pN_mode` does not apply to a mouse port.

## Capture

While a mouse port exists, the runtime captures the pointer (SDL relative mode,
hidden cursor) when the window has focus and no host menu is open.
`F10` toggles capture. Rebind it in `config.ini`:

```ini
[KeyMap]
MouseCapture = Ctrl+M
```

With capture off, or the window unfocused, the mouse reports no motion and both
buttons released.

## Limits

- A keyboard in port 2 uses the `[player2]` section of `keybinds.ini`. It
  starts with the same defaults as `[player1]`; rebind it from Configure.
- Netplay carries pads only. Every netplay seat is a pad.
- Replays, input routes, TAS runs and debug-server input drive pads only. They
  never sample the mouse. A player replay (F11, or the launcher's Record
  replay) refuses to start while either port holds a mouse (PS1B-313 tracks
  peripherals in replays).
- `keybinds.ini` mouse binds (`Mouse1`, `LMB` and so on) stay live for pad
  seats. If you bind a mouse button to a pad button, a click on a mouse port
  also presses that pad button. The default keybinds bind no mouse buttons.
- The mouse kind is not part of save states. A state loads into whatever device
  the port has now.
- Behaviour that PSX-SPX does not document is a runtime choice (see the spec's
  open questions): the reply to commands other than 42h, and motion beyond one
  byte between reads, which carries to the next read.

## Tests

- `sio_mouse_p2_test`, `sio_mouse_p5_test`: the byte protocol, including a
  multitap seat.
- `sio_port_device_identity_p{2,5}_test_{0,1,2}[_mouse_roundtrip]`: pad and
  memory-card traffic is byte-identical to pin F (a3e5fd892) under the default
  model and both source pad profiles, also after a port was a mouse and went
  back to a pad.
- `offline_seats_test`: a pad next to a mouse is read in a one-player game.
- recomp-ui `recomp-ui-launcher-mouse-source`: the launcher's mouse source and
  the Player 1 mouse + pad rules.
