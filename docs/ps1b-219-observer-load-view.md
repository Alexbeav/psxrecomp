# PS1B-219: passive post-load register view

Exposure: prior PS1B-102 reference-behavior comments are recorded by the handover. This session has not opened prohibited emulator source. Owned source, clean L1 fixtures/specification, and PSX-SPX are the implementation inputs.

This branch depends on PS1B-220 candidate 8d49501a2 and is rooted at A1 pin-line 542730c72 through that candidate. Its issue-specific diff is git diff 8d49501a2..HEAD. Do not land it independently of the final accepted CPUState pending-value design. Rebase the small observer diff after PS1B-220 is qualified.

The frontend-return TSV now projects a carried load at age1, after the successor executed. At age0 it keeps the old GPR. It does not write CPUState, run a device, or advance the load pipeline. The instruction-boundary diagnostic is unchanged.

Evidence: `python runtime/tests/test_cpu_return_load_view.py` compiles the actual observer header with WinLibs UCRT GCC16.1 at O0/O2 and parses its TSV. Six snapshots cover both live ages, retired state, repeated same-register loads and cancellation. A byte comparison proves every observer call leaves CPUState unchanged. Baseline fails rows2/5 (old values); candidate passes. Logs: ps1b-219-baseline.log and ps1b-219-candidate.log beside STATUS.md. The exact oracle boundary convention is supplied by PS1B-219; PSX-SPX CPU Specifications defines completion after the successor, https://psx-spx.consoledev.net/cpuspecifications/ (consulted2026-09-26). Corpus search found no reviewed observer-specific note.

Next: replay actual source-mode observer boundaries with the same oracle selectors. Pegasus Tier1 and Tier3 must show each former ADV-001 exception matches before removing that exception row. This branch removes no baseline exceptions and makes no route qualification claim. No execution-state or save-format change is introduced by its issue-specific diff.

2026-09-27 final dependency update: rebased onto220 candidate8d49501a20466f52a04dc46869b6370f67d821d5. The six passive observer snapshots pass again at O0/O2 (ps1b-219-final-observer.log). Actual source-route Tier1/Tier3 and ADV-001 exception removal remain external gates.
