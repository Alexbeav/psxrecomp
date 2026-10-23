#!/usr/bin/env python3
"""Require BIOS assets selected by the staged title recipe.

A single-program package has its recipe at the stage root (game.toml). A set
of programs (set.toml, tools/program_set.py) has one recipe per program, in
the program's folder, and every one of them is checked. Their BIOS profile
paths are taken from the stage root: psxrecomp/ is there once, and setup
links it into each program's folder.

A Python before 3.11 with no tomli (the stock python3 of macOS 13) reads the
recipe with tools/program_set.py's reader, the one set.toml already uses: it
reads the [runtime] and [recompiler] keys this check needs (PS1B-576).
"""
import sys
from pathlib import Path
try:
    import tomllib
except ModuleNotFoundError:
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        tomllib = None


def program_set_module():
    tools = str(Path(__file__).resolve().parent)
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import program_set
    return program_set


def load_recipe(recipe):
    """The recipe's tables: tomllib or tomli when present, else program_set's reader."""
    if tomllib is not None:
        with recipe.open("rb") as handle:
            return tomllib.load(handle)
    tables = program_set_module().parse_toml_subset(recipe.read_text(encoding="utf-8"))
    # That reader reads [runtime] and [recompiler] tables only. A dotted key or an
    # inline table for them would be missed, so it stops instead of guessing.
    for key in tables.get("", {}):
        if key.split(".")[0].strip().strip("\"'") in ("runtime", "recompiler"):
            raise ValueError(recipe.name + ": write " + key + " inside its [table] on a Python "
                             "without tomllib or tomli")
    return tables


def recipe_assets(stage, recipe):
    """The BIOS files one recipe needs in the stage."""
    config = load_recipe(recipe)
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
    program_set = program_set_module()
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
