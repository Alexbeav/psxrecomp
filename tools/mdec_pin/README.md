# MDEC pin (removed for pin H)

This folder held two one-off diagnostic programs from 2026-06-27, `mdec_pin.c` and `mdec_e2e.c`, and a finding note.
They ran an early MDEC decode pipeline and a second pipeline on the same synthetic input and compared the stages.
The second pipeline was reference-emulator code copied into the tool. Copied reference code must not be in the tree, so the programs were removed (PS1B-216). The git history keeps them.

They were not part of any build, test or gate.

## What they were used for

They localised a colour fault in FMV output: the 15-bit pack path did not truncate a colour channel to 8 bits before it reduced it to 5 bits. The fault was fixed in `runtime/src/mdec.c` at the time.

The note also recorded a lesson that still applies: a pin whose expected values come from a copy of your own code cannot catch an error that both copies share. The fault was found only by comparing video memory against the oracle process.

## What pins the MDEC now

The MDEC decode was rewritten later (PS1B-188). It is pinned by oracle-observation fixtures: `runtime/tests/data/mdec_clean` (sets M1-M5 and M10-M13), checked by `runtime/tests/test_mdec_clean_fixtures.py`.

A replacement for the stage-by-stage comparison, if one is wanted, takes its expected values from an oracle observation fixture, not from reference code.
