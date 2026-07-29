# PR #13 FPS telemetry provenance

Parked from NyperYuhgard's PSXrecomp PR #13, commit
[`b49548e7b8f494c9584eb09039a8c325d7375b46`](https://github.com/mstan/psxrecomp/commit/b49548e7b8f494c9584eb09039a8c325d7375b46).

This branch retains only the title-neutral simulated-vblank FPS and realtime-speed
telemetry. It was adapted to current master and guards window-title access in
headless/windowless operation. The overlay-cache inventory from the same upstream
commit is parked separately.
