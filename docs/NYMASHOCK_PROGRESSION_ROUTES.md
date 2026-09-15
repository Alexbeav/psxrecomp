# Bio Hazard progression-save routes

The completed original227,202-input TAS leaves no allocated saved-game entries
on its card. Progression saves therefore require a separate controller route;
they cannot be inferred from TAS completion or synthesized from emulator RAM.

`tools/tasreplays/nymashock_progression.py` prepares an independently licensed
stock Nymashock2.9.1 host for that work. It requires an admitted full source
reference. `build-helper REFERENCE OUTPUT` compiles the read-only
`Progression291.cs` observer against the reference host's exact managed files.
The source TAS helper and stock release files remain untouched.

Two session starts are supported:

- `run --reference REF --helper-build RECEIPT --output NEW --prefix N` cold-boots
  the original blank-card movie, executes exactly its firstN original inputs,
  then stops playback without saving the movie and pauses for authored controls.
- `run --reference REF --helper-build RECEIPT --output NEW --card CARD` cold-boots
  without a movie and lets the stock frontend load a fresh writable copy of that
  raw128KiB card. The helper hashes the actual loaded card before the first frame.

The distinction follows stock `MainForm.cs`: a queued movie intentionally skips
the ordinary SaveRAM load. A progression-card load must therefore use the second
form. Original-prefix sessions reject a supplied card.

While paused, inspect `step-000000.png` and the corresponding read-only card/RAM
capture. Submit bounded input with `step RUN FRAMES --press up cross` (for example),
or provide four byte axes with `--axes LY LX RY RX`. Each command is a fresh,
numbered data file, with no executable Lua content. Wait for its matching
`step-NNNNNN.json` and image before authoring the next command. `finish RUN`
closes the evidence, exits, and exports the complete observed controller input
to PSXRTI2. Paused wall-clock time does not advance guest frames.

All console events, physical Analog presses, card/state injection, input
retiming and original-movie recording are excluded. Full RAM digests,512page
hashes and the public master clock remain captured at every return. Commands
are bounded to12,000frames each,300,000total returns and4,096steps; the exported
route additionally enforces the native codec's complete-state step limit.
The caller reserves storage before launching; the session enforces3GiB,
5,000files and its declared host timeout. Only one source emulator may run.

An exported card is merely a read-only snapshot of bytes written by the game.
For each of four to eight selected progression points, retain the source route,
image-bound area/inventory/health/flag observations, raw-card provenance and
hash. Cold-boot independent writable copies in source and native, load through
the game, perform an authored gameplay route, then save and reload again.
Whole-RAM equality after a fresh load requires equivalent boundaries; otherwise
use explicit semantic observations, as required by the campaign acceptance gates.

This tool is prepared infrastructure until its actual source prefix, card load,
authored controller delivery and save/reload executions have been qualified.
Compilation and parser tests do not claim a progression-save pass.
