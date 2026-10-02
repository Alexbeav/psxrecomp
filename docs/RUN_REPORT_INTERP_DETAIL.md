# Interpreted code in the run report

Every run writes `psx_last_run_report.json` next to the exe when it exits.
`overlay_loader.disp_interp_overlay` in it is one number: the dispatches above
the kernel window for which the overlay loader had no native unit. The
`interp_detail` object says what stands behind that number. A product has no
debug server, so this is the only place where a start says it.

```json
"interp_detail": {
    "text_guard": {"armed": 1, "lo": "0x00010000", "hi": "0x000AE800", "foreign_pages": 0,
                   "native_blocked": 1204, "diverged_pages": 0, "exact_mismatches": 1204,
                   "last_mismatch": {"range": "0x00064D80", "len": 844, "at": "0x00065070",
                                     "live_byte": 33, "image_byte": 37}},
    "ranges": {"kernel_end": "0x00010000", "text_lo": "0x00010000", "text_hi": "0x000AE800"},
    "interpreter": {"blocks_run": 71132060, "insns_run": 912345678, "aborts": 0,
                    "guard_yields": 12, "native_handoffs": 40311},
    "per_address": {"table_size": 262144, "addresses": 2210,
                    "by_place": {"kernel": {"addresses": 14, "hits": 3849, "entries": 3849},
                                 "text": {"addresses": 2196, "hits": 71128211, "entries": 880231},
                                 "overlay": {"addresses": 0, "hits": 0, "entries": 0}},
                    "hottest_max": 32, "hottest": [
      {"pc": "0x80064D80", "place": "text", "hits": 2364000, "entries": 2364000, "insns": 40188000,
       "last_caller_ra": "0x80064164", "unit_crc": "0x53132191", "unit_valid": 0}
    ]},
    "loader_miss": {"above_kernel": {"miss_cached": 70100000, "no_unit": 12, "stale_bytes": 1028199,
                                     "outside_window": 0, "device_touch": 0, "native_off": 0,
                                     "diff_gate": 0, "rank": 0, "bad_entry": 0},
                    "kernel": {"miss_cached": 3800, "no_unit": 49, "stale_bytes": 0,
                               "outside_window": 0, "device_touch": 0, "native_off": 0,
                               "diff_gate": 0, "rank": 0, "bad_entry": 0}}
}
```

The numbers above show the form; they are not a measurement.

## text_guard

The text image guard compares the game's own code in RAM with the boot
executable before a statically compiled function runs.

| Field | Meaning |
| --- | --- |
| `armed` | 1 when the guard has its reference image. 0 means that no statically compiled game function runs at all: every one is refused, and the game's own code is interpreted. |
| `lo`, `hi` | The physical range of the reference image. |
| `foreign_pages` | Pages where the loaded executable differs from the one the build was generated from. Code on them runs from RAM. |
| `native_blocked` | How often the guard refused a static function. |
| `diverged_pages` | Pages marked as changed for good by the page-level check. |
| `exact_mismatches` | How often a function's own byte ranges differed from the image. |
| `last_mismatch` | The last such case: the range, the first differing address, the byte in RAM and the byte in the image. |

## ranges and place

Each address has a place: `kernel` below `kernel_end`, `text` from `text_lo`
to `text_hi` (the boot executable), `overlay` for the rest of RAM. A boot
executable that loads high has overlay RAM on both sides.

## interpreter

Totals of the RAM interpreter: blocks and instructions run, aborts on an
unsupported instruction, long loops that yielded, and hand-backs to native
code.

## per_address

The interpreter keeps one row per block entry address it ran. `addresses` is
the number of rows in use; `by_place` sums them per place. `hottest` lists the
`hottest_max` rows with the most hits, the hottest first.

| Field | Meaning |
| --- | --- |
| `hits` | Times the interpreter started a block at this address. |
| `entries` | Of those, the times it arrived from native code or the dispatch loop, not from the interpreter's own block chaining. |
| `insns` | Instructions run from this address. |
| `last_caller_ra` | The guest return address at the last such arrival: who called it. |
| `unit_crc`, `unit_valid` | The compiled overlay unit whose ranges cover the address at that time, and whether its code matched. 0 and 0: no unit covers it. A CRC with `unit_valid` 0: a unit exists, but its code is not the code in RAM. |

## loader_miss

`overlay_loader.disp_interp` by reason, above the kernel window and inside it.
The reasons of one place add up to that place's share of `disp_interp`.

| Reason | Meaning |
| --- | --- |
| `miss_cached` | The loader already knew that it has nothing valid for this address since the last change of code. The first miss is counted under its own reason. |
| `no_unit` | No compiled unit covers the address. |
| `stale_bytes` | A unit covers it, but none matches the code bytes in RAM. |
| `outside_window` | Clean boot text, or another address the loader does not serve. |
| `device_touch` | The unit matches but touches a device; such a unit never runs natively. |
| `native_off` | Native execution is off, or this function is blocked. |
| `diff_gate` | Held back by the native-against-interpreter comparison (development). |
| `rank` | Held back by the native rank filter (debug tools only). |
| `bad_entry` | The unit ran and refused a foreign interior entry. |

`disp_interp` counts loader lookups that found no native unit. The interpreter
then may still decline the target, so `per_address` is the record of what was
interpreted.

## Cost

The object is built when the report is written. During play the added work
is one counter increment at each exit of the loader's dispatch that ends in
the interpreter, and one local variable set to 0 in that function. A native
dispatch passes none of the increments.
