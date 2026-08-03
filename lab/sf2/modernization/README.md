# SF2 modernization pass 1

This opt-in profile remains separate from the compatibility executable at
commit `2009297`. It changes presentation and host input only:

- OpenGL at 4x internal resolution;
- borderless desktop-resolution 4:3 output;
- WASD-oriented keyboard bindings translated to ordinary retail PAD buttons;
- mouse buttons translated to retail fire/aim/weapon/roll/interact buttons;
- bounded relative mouse motion translated to retail D-pad turning, with the
  vertical axis active only while retail manual aim is held.

It does not write player or camera state, patch generated retail code, enable
widescreen, or claim smooth native free-look. Horizontal chase mouse control is
digital in this first pass because SF2 has no retail right-stick camera axis.

Use `tools/build_sf2_modernized.ps1`, then
`tools/start_sf2_modernized.ps1`. The baseline remains available through
`tools/start_sf2_baseline.ps1`; each uses a separate executable directory and
memory-card directory.

Keyboard bindings are W/S move, Q/E turn, A/D strafe, Left Ctrl fire, Left Alt
aim, Tab target, F interact, C kneel, Space roll, R weapon, and Escape pause.
Mouse bindings are left fire, right aim, middle weapon, X1 roll, and X2
interact. Mouse movement is translated to retail digital turn/aim input and is
therefore a compatibility-first approximation, not native free-look.

The modern launcher uses strict assigned-device routing for its lifetime. This
prevents the framework's developer convenience mode from merging unrelated
background controllers into the keyboard/mouse PAD stream. The baseline
launcher retains the compatibility build's existing behavior.

See `docs/sf2/MODERNIZATION_PASS1.md` for evidence, ownership accounting, and
the A/B acceptance procedure.
