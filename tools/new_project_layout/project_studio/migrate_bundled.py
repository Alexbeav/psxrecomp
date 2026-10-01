"""Move an existing PSX title onto bundled releases (committed generated/ C,
compiled-in BIOS backends, compiled game shipped).

A title that predates 2026-09-30 carries the setup-host shape: a release.yml
that wipes generated/ and ships an uncompiled kit, scripts/
package_setup_release.sh, a .gitignore that ignores generated/, and a
psxrecomp pin whose template and BIOS backends are gone. This module turns it
into the bundled shape in one pass, as the CLI (`migrate-bundled`), the bulk
op (`bulk-migrate-bundled`) and retcomm-studio's bulk tab all run it:

  1. bump the psxrecomp submodule (and recomp-ui, which the new runtime
     needs) to a ref that carries docs/ci/templates/game-release.yml and
     the committed BIOS backends; refuse a pin that does not
  2. un-ignore generated/ (merge_gitignore), ensure [runtime]
     overlay_cache = true (the packager refuses to ship without it)
  3. emit scripts/package_release.sh, retire package_setup_release.sh,
     emit .github/workflows/release.yml from the new template
  4. regenerate the game C against the new pin from the title's disc
     (build_emitters.sh + psxrecomp_cli.py generate) -- an emitter bump
     changes codegen, so C generated under the old pin must not be committed
  5. refresh framework_pins.txt, run the CI gates locally
     (check_boot_exe.sh, check_generated.sh), stage, commit, optionally push

Everything is reported per step in CmdResult.detail so a bulk run over many
titles reads as a ledger. Dry-run performs no git mutation and no generate.
"""

from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .gitops import CmdResult, _git, _is_git_repo, ensure_actions_registers_release_yml, push
from .models import MigrateOptions

PACKAGER_WRAPPER = "package_release.sh"
LEGACY_PACKAGER_WRAPPER = "package_setup_release.sh"


@dataclass
class BundledMigrateOptions:
    psxrecomp_ref: str = "origin/master"
    recomp_ui_ref: str = "origin/master"
    regenerate: bool = True
    disc: str | None = None
    bios: str | None = None
    zip_prefix: str | None = None
    push_remote: bool = False
    dry_run: bool = False
    extra_notes: list[str] = field(default_factory=list)


def _run(cmd: list[str], cwd: Path, *, env: dict | None = None) -> tuple[int, str]:
    r = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env, check=False)
    return r.returncode, (r.stdout + r.stderr)


def zip_prefix_from_wrapper(root: Path) -> str:
    for name in (PACKAGER_WRAPPER, LEGACY_PACKAGER_WRAPPER):
        try:
            text = (root / "scripts" / name).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        m = re.search(r"--zip-prefix\s+([^\s\\'\"]+)", text)
        if m:
            return m.group(1)
    return ""


def find_disc(root: Path, explicit: str | None) -> Path | None:
    """The title's disc image: --disc, else disc.cfg, else the one disc/*.cue."""
    if explicit:
        p = Path(explicit).expanduser()
        if not p.is_absolute():
            p = root / p
        return p if p.is_file() else None
    cfg = root / "disc.cfg"
    if cfg.is_file():
        line = cfg.read_text(encoding="utf-8", errors="replace").strip().splitlines()
        if line:
            p = Path(line[0].strip())
            if not p.is_absolute():
                p = root / p
            if p.is_file():
                return p
    cues = sorted(glob.glob(str(root / "disc" / "*.cue")))
    if len(cues) == 1:
        return Path(cues[0])
    return None


def ensure_overlay_cache_key(root: Path, dry_run: bool) -> tuple[bool, str]:
    """Add `overlay_cache = true` under [runtime] when absent. Returns (changed, note)."""
    p = root / "game.toml"
    if not p.is_file():
        return False, "game.toml missing"
    text = p.read_text(encoding="utf-8", errors="replace")
    if re.search(r"^\s*overlay_cache\s*=\s*true", text, re.M):
        return False, "overlay_cache = true already set"
    if re.search(r"^\s*overlay_cache\s*=\s*false", text, re.M):
        return False, "overlay_cache = false is set explicitly; left alone (the packager will need --ship-without-overlay-cache-key-because)"
    block = ("# Enable the overlay loader: the shipped static shard / bundled overlay\n"
             "# toolchain turn streamed overlays native instead of running them interpreted.\n"
             "overlay_cache = true\n")
    m = re.search(r"^\[runtime\]\s*$", text, re.M)
    if m:
        insert_at = m.end()
        # keep the section header's newline
        nl = text.find("\n", insert_at)
        insert_at = len(text) if nl < 0 else nl + 1
        new = text[:insert_at] + block + text[insert_at:]
    else:
        new = text.rstrip("\n") + "\n\n[runtime]\n" + block
    if not dry_run:
        p.write_text(new, encoding="utf-8")
    return True, "added [runtime] overlay_cache = true"


