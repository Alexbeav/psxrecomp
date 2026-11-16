# Restricted test data

Clean-room implementers must not open, decode or search any file in this
directory. Exclude `runtime/tests/restricted/` from every recursive search,
for example `rg --glob '!**/restricted/**'`.

| File | Used by | Holds |
|---|---|---|
| `mdec_source_dma_reference.json` | `runtime/tests/test_mdec_source_contracts.py` (dma kind) | The oracle output rows behind `mdec_source_dma_fixtures.json`, compared per field under SPEC-PS1B-186 amendment 4 |

The test reads these files itself. Run it black-box, and do not set
`PSX_MDEC_CONTRACT_SHOW_VALUES` in a clean-room session.
