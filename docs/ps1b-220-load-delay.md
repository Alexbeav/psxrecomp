# PS1B-220 load-delay work

Exposure: the handover records prior PS1B-102 reference-behavior comments.
Implementation inputs are PSX-SPX, the clean L1 specification, and its authored fixtures.
No restricted reference source is used.

The baseline is b8ccd50fafda9b90fb5cd765ec62318aff663027.
Run `python runtime/tests/test_load_delay_l1.py` with WinLibs UCRT gcc on PATH.
The fixture executes the production decoder through a test-only entry point.
Unexpected device or dispatch seams terminate the test.

The first baseline test uses L1 case a at a game-RAM address.
It fails at O0 with `slot=bbbb0002 after=bbbb0002`.
L1 requires `slot=aaaa0001 after=bbbb0002`.
Evidence is `../ps1b-220-baseline-a.log` in the private workspace parent.
This establishes the eager-value failure on this decoder path only.
It does not establish native, overlay, precise, exception, or performance results.

The intended change carries pending values in CPUState across executor boundaries.
An instruction reads the old GPR, then its write can cancel the pending value.
An LWL/LWR merge reads the pending value without exposing it to ordinary operands.
Accepted exception entry commits the pending value before the handler reads registers.
The timing model remains outside this change.

Full L1 cross-executor comparison, save round trips, seven Tier 1 routes,
Tier 3, and an interleaved two-title performance comparison remain required.
The branch is private preparation and must not land before those gates.