def _bump_submodule(root: Path, name: str, ref: str, dry_run: bool) -> tuple[bool, str, str]:
    """Fetch and check out `ref` in submodule `name`. Returns (ok, sha, note)."""
    sub = root / name
    if not (sub / ".git").exists():
        code, out = _run(["git", "submodule", "update", "--init", "--", name], root)
        if code != 0:
            return False, "", f"{name}: submodule init failed: {out.strip()[-300:]}"
    code, out = _run(["git", "fetch", "-q", "origin"], sub)
    if code != 0:
        return False, "", f"{name}: fetch failed: {out.strip()[-300:]}"
    code, sha = _run(["git", "rev-parse", "--verify", f"{ref}^{{commit}}"], sub)
    if code != 0:
        return False, "", f"{name}: ref {ref!r} not found after fetch"
    sha = sha.strip()
    code, cur = _run(["git", "rev-parse", "HEAD"], sub)
    cur = cur.strip()
    if cur == sha:
        return True, sha, f"{name}: already at {sha[:10]}"
    if dry_run:
        return True, sha, f"{name}: would move {cur[:10]} -> {sha[:10]}"
    code, out = _run(["git", "status", "--porcelain"], sub)
    if out.strip():
        return False, sha, f"{name}: working tree has local changes; commit or stash them first:\n{out.strip()[:400]}"
    code, out = _run(["git", "checkout", "-q", sha], sub)
    if code != 0:
        return False, sha, f"{name}: checkout {sha[:10]} failed: {out.strip()[-300:]}"
    code, out = _run(["git", "submodule", "update", "--init", "--recursive"], sub)
    if code != 0:
        return False, sha, f"{name}: nested submodule update failed: {out.strip()[-300:]}"
    return True, sha, f"{name}: {cur[:10]} -> {sha[:10]}"


