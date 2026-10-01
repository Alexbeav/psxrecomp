#!/usr/bin/env python3
"""migrate_bundled_release.py -- move an EXISTING PSX title onto bundled
releases: committed generated/ game C, compiled-in BIOS backends, and a
release.yml that builds and ships the compiled game (docs/ci/BUNDLED_RELEASES.md).

    python3 psxrecomp/tools/migrate_bundled_release.py                 # cwd is the title
    python3 psxrecomp/tools/migrate_bundled_release.py ~/src/MyGameRecomp --dry-run
    python3 psxrecomp/tools/migrate_bundled_release.py . --disc disc/Game.cue --push

What it does to the title, in order (each step is reported):
  1. bumps the psxrecomp (and recomp-ui) submodule to --psxrecomp-ref /
     --recomp-ui-ref (default origin/master) and refuses a pin that lacks
     docs/ci/templates/game-release.yml or the committed BIOS backends
  2. un-ignores generated/, ensures [runtime] overlay_cache = true
  3. writes scripts/package_release.sh (retiring package_setup_release.sh)
     and .github/workflows/release.yml from the pinned framework's template
  4. rebuilds the emitters and regenerates the game C from the title's disc
     (--disc, else disc.cfg, else the single disc/*.cue); --skip-generate
     defers that to you, and the release gate will refuse until it is done
  5. refreshes framework_pins.txt, runs the CI gates locally, commits;
     --push also pushes

This is the thin face of Project Studio's migrate_bundled operation
(tools/new_project_layout/project_studio/migrate_bundled.py), which is also
what `project_studio git migrate-bundled` / `git bulk-migrate-bundled` and
Retro Studio's "Bulk migrate" tab run. The title's working tree must be
clean; the pins are moved inside the submodules, so stash any WIP there first.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE / "new_project_layout"))

from project_studio.migrate_bundled import (  # noqa: E402
    BundledMigrateOptions,
    migrate_to_bundled_release,
)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", nargs="?", default=".", help="Title repository root (default: cwd)")
    ap.add_argument("--psxrecomp-ref", default="origin/master")
    ap.add_argument("--recomp-ui-ref", default="origin/master")
    ap.add_argument("--disc", default="", help="Disc image for generate")
    ap.add_argument("--bios", default="", help="BIOS dump for generate (default: bundled OpenBIOS)")
    ap.add_argument("--zip-prefix", default="", help="CI zip prefix (default: keep the existing packager's)")
    ap.add_argument("--skip-generate", action="store_true", help="Do not regenerate game C")
    ap.add_argument("--push", action="store_true", help="Push after committing")
    ap.add_argument("--dry-run", action="store_true", help="Report what would change; write nothing")
    args = ap.parse_args(argv)

    opts = BundledMigrateOptions(
        psxrecomp_ref=args.psxrecomp_ref,
        recomp_ui_ref=args.recomp_ui_ref,
        regenerate=not args.skip_generate,
        disc=args.disc or None,
        bios=args.bios or None,
        zip_prefix=args.zip_prefix or None,
        push_remote=args.push,
        dry_run=args.dry_run,
    )
    r = migrate_to_bundled_release(Path(args.root), opts)
    if r.detail:
        for ln in r.detail.splitlines():
            print(f"  {ln}")
    print(f"[{'OK' if r.ok else 'FAIL'}] {r.message}")
    return 0 if r.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
