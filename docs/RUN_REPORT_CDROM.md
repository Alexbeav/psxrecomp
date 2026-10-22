# CD read counters in the run report

Every run writes `psx_last_run_report.json` next to the exe when it exits.
The `cdrom` object in it counts the read streams of that run:

```json
"cdrom": {"read_starts": 16, "silent_reads": 1, "silent_run_max": 1,
          "retry_dither": 0, "retry_dither_fires": []}
```

| Field | Meaning |
| --- | --- |
| `read_starts` | ReadN and ReadS commands that began a new read stream. |
| `silent_reads` | Streams that ended (Pause, Stop, or the next read start) before one sector was read off the disc. |
| `silent_run_max` | The longest run of silent streams in a row. A stream that reads a sector ends the run. |
| `retry_dither` | How often the retry dither added a first-sector delay (see below). |
| `retry_dither_fires` | The first 32 of those fires: the frame, the read target (LBA), the re-issue count of the run and the delay in CPU cycles. |

A stream that is still open when the report is written is not counted.

## What to look for

A game's CD library restarts a read that delivers nothing for about a second.
One or two silent reads in a row are normal: the first read after a Stop is
cancelled once while the motor spins up. A long run means the game restarts
the same read for ever and never gets a sector.

| `silent_run_max` | Reading |
| --- | --- |
| 0 to 2 | normal |
| 3 or more | look at the title: a read does not deliver in time |
| tens or hundreds | the game is stuck in a read retry loop |

Time Crisis stayed on its logo with 191 silent reads in a row before the
motor rule (behaviour spec 6.11, PS1B-317). With the rule it shows 1.
Street Fighter EX Plus Alpha showed 134 on a source-profile route (the rule
did not yet apply to source profiles, PS1G-120); with it the same route shows 1.

## Fleet check

Run each title headless long enough to pass its first loads, for example
`PSX_EXIT_AFTER_MS=120000` with `--headless`, and read the three numbers from
each report. The counters are per process and are not part of a save state;
after a state load they can be off by one.

The numbers are diagnostics. The drive reads none of them to decide anything.

## Retry dither (PS1G-140)

Some games re-issue the same read in a short loop: the drive delivers the
target sector and a few after it, the game pauses, seeks back and reads again.
The drive's latencies are constant, so every attempt lands at the same phase
against vblank, and a game that loses a race at that phase loops for minutes
(Resident Evil 3, RPD stairwell). From the third such re-issue in a row the
drive adds a delay of 0 to 1 sector period before the first sector, hashed
from the cycle count, so it is the same in every run.

A re-issue counts when it reads the same target as the attempt before, that
attempt reached the target, and it stopped within 8 sectors past it. The
drive's last delivered sector is not cleared per attempt, so once the run has
reached the target, an attempt that delivers nothing still counts. A read of
another target, or one that ran further, ends the run. The run (target and
count) is part of the save state; the two report fields are not. The dither is
off in the source-clock profile and for XA streams.

`retry_dither` and `retry_dither_fires` show whether a run met such a loop.
They are 0 and empty for a run that met none, and its timing is unchanged.
The Resident Evil 3 replay (PS1G-140) shows four fires, two in a loop at
LBA 36916 and two in the stairwell loop:

```json
"retry_dither": 4, "retry_dither_fires": [
  {"frame": 105962, "target": 36916, "run": 3, "cycles": 52000},
  {"frame": 105966, "target": 36916, "run": 4, "cycles": 204030},
  {"frame": 123023, "target": 33485, "run": 3, "cycles": 14265},
  {"frame": 123027, "target": 33485, "run": 4, "cycles": 72125}]
```

To turn the dither off, set `PSX_CD_RETRY_DITHER=0` in the environment before
the start. Use it to compare a run with and without the dither, or to play a
replay recorded before the dither existed that meets such a loop.
