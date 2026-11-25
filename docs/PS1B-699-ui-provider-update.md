# PS1B-699 launcher provider update
Framework parent: 6e8cf2ac (stage/pin-j, pin J v0.1.8).
UI parent 1f79bfeaa6d4cf6aaa7d55ce9a54dea36fb98f35 moves to cba15e92dbf6e9186cf35eff7cf99054e2bed4cb: one commit on 1f79bfe. This file held 24b4df46 since pin J v0.1; v0.1.4 to v0.1.8 paired 1f79bfe through the pin record without moving it.
Reason: with the PS1 Mouse in port 2 of a one-player PSX title, the Player 1 card drew the port 1 pad's type selector twice and offered the mouse port twice. Pin I's guard (2d5ba07) is restored and the duplicate port list from f17ed17 is removed. No host change. Only tools/ci/recomp_ui_pin.txt moves a provider record. No qualified-hunk, codegen hash, replay profile or route-source record moves.
Measurement: recomp-ui-psx-settings-draw-legacy and -module fail on 1f79bfe (two pad-type selectors on the mouse card) and pass on cba15e92; the recomp-ui test tree passes 81 of 81.
Evidence: Eagle lane PS1B-699-pini-loss-audit (source), Evidence/PS1B-699 (landing).
