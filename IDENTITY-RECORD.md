# Artifact and dependency identity record (2026-09-22)

## Merged runtime identity

The integration runtime is a **local merge**, now committed so it has a stable
identity. Not pushed.

| Field | Value |
| --- | --- |
| repository | `<projects>\_local\psxrecomp-merge` (clone of `Alexbeav/psxrecomp`) |
| branch | `integration-test` |
| merge commit | `f3de715436426d3000f4f646d3f464d67b9c4529` |
| tree | `b2aff5070acd438e5eb107874e56742d8668bbc1` |
| parents | `f4751d005ee69b26a69f8fe0d7a58807f16b46c1` + `452cc0c06ec9fb93f28c5848960f7564c76a1ea8` |
| subject | integration: merge SF2 feature branch (452cc0c) onto launcher baseline (f4751d00) |
| source files | 2025 tracked |
| working-tree digest (pre-commit) | `d0a91817e18bdb92ac2f45aeb508feee8ff17a70c4995d036599deda0c761fa8` |

`452cc0c` and `f4751d00` both descend from merge base
`0cfa9fe0a8da944e9f694a24361b4973c57131ea` (2026-08-01) and are **divergent**:
`452cc0c` is 46 commits ahead of the base on a separate SF2 modernization line.

## Dependency set (all required together — enforced by compile-time gates)

| Component | Ref | Enforced by | Held hash |
| --- | --- | --- | --- |
| psxrecomp | `f3de7154` (local merge) | — | this record |
| recomp-ui | `ff92028ec86e30503694c70c532b93b8198663aa` | `runtime.cmake:2022` requires `ff92028`+ for `RecompLauncherCNetplayChatMessage` | — |
| retcomm-rbengine | `a7b98507a62fe00e5aec3b90c52a4134f3c174bc` | `runtime.cmake:559` requires snap-ring when `PSX_REWIND=ON` | — |
| recomp-net | `268e74fe718b38fe38643c358588bbc1e0f0af70` | framework submodule | tree hash in prepare receipts |

Provenance of each dependency ref:

- **recomp-ui `ff92028`** — tip of branch `wave4-posix-tools-20260914` in
  `Alexbeav/recomp-ui`. **Not** an ancestor of that repo's `main`
  (`git merge-base --is-ancestor` fails), and `main` does not contain
  `RecompLauncherCNetplayChatMessage`. The previously pinned `514c9e29` is too old
  for this framework.
- **rbengine `a7b98507`** — `refs/heads/main` of `RetroPortingToolKit/rbengine`.
  It is the submodule gitlink recorded in the framework tree at `lib/retcomm-rbengine`.
  A GitHub source archive never contains it, which is why the earlier kit had none.
- **recomp-net `268e74f`** — unchanged from `framework_pins.txt`.

Submodule gitlinks in the merged tree (verified with `git ls-tree HEAD lib/`):

```
160000 commit 859ba28b93f7374f2ac1eb70d2171594c5cd4f4b  lib/recomp-net
160000 commit a7b98507a62fe00e5aec3b90c52a4134f3c174bc  lib/retcomm-rbengine
```

## The three artifacts, preserved

| Artifact | exe SHA-256 | Role | State |
| --- | --- | --- | --- |
| `PSX-References\_local\sf2-launcher-candidate\FROZEN-working-build` | `2c46e4430401ac9fa299448e49527aee9567ab2a33a89b0b62802174bccd687f` | original working build, mouse-look confirmed, no F1/F7 | untouched all session |
| `_local\sf2-widescreen-candidate\work\kit` | `75c763f138c09d3c949430814701d8804ac92e34885c40b41a31bcd51e10150b` | widescreen comparison candidate | preserved as the comparison build |
| `_local\integration-kit\bld` | `97282fb89d1a0420572baad527dc16f71539fee9a69d323ef2415ab197901bff` | merged runtime, both feature sets | boots, runs, 902 frames at 59.9 fps; screen black |

No commits or pushes in `PSX-References`, `PSX-Ports`, or any published remote. The
merge commit above is local to `psxrecomp-merge` only.

## Environment

| Item | Value |
| --- | --- |
| compiler | `gcc/g++ 16.1.0` (MinGW-W64 x86_64-ucrt-posix-seh, Brecht Sanders r3) |
| GPU / driver | NVIDIA, OpenGL 3.3.0, driver 616.92 |
| panel | 144 Hz, vsync off; guest paced at 59.94 Hz |
| BIOS | `<projects>\PSX-Ports\suikoden-ii\psxrecomp\bios\SCPH1001.BIN` |
| disc | `<projects>\_local\integration-kit\disc\Syphon Filter 2 (USA) (Disc 1).cue` |
