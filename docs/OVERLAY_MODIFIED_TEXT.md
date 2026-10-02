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
interpreter for the rest of the start, loaded units included. The count does
not decay. A game that re-patches code every frame is stopped in about half a
second; a game that re-patches at a change of scene keeps its units for its
first 32 changes on that page and then runs as it did before PS1B-421.

A page that becomes dirty later (a load path writes it) is in the capture
window from then on, and the limit no longer applies to it.

## In the run report

```json
"overlay_modified_text": {
  "loads": 2,
  "taken_out": 0,
  "backed_off_pages": 0,
  "backoff_limit": 32
}
```

- `loads`: units loaded through the wider window.
- `taken_out`: times a valid unit touching such a page stopped matching.
- `backed_off_pages`: pages that reached the limit.
- `backoff_limit`: the limit of this build.

## A cache written before PS1B-421

Units for rewritten text were already built and stored; they were never
loaded. After the change they are found by the same manifest and load only on
an exact match of the live bytes, under the cache's own tag (ABI, codegen
version and hash, config hash). Nothing in such a unit is stale by its age: a
unit whose bytes are not the live bytes does not load, in an old cache as in a
new one.