def migrate_to_bundled_release(root: Path, opts: BundledMigrateOptions) -> CmdResult:
    root = Path(root).expanduser().resolve()
    log: list[str] = []

    def note(s: str) -> None:
        log.append(s)

    def fail(msg: str) -> CmdResult:
        return CmdResult(False, msg, "\n".join(log))

    if not _is_git_repo(root):
        return fail("not a git repository")
    cmake = root / "CMakeLists.txt"
    if not cmake.is_file() or "psxrecomp_add_game_runtime" not in cmake.read_text(encoding="utf-8", errors="replace"):
        return fail("CMakeLists.txt does not use psxrecomp_add_game_runtime (migrate the layout first: migrate_project.py apply)")
    if not (root / "game.toml").is_file():
        return fail("game.toml missing")
    # Submodule gitlinks are exempt: a caller (Retro Studio's bulk tab) may
    # have moved the psxrecomp pin already so this script exists in-title, and
    # the pins are re-resolved below anyway. Content changes INSIDE a
    # submodule are caught by _bump_submodule.
    code, dirty = _run(["git", "status", "--porcelain", "--untracked-files=no",
                        "--ignore-submodules=all"], root)
    if dirty.strip() and not opts.dry_run:
        return fail("working tree has uncommitted changes; commit or stash them first:\n" + dirty.strip()[:600])

    # 1. framework pins
    ok, fw_sha, msg = _bump_submodule(root, "psxrecomp", opts.psxrecomp_ref, opts.dry_run)
    note(msg)
    if not ok:
        return fail("psxrecomp bump failed")
    fw = root / "psxrecomp"

    def pinned_has(rel: str) -> bool:
        # Ask the object store, not the checkout: dry-run leaves the checkout alone.
        code, _ = _run(["git", "cat-file", "-e", f"{fw_sha}:{rel}"], fw)
        return code == 0

    if not pinned_has("docs/ci/templates/game-release.yml"):
        return fail(f"psxrecomp {fw_sha[:10]} has no docs/ci/templates/game-release.yml; pick a ref at or after bundled releases")
    for stem in ("OpenBIOS", "SCPH1001"):
        if not pinned_has(f"generated/{stem}_dispatch.c"):
            return fail(f"psxrecomp {fw_sha[:10]} does not carry generated/{stem}_dispatch.c; pick a ref that commits its BIOS backends")
    if (root / "recomp-ui").is_dir():
        ok, _ui_sha, msg = _bump_submodule(root, "recomp-ui", opts.recomp_ui_ref, opts.dry_run)
        note(msg)
        if not ok:
            return fail("recomp-ui bump failed")

    # Project Studio ops (gitignore / packager / workflow) share the token logic.
    from .ops import op_emit_ci_workflow, op_emit_packager, op_merge_gitignore
    mopts = MigrateOptions(
        zip_prefix=opts.zip_prefix or zip_prefix_from_wrapper(root) or None,
        enable_ci=True, force=True, dry_run=opts.dry_run,
    )

    # 2. gitignore + game.toml
    r = op_merge_gitignore(root, mopts)
    note(f".gitignore: {r.message}")
    if not r.ok:
        return fail("gitignore merge failed")
    gi = root / ".gitignore"
    if gi.is_file():
        text = gi.read_text(encoding="utf-8", errors="replace")
        stale = "# Setup-host: players Generate game C locally (CI clears generated/).\n"
        if stale in text and not opts.dry_run:
            gi.write_text(text.replace(stale, "# generated/ is COMMITTED: releases ship the compiled game built from it.\n"), encoding="utf-8")
    changed, msg = ensure_overlay_cache_key(root, opts.dry_run)
    note(f"game.toml: {msg}")

    # 3. packager + workflow
    r = op_emit_packager(root, mopts)
    note(f"packager: {r.message}")
    if not r.ok:
        return fail("packager emit failed")
    legacy = root / "scripts" / LEGACY_PACKAGER_WRAPPER
    retire_legacy = legacy.is_file()
    r = op_emit_ci_workflow(root, mopts)
    note(f"workflow: {r.message}")
    if not r.ok:
        return fail("workflow emit failed")

    # 4. regenerate against the new pin
    staged_generated = False
    if opts.regenerate:
        disc = find_disc(root, opts.disc)
        if disc is None:
            return fail("no disc image found (pass --disc, or write disc.cfg, or keep exactly one disc/*.cue); "
                        "or pass --skip-generate and regenerate before the first release")
        note(f"disc: {disc}")
        if opts.dry_run:
            note("would run build_emitters.sh + psxrecomp_cli.py generate")
        else:
            code, out = _run(["bash", "psxrecomp/tools/ci/build_emitters.sh"], root)
            if code != 0:
                return fail("build_emitters.sh failed:\n" + out.strip()[-1200:])
            note("emitters built (build-recompiler)")
            cmd = [sys.executable, "psxrecomp/psxrecomp_cli.py", "generate",
                   "--config", "game.toml", "--project-root", ".", "--disc", str(disc)]
            if opts.bios:
                cmd += ["--bios", str(Path(opts.bios).expanduser().resolve())]
            code, out = _run(cmd, root)
            tail = "\n".join(ln for ln in out.splitlines() if "opcode 0x2F" not in ln)[-1500:]
            if code != 0 or "Generate complete" not in out:
                return fail("psxrecomp_cli.py generate failed:\n" + tail)
            note("game C regenerated against the new pin")
            staged_generated = True
    else:
        note("regenerate skipped (--skip-generate): generated/ must be regenerated against this pin before a release")

    # 5. pins snapshot, gates, stage, commit
    if (root / "framework_pins.txt").is_file() and not opts.dry_run:
        code, out = _run(["bash", "psxrecomp/tools/ci/record_pins.sh"], root)
        if code == 0:
            (root / "framework_pins.txt").write_text(out, encoding="utf-8")
            note("framework_pins.txt refreshed")

    if opts.dry_run:
        return CmdResult(True, "dry-run: would migrate to bundled releases", "\n".join(log))

    to_add = [".gitignore", "game.toml", f"scripts/{PACKAGER_WRAPPER}",
              ".github/workflows/release.yml", "psxrecomp"]
    if (root / "recomp-ui").is_dir():
        to_add.append("recomp-ui")
    if (root / "framework_pins.txt").is_file():
        to_add.append("framework_pins.txt")
    if (root / "generated").is_dir():
        to_add.append("generated")
    code, out, err = _git(root, "add", "--", *to_add)
    if code != 0:
        return fail("git add failed: " + (err or out).strip()[-400:])
    if retire_legacy:
        _git(root, "rm", "-q", "--cached", "--", f"scripts/{LEGACY_PACKAGER_WRAPPER}")
        try:
            legacy.unlink()
        except OSError:
            pass
        note(f"retired scripts/{LEGACY_PACKAGER_WRAPPER}")

    for gate in ("check_boot_exe.sh", "check_generated.sh --root ."):
        code, out = _run(["bash", "-c", f"psxrecomp/tools/ci/{gate}"], root)
        if code != 0:
            return fail(f"gate {gate.split()[0]} failed (nothing committed; staged changes left for inspection):\n" + out.strip()[-1200:])
        note(f"gate ok: {gate.split()[0]}")

    code, porcelain, _ = _git(root, "diff", "--cached", "--name-only")
    if not porcelain.strip():
        return CmdResult(True, "already on bundled releases (nothing to commit)", "\n".join(log))
    msg = (f"release: migrate to bundled releases (psxrecomp {fw_sha[:10]})\n\n"
           "Committed generated/ C, compiled-in BIOS backends, compiled game shipped;\n"
           "release.yml and scripts/package_release.sh from the psxrecomp template.\n"
           + ("Game C regenerated against the new pin.\n" if staged_generated else
              "Game C NOT regenerated here (--skip-generate).\n"))
    code, out, err = _git(root, "commit", "-q", "-m", msg)
    if code != 0:
        return fail("git commit failed: " + (err or out).strip()[-400:])
    note(f"committed: {msg.splitlines()[0]}")
    if not opts.push_remote:
        return CmdResult(True, f"migrated to bundled releases (psxrecomp {fw_sha[:10]}); not pushed", "\n".join(log))
    pr = push(root)
    note(pr.message)
    if not pr.ok:
        return CmdResult(False, "committed but push failed", "\n".join(log) + "\n" + pr.detail)
    nudge = ensure_actions_registers_release_yml(root)
    if nudge.message:
        note(nudge.message)
    return CmdResult(True, f"migrated to bundled releases (psxrecomp {fw_sha[:10]}) and pushed", "\n".join(log))
