# Development & Validation Workflow

## Principle

Optimize validation cost based on the current confidence level.

Do not spend minutes validating a feature that the user can reject in seconds.

Progress through increasingly expensive validation gates only after the
previous gate has passed.

## Gate A — Local correctness (developer)

Before handing a build to the user:

- Build succeeds.
- Static analysis and unit tests, if applicable, pass.
- The game launches successfully.
- The affected feature reaches its intended test location.
- Run only the smallest deterministic replay or smoke test necessary to verify
  the implementation did not obviously break.

Do not run long campaign routes or the full regression suite here unless the
change is inherently high risk.

## Gate B — User evaluation

Immediately provide the user with a test candidate.

Always include:

- Exact launch shortcut.
- What changed.
- Where to test.
- Two to five specific behaviors to inspect.
- Expected result.
- Known limitations or areas of uncertainty.
- Any useful A/B comparison build, if applicable.

Then stop and wait for feedback.

## Gate C — Iteration

If the user rejects the feature:

- Modify only the affected implementation.
- Repeat Gate A.
- Produce a new candidate immediately.

Do not perform expensive regression testing on implementations likely to be
discarded.

## Gate D — Qualification

Only after the user explicitly approves the feature:

- Run focused regression tests.
- Run deterministic replay validation.
- Run campaign validation where appropriate.
- Run the full automated test suite.
- Verify compatibility builds.
- Check artifacts and performance.
- Update documentation.

## Gate E — Finalization

Only after qualification succeeds:

- Commit.
- Push.
- Update documentation and the changelog.
- Mark the feature complete.

## Exceptions

Deeper validation before user testing is justified only when the change may:

- Corrupt save data.
- Break deterministic replay compatibility.
- Modify serialization formats.
- Affect persistent state.
- Alter core runtime invariants such as CPU, GPU, DMA, timing, scheduler, or
  memory behavior.
- Risk data loss.

Even then, perform the minimum focused validation required, not a blanket
full-campaign run.

## Philosophy

The validation cost should scale with confidence.

Cheap rejection first. Expensive validation last.

User feedback is part of the development loop, not the release process.

Never spend 30 minutes proving that a feature works before confirming the user
even wants that version of the feature.

## One feature per candidate

Whenever practical, each candidate build should contain a single logical
change. Avoid bundling unrelated fixes together.

A candidate should answer one question only:

> Did this specific change improve the game?

This makes user feedback actionable, simplifies regression analysis, and makes
reverts trivial.
