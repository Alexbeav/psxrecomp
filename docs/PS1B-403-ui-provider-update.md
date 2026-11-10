# PS1B-403 launcher provider update
Framework parent: fa26d4ff (stage/pin-i with PS1B-481 and PS1G-50; PS1B-412 is not landed).
UI parent e85b3f126d277c15943bf6387b15de294c3ccd06 moves to 4f547c2b6a32a9e7fd8a299932907ce797508a40 (tree ba76c4f755fccc6639d4ca535d4343c1604f4a39): e85b3f12 merged with lane/ps1b-403-ui 1604816c (base f998b0a7), no conflict.
Reason: the launcher ticks the Serial row only for a serial the build lists; the host side (this merge of lane/ps1b-403-fw 554e34ee) says whether the disc serial is listed. Only tools/ci/recomp_ui_pin.txt moves a provider record. No qualified-hunk, codegen hash, replay profile or route-source record moves.
Measurement: runtime/tests/test_launcher_serial_status_wiring.py fails on the stage tree and passes on this tree; the launcher tests run in the Windows gate (a) launcher tree.
Evidence: Evidence/PS1B-403/PS1B-403-burndown-20261022 (source), Evidence/PS1B-526 pin I landing 2026-10-29 (landing).
