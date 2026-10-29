# PS1B-483 launcher provider update
Framework parent: db2766fa (stage/pin-i with PS1B-415).
UI parent addc54c74c78a913e99b342f6b664a6e4e4c8369 moves to eccd4f56111f45b7bf4e22a4a983a88b555a626b (tree 1c67f9012572f4d77216d5e43ab33c3b026e9be0): addc54c7 merged with 9aba62da and 9404fce3 (base f998b0a7), no conflict.
Reason: the launcher window scale applies to windowed mode only, so changing it no longer resizes borderless fullscreen; the host side (framework patch f8355420, "runtime: window scale applies to windowed mode only") applies the scale only to a windowed start. Only tools/ci/recomp_ui_pin.txt moves a provider record. No qualified-hunk, codegen hash, replay profile or route-source record moves.
Measurement: recomp-ui-launcher-video-pick-test fails on the stage UI pin and passes on eccd4f56; the launcher tests run in the Windows gate (a) launcher tree.
Evidence: Eagle lane PS1B-483-launcher-video-20261022 (source), Evidence/PS1B-526 pin I landing 2026-10-29 (landing).
