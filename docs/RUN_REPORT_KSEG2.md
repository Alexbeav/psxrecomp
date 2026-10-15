# KSEG2 ignored-access counters in the run report

Every run writes `psx_last_run_report.json` next to the exe when it exits.
The `kseg2_ignored` object in it counts guest accesses that memory.c dropped
because they fall in KSEG2 (0xC0000000 and up), where only the cache control
register at 0xFFFE0130 exists:

```json
"kseg2_ignored": {"reads": 0, "writes": 0}
```

| Field | Meaning |
| --- | --- |
| `reads` | Byte, half and word loads at KSEG2 that returned 0. The cache control register read is not counted. |
| `writes` | Byte, half and word stores at KSEG2 that were ignored. |

Both counters are per process and not part of a save state. A title is
expected to read 0 and 0. A non-zero `reads` means the game loads from KSEG2
and gets 0 (since PS1B-506 also for inline word and half loads, which once
read the RAM mirror). The value is `null` only if the text did not fit its
buffer. The emulator reads neither counter to decide anything.
