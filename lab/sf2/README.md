# SF2 lab workspace

Only source-owned metadata belongs in this directory.

- `reference-manifest.toml` records the supported executable identity and the
  comparison baseline.
- `local/`, `generated/`, `captures/`, and `traces/` are ignored and may hold
  private local experiment artifacts.
- Generated C and overlay captures are derived from retail code and must not be
  committed or redistributed.

The first generated project should use Disc 1. Add Disc 2 only after the
resident executable identity is confirmed and the overlay capture strategy is
explicitly documented.
