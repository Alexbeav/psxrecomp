"""Locate toolkit roots (templates, CI helpers) relative to this package."""

from __future__ import annotations

from pathlib import Path


def toolkit_dir() -> Path:
    """tools/new_project_layout/"""
    return Path(__file__).resolve().parent.parent


def templates_dir() -> Path:
    return toolkit_dir() / "templates"


def psxrecomp_root_from_toolkit() -> Path | None:
    """If this toolkit lives inside a psxrecomp checkout, return that root."""
    # …/psxrecomp/tools/new_project_layout/project_studio
    candidate = toolkit_dir().parent.parent
    if (candidate / "runtime" / "runtime.cmake").is_file():
        return candidate
    return None


CI_RELEASE_TEMPLATE = "game-release.yml"


def ci_release_template(game_root: Path | None = None) -> Path | None:
    """The bundled-release workflow template (docs/ci/templates/game-release.yml).

    Prefer the game's own psxrecomp submodule so the workflow matches the
    framework the project pins; fall back to the toolkit's parent psxrecomp.
    A submodule pinned before bundled releases has no such template, and the
    caller says so rather than silently installing the retired setup-host one.
    """
    if game_root is not None:
        for sub in ("psxrecomp", "psxrecomp-v4"):
            p = game_root / sub / "docs" / "ci" / "templates" / CI_RELEASE_TEMPLATE
            if p.is_file():
                return p
    root = psxrecomp_root_from_toolkit()
    if root is None:
        return None
    p = root / "docs" / "ci" / "templates" / CI_RELEASE_TEMPLATE
    return p if p.is_file() else None


# Retired name, kept so a caller pinned to it still resolves.
ci_setup_release_template = ci_release_template
