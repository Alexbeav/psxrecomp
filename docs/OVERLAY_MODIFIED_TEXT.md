# Units for game text the game rewrites (PS1B-421)

A game can replace code inside its own boot executable's text range. How the
bytes arrive decides which bit the page gets:

- by a load path (CD DMA, a data shard): the page is marked executable and is
  **dirty**. It is in the capture window (`overlay_cache_window_contains`).
- by ordinary CPU stores: the page is **text_modified** and not dirty
  (`memory.c`, `text_guard_note_write`). It stays outside the capture window.

In both cases the static function for the old bytes is refused by the text
guard, and the code runs in the interpreter until a unit takes over. The
periodic capture goes by the addresses the interpreter executed, so it builds a
unit for either kind of page.

## The lazy load's window

`overlay_loader_dispatch` looks for a cached unit (the lazy load) for an
address in the capture window and, since PS1B-421, for an address in a
text_modified page when that address is an exact entry of a cached unit. The
unit loads only when its recorded crc equals the live bytes. Nothing marks the
page dirty.

A store into a loaded unit's code range raises the page's generation; the next
dispatch re-hashes and takes the unit out when the bytes no longer match. The
old code's call-return addresses are not entries of the new unit: they stay
with the interpreter, and under continuation passing they fail closed as
foreign interior entries.

## A page that keeps changing

Each time a valid unit stops matching, every text_modified page its code
ranges touch counts one take-out. After 32 take-outs a page is left to the
interpreter, loaded units included. The count does not decay and is not reset:
it holds for the rest of the process, across a rematch or a soft return to
the launcher too. Each dispatch such a page costs is counted in the run
report under the miss reason `modified_text_backoff`
(`docs/RUN_REPORT_INTERP_DETAIL.md`). A game that re-patches code every frame is stopped in about half a
second; a game that re-patches at a change of scene keeps its units for its
first 32 changes on that page and then runs as it did before PS1B-421.

A page that becomes dirty later (a load path writes it) is in the capture
window from then on, and the limit no longer applies to it.

## A page with many units

The take-out limit counts units that stop matching. A game whose capture keeps
making new variants of the same code has none of those: every unit stays
valid, and the cost comes with the loads. A lazy load is made on the
emulation thread and waits for it. V-Rally 2 (USA), seven five-minute starts
on one cache, without this bound: 11, 73, 132, 170, 276, 282 and 291 loads
through the wider window in a start, and from the third start on the frame
rate was below the unchanged build's and fell further with each start.

Each page therefore gets 8 loads through the wider window in a process. A
load counts at the page of the dispatch that asked for it. When a page has had
its 8, no further unit is loaded for it. The units already loaded keep
running, and they come back when their bytes come back. A dispatch of an
exact entry on such a page that no loaded unit serves is counted in the run
report under the miss reason `modified_text_load_bound`. The count is not
reset for the rest of the process.

8 is from those rows: V-Rally 2 was level with the unchanged build at 11 and
at 73 loads over its eight draw pages and behind it at 132. Digimon World 2
needs 2 loads over three pages. `PSX_OVERLAY_MODTEXT_LOAD_LIMIT` (1 to 255)
sets another limit for a test or a measurement; a product does not set it.

## In the run report

```json
"overlay_modified_text": {
  "loads": 2,
  "load_ms": 31,
  "load_bound_pages": 0,
  "load_limit": 8,
  "taken_out": 0,
  "backed_off_pages": 0,
  "backoff_limit": 32
},
"overlay_loader_cost": {
  "load_ms": 212,
  "longest_load_ms": 19,
  "rehashes": 5120,
  "rehash_misses": 14,
  "dispatches_without_rehash": 48211907
}
```

`overlay_modified_text`:

- `loads`: units loaded through the wider window.
- `load_ms`: the time those loads took, on the emulation thread. Measured on
  Windows only; 0 on Linux and macOS.
- `load_bound_pages`: pages that reached the limit of loads.
- `load_limit`: the limit of loads for a page in this process.
- `taken_out`: times a valid unit touching such a page stopped matching.
- `backed_off_pages`: pages that reached the limit of take-outs.
- `backoff_limit`: the limit of take-outs of this build.

`overlay_loader_cost` is for every unit of the start, not only those of
rewritten text. It is what the loader's own work cost:

- `load_ms`, `longest_load_ms`: the time of all unit loads and of the longest
  one. Windows only; 0 elsewhere.
- `rehashes`: dispatches that had to hash a unit's code ranges again, because
  a store reached a page the unit covers. `rehash_misses`: those that found
  the bytes changed.
- `dispatches_without_rehash`: dispatches that ran a unit without a hash.

Together with `overlay_compile` (the background compile work) and the
registered and valid counts of `overlay_loader`, one start shows where a
title with many units pays: in the loads, in the hashes, or in the compiler.

## A cache written before PS1B-421

Units for rewritten text were already built and stored; they were never
loaded. After the change they are found by the same manifest and load only on
an exact match of the live bytes, under the cache's own tag (ABI, codegen
version and hash, config hash). Nothing in such a unit is stale by its age: a
unit whose bytes are not the live bytes does not load, in an old cache as in a
new one.
