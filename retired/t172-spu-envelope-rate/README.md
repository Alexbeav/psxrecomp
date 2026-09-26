# Retired: T172 SPU envelope rate candidate

These files are out of the build. Nothing compiles, includes, or tests them.

| File | Was |
|---|---|
| `spu_envelope_rate.h` | `runtime/include/spu_envelope_rate.h` |
| `test_spu_envelope_rate.c` | `runtime/tests/test_spu_envelope_rate.c` (CTest `spu_envelope_rate_test`) |
| `spu_envelope_rate_provenance.json` | `runtime/tests/spu_envelope_rate_provenance.json` |

They were the first T172 SPU envelope replacement: 02a69607, 4610be6c, 16041d65 and 9f1cfafb (2026-09-19). PS1B-192 replaced them with the envelope unit fitted to oracle fixtures, and `runtime/src/spu.c` no longer uses them.

They are kept, not deleted, so the provenance record and the history of the replacement stay with the source. Do not include them from runtime code. A future change that needs envelope timing uses the PS1B-192 unit.
