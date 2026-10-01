# CD read counters in the run report

Every run writes `psx_last_run_report.json` next to the exe when it exits.
The `cdrom` object in it counts the read streams of that run:

```json
"cdrom": {"read_starts": 16, "silent_reads": 1, "silent_run_max": 1}
```

| Field | Meaning |
| --- | --- |
| `read_starts` | ReadN and ReadS commands that began a new read stream. |
| `silent_reads` | Streams that ended (Pause, Stop, or the next read start) before one sector was read off the disc. |
| `silent_run_max` | The longest run of silent streams in a row. A stream that reads a sector ends the run. |

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

## Fleet check

Run each title headless long enough to pass its first loads, for example
`PSX_EXIT_AFTER_MS=120000` with `--headless`, and read the three numbers from
each report. The counters are per process and are not part of a save state;
after a state load they can be off by one.

The numbers are diagnostics. The drive reads none of them to decide anything.
