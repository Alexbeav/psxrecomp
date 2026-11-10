# Authored MDEC clean decode fixtures

`runtime/tests/data/mdec_clean/manifest.json` binds the retained PS1B-188
M1–M5, M10, M11, M12 and M13 TSV inputs by SHA-256. They contain 721 authored
cases or blocks, not game, BIOS or movie data. Their original TSV provenance
and expected bytes are retained unchanged from accepted `ac56b624`.

`test_mdec_clean_fixtures.py --library <probe> --entry-path source` checks the
production optional scalar callback and its table-loading seam. The registered
O0/O2 tests select this path explicitly. The probe also exposes the modern
default path; `--entry-path default` or `both` can compare it with this older
contract. That comparison is diagnostic: the modern decoder differs on 389
rows in the continuation baseline. It is not silently changed to fit the old
contract, and source-only success is not reported as default decoder parity.

The optional source callback passes all 721 rows at O0 and O2. Complete FIFO
and DMA timing/restore transcripts are separate tests described in
[the source profile](../source-mdec-profile.md). None of these component results
establish retail playback, source-system arbitration or hardware parity.
