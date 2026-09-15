# Unattended replay investigations

`investigate.py` runs an explicit JSON recipe, preserves each command attempt,
and compares every completed return. This is the canonical controller guide.
It requires Python 3.11+ and Windows for process supervision. Evidence-only
comparison also works without Windows. It does not schedule work or authorize
a replay: operator holds and the existing launchers' admission checks still apply.

## Try it without game data

From this checkout, choose fresh directories:

```powershell
python tools/tasreplays/test_investigate.py --example D:/psx-investigation-example
python tools/tasreplays/investigate.py run D:/psx-investigation-example/recipe.json D:/psx-investigation-run
```

The generated recipe uses only synthetic Python subprocesses. It deliberately
differs at return 2 and matches again at returns 3 and 4. Exit 2 is expected.
The controller then runs paired snapshot and trace diagnostics. The report shows
prefix 1, furthest returns `[4, 4]`, one differing RAM byte at return 2, and the
first differing configured trace operation at operation 3. This is no gameplay claim.

Run the regression suite with `python tools/tasreplays/test_investigate.py -v`.
It is also registered as CTest `tas_investigation_controller`; command supervision
tests skip on non-Windows hosts. No native build is needed for this suite.

## Recipe contract

Use `schema: "tas-investigation-v1"`, `expected_returns` (1..1000000),
`identity`, `comparison`, and an ordered `steps` array. Unknown optional fields
do not change execution. Required keys and finite budgets are checked before work.

* `identity` maps names to original file paths. `input`, `profile`, and `executable`
  are required. Add media, launcher scripts, source manifests and other relevant
  files. Inputs, profiles and controller modules are hashed and copied before
  any command. The replay executable is hashed and copied after the selected
  build/test steps, so those steps can produce it. The profile file should record
  the intended effective options; existing launchers still verify their own
  effective identity. The controller does not infer a profile from an executable.
* `comparison` has `source` and `native` directories containing the existing
  `ram-pages.tsv` format, beginning at return 1. An empty `steps` array performs
  read-only analysis. Original files remain in place; the report binds their
  paths, sizes and SHA-256 hashes. Captures must be quiescent.
* Each step has a unique `id` (lowercase letters, digits, underscore or hyphen),
  `phase`, and a nonempty `argv` array. Phases must be ordered `build`, `test`,
  `replay`, `snapshots`, `trace`. Any phase can be omitted. There is one process
  tree at a time. `cwd` defaults to the fresh attempt directory; `env` supplies
  explicit overrides. The effective environment and absolute command executable
  identity are retained with the command. These local receipts can contain
  environment values and should remain private.
* Every step requires positive `timeout` (seconds, at most 86400), `max_bytes`
  (at most 1 TiB), `max_files` (at most 1000000), and `memory_mib` (at most 262144).
  Storage and file limits cover the entire investigation directory, including
  prior attempts and preserved inputs. Windows Job Objects bound total committed
  memory and kill remaining descendants when the worker exits. The existing
  `process_budget.wait_budgeted` enforces wall time and sampled storage limits.
  Storage can overshoot between samples; commands must put all outputs under
  the investigation directory. This is supervision of trusted commands, not a
  filesystem sandbox. CPU usage is bounded by elapsed time, not a CPU percentage.
* `attempts` defaults to 1 and is limited to 3. A retry is permitted only after
  a durable failure result. Recipes with retries must pass `{attempt}` in their
  arguments and use it for fresh outputs. A later successful attempt never
  removes an earlier failure. Exhausted retries stop the investigation.

Placeholders in string values are `{python}`, `{tools}`, `{recipe_dir}`, `{run}`,
and command-local `{attempt}`. Diagnostics additionally receive `{first_return}`.
Arguments remain separate strings; no shell interpolation, expression evaluation,
endpoint binary search, or generated patch is involved. Relative file paths are
relative to the controller's working directory; prefer placeholders or absolute paths.

For example, select existing build and test targets with argv arrays of the form
`["cmake", "--build", "{run}/build", "--target", "chosen_target", "--parallel", "2"]`
and `["ctest", "--test-dir", "{run}/build", "-R", "^chosen_test$", "--output-on-failure"]`.
Supply an explicit preceding configuration command and the four budgets on every
step. The controller never chooses a broad build or test suite itself.

