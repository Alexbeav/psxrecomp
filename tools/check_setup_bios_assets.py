#!/usr/bin/env python3
"""Require BIOS assets selected by the staged title recipe.

A single-program package has its recipe at the stage root (game.toml). A set
of programs (set.toml, tools/program_set.py) has one recipe per program, in
the program's folder, and every one of them is checked. Their BIOS profile
paths are taken from the stage root: psxrecomp/ is there once, and setup
links it into each program's folder.
"""
import sys
from pathlib import Path
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib


def recipe_assets(stage, recipe):
    """The BIOS files one recipe needs in the stage."""
    with recipe.open("rb") as handle:
        config = tomllib.load(handle)
    openbios = config.get("runtime", {}).get("openbios", True)
    if not isinstance(openbios, bool):
        raise ValueError("runtime.openbios must be a boolean")
    profile = config.get("recompiler", {}).get("bios_config")
    required = []
    if profile:
        required.append(stage / profile)
    elif not openbios:
        # Existing title recipes without a profile use the CLI default.
        required.append(stage / "psxrecomp/bios/SCPH1001.toml")
    if openbios:
        required.extend(stage / "psxrecomp/bios" / name for name in
                        ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"))
    return required


def set_recipes(stage):
    """Each program's recipe of a staged set, in set order."""
    tools = str(Path(__file__).resolve().parent)
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import program_set
    try:
        spec = program_set.load_set(stage / program_set.SET_FILE)
    except program_set.SetError as error:
        raise ValueError(str(error))
    recipes = []
    for program in spec["programs"]:
        recipe = stage / program["folder"] / "game.toml"
        if not recipe.is_file():
            raise ValueError("missing staged " + program["folder"] + "/game.toml for BIOS policy")
        recipes.append(recipe)
    return recipes


def check(stage):
    stage = Path(stage).resolve()
    recipe = stage / "game.toml"
    if recipe.is_file():
        recipes = [recipe]
    elif (stage / "set.toml").is_file():
        recipes = set_recipes(stage)
    else:
        raise ValueError("missing staged game.toml for BIOS policy")
    required = []
    for recipe in recipes:
        for path in recipe_assets(stage, recipe):
            if path not in required:
                required.append(path)
    for path in required:
        path = path.resolve()
        try:
            path.relative_to(stage)
        except ValueError:
            raise ValueError("BIOS asset must remain inside the staged kit")
        if not path.is_file():
            raise ValueError("missing staged BIOS asset: " + str(path.relative_to(stage)))
    return [str(path.relative_to(stage)) for path in required]


if __name__ == "__main__":
    try:
        print("staged BIOS policy: " + ", ".join(check(sys.argv[1])))
    except (ValueError, OSError) as error:
        sys.exit("error: " + str(error))
