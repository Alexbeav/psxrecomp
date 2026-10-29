# PS1B-415 launcher provider update
Framework parent: fa0ab633 (stage/pin-i with PS1B-403).
UI parent 4f547c2b6a32a9e7fd8a299932907ce797508a40 moves to addc54c74c78a913e99b342f6b664a6e4e4c8369 (tree 508398db242220b2f1b728140b94b04a8c7e6309): 4f547c2b merged with lane/ps1b-415-ui 2c764cff (base f998b0a7, which carries the PS1B-403 Serial-row commits), no conflict.
Reason: the launcher disc panel follows a file that setup refused, so it no longer says "Disc verified" above setup's refusal; the host side (this merge of lane/ps1b-415-fw c446aba6) remembers the one file the CLI refused and reports the final verdict. Only tools/ci/recomp_ui_pin.txt moves a provider record. No qualified-hunk, codegen hash, replay profile or route-source record moves.
Merge: docs/RUN_REPORT_START.md conflicted (both sides added a paragraph); both kept.
Measurement: runtime/tests/test_setup_refusal_verdict.py fails on the stage tree and passes on this tree; the launcher tests run in the Windows gate (a) launcher tree.
Evidence: Evidence/PS1B-415/PS1B-415-burndown-20261022 (source), Evidence/PS1B-526 pin I landing 2026-10-29 (landing).