## Escalation and evidence

After replay, the controller streams all page hashes and return clocks, using
the established `observation_evidence.compare_returns` verdict on valid captures.
It retains partial counts and the earliest discrepancy even if a later row is
malformed. Missing returns on either or both sides are incomplete evidence.
Extra returns also prevent a complete match. A later matching return cannot
erase an earlier mismatch. The exact matching prefix describes RAM page hashes
and return clocks only; it is not byte-level equality or gameplay completion.

Only an observed state/clock mismatch triggers the explicitly configured
`snapshots` and then `trace` commands. Their arguments can use `{first_return}`
with existing launcher options such as `--ram-snapshot-frame`. Configure bounded
CPU/device windows explicitly with the launcher's supported options. The controller
does not invent clock conversions, patch a source observer, or bypass checkpoint
guards. In particular, the full Biohazard profile cannot capture/resume checkpoints,
and this upstream integration does not include the separate TAS state format.

Top-level `snapshots: {"source": "...", "native": "..."}` optionally selects the
diagnostic directories; otherwise the original comparison pair is used. The first
discrepant return's `ram-frame-NNNNNN.bin` files are checked for exact 2 MiB size,
validated against their own page index, and compared byte for byte. Missing files
are reported as unavailable. For full per-page and byte-range detail, the existing
`compare_ram_pages.py` remains available as an explicitly budgeted diagnostic step.

Top-level `trace` optionally compares TSVs with headers:

```json
{
  "columns": ["cycle", "pc", "value"],
  "expected_operations": 3,
  "source": {"path": "{run}/trace/source/trace.tsv", "phase": "pre-fetch", "window": "return-{first_return}", "alignment": "sequence 1"},
  "native": {"path": "{run}/trace/native/trace.tsv", "phase": "pre-fetch", "window": "return-{first_return}", "alignment": "sequence 1"}
}
```

Phase, window and alignment must be explicitly declared, nonempty and equal.
They are operator-supplied assertions, not inferred equivalence. Columns are
compared literally, in order, through all rows; there is no row skipping or
resynchronization. Adapt heterogeneous raw formats with an explicit bounded
diagnostic command, retain both raw inputs, and bind that adapter in `identity`.
An unaligned source CPU hook and a native after-instruction observer must not
be labeled as the same phase. Missing, truncated, malformed or differently phased
traces cannot establish automatic causal diagnosis. A differing observable
operation identifies only the first difference in the configured trace fields.

Optional `milestones` entries have `claim` and `evidence` (an existing file path).
They are reported separately as externally established claims with file hashes.
Without them, gameplay status is unknown, regardless of matching returns.

## Recovery, pause and stop

```powershell
python tools/tasreplays/investigate.py pause D:/psx-investigation-run
python tools/tasreplays/investigate.py resume D:/psx-investigation-run
python tools/tasreplays/investigate.py stop D:/psx-investigation-run
```

Pause is a boundary: the active worker can finish within its budget, while no
new step starts. Resume removes the pause request and collects completed work.
Stop requests active termination and prevents future commands. Stopped, failed
and completed investigations are terminal; use a fresh run for new work.
If the controller is still running, its exclusive OS lock rejects a second
controller; retry resume after it has exited. These commands create no service.

Rerunning `run` with the same recipe/directory also recovers an interrupted
controller. Workers survive controller exit and retain their own bounded result.
Launch intent is written before process creation. If an interruption leaves no
terminal worker receipt after timeout plus 45 seconds of cleanup/startup allowance,
the status becomes `uncertain`. It never retries that attempt automatically.
Inspect the process and retained files before choosing a fresh run. No PID-only
test is used to assume a process belongs to this investigation.

The original recipe and its directory, copied identities, command argv,
environment, logs, attempts, and results remain on disk. `progress.json` is
atomically updated and includes a controller heartbeat, current step and outcome
history. `report.txt` is the concise human entry point. `conclusion.json` retains
the terminal machine-readable verdict. `evidence.json` seals
settled files by SHA-256 without deleting partial output. These hashes detect
changes; they do not impose filesystem write protection. Preserve the directory.
The controller does not reconstruct a missing receipt or rerun completed steps.
