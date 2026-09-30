# PS1 Mouse

The runtime can present the Sony PlayStation Mouse (SCPH-1030 / SCPH-1090) on a
controller port. The host mouse drives it.

The device behaviour follows PSX-SPX, "Controllers - Mouse".

## Turning it on

In the launcher, pick **PS1 Mouse** as the player's input source (a recomp-ui
that defines `RECOMP_LAUNCHER_HAS_MOUSE_SOURCE`).

Without the launcher, edit `settings.toml` next to the executable. Set the
port's device to `mouse`:

```toml
[controller]
p1_device = "mouse"             # the mouse in port 1
ps1_mouse_sensitivity = 1.0     # 0.05 .. 10.0; mouse counts per host pointer count
ps1_mouse_capture = true        # capture the pointer when the game starts
```

Some games want a pad in port 1 and the mouse in port 2 (the pad moves, the
mouse aims). Use `p1_device = "keyboard"` (or a pad) and `p2_device = "mouse"`.
The title must be built for two players for port 2 to exist.

`pN_mode` does not apply to a mouse port. These keys are not recomp-ui's
`mouse_sensitivity`, which is the aiming rate for titles with mouse aiming.

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

- A launcher without the PS1 Mouse source shows a mouse port as **None**. If
  you leave it at None, the port stays a mouse. Choosing Keyboard or a pad
  replaces the mouse.
- Netplay carries pads only. Every netplay seat is a pad.
- Replays and debug-server input drive pads only. They never sample the mouse.
- `keybinds.ini` mouse binds (`Mouse1`, `LMB` and so on) stay live for pad
  seats. If you bind a mouse button to a pad button, a click on a mouse port
  also presses that pad button. The default keybinds bind no mouse buttons.
- The mouse kind is not part of save states. A state loads into whatever device
  the port has now.
- Behaviour that PSX-SPX does not document is a runtime choice: the reply to
  commands other than 42h (hi-z, no /ACK), and motion beyond one byte between
  reads, which carries to the next read.

## Tests

- `sio_mouse_p2_test`, `sio_mouse_p5_test`: the byte protocol, including a
  multitap seat.
- `sio_pad_bus_identity_p{2,5}_test_kinds`: pad and memory card traffic stays
  byte-identical to the recorded goldens after every port was a mouse and went
  back to a pad.
- `launcher_device_roundtrip_test`: the mouse seat at the launcher seam.
- `host_keymap_test`: the `MouseCapture` hotkey.
- recompiler `ps1_mouse_settings_test`: the settings.toml keys.
- recomp-ui `recomp-ui-launcher-mouse-source`: the launcher's PS1 Mouse source.
