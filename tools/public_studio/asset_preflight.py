"""Stage the selected source's real UI asset manifest before a title compile."""
import shutil
from pathlib import Path
from pin_builder import ui_selection


def canonical_icon(core, product):
    source = Path(core) / "assets" / "psxrecomp.png"
    destination = Path(product) / "assets" / "psxrecomp.png"
    if source.is_file() and not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        return {"source": str(source), "sha256": ui_selection.sha256_file(source),
                "reason": "selected core's canonical app PNG, absent from recipe output"}
    return None


def check(core, ui, pin, ui_file, destination, cmake, runner, environment):
    destination.mkdir(parents=True)
    selected = ui_selection.select(pin, ui_file)
    artifacts = ui_selection.module_artifacts(pin, selected)
    if not artifacts:
        return {"mode": "bundled"}
    assets = (ui / "assets").as_posix()
    manifest = (ui / "cmake" / "recomp_ui_assets.cmake").as_posix()
    product = (destination / "product").as_posix()
    script = destination / "stage.cmake"
    script.write_text('''cmake_minimum_required(VERSION 3.20)
set(RUI_ASSETS "''' + assets + '''")
include("''' + manifest + '''")
_recomp_ui_resolve_console_assets(_console psx)
_recomp_ui_resolve_console_optional_assets(_optional psx)
file(MAKE_DIRECTORY "''' + product + '''/assets/fonts" "''' + product + '''/assets/img")
file(COPY ${_RUI_COMMON_FONT_ASSETS} DESTINATION "''' + product + '''/assets/fonts")
file(COPY ${_RUI_COMMON_IMG_ASSETS} ${_console} ${_optional} DESTINATION "''' + product + '''/assets/img")
''', encoding="utf-8")
    runner.run([cmake, "-P", script], destination, env=environment)
    target = destination / "product"
    icon = canonical_icon(core, target)
    contract = pin["ui_host_contract"]
    for key, source in [("module", artifacts["module"]), ("shared_sdl", artifacts["sdl3_dll"])]:
        path = target / contract[key]["file"]
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, path)
    receipt = ui_selection.write_asset_receipt(target / "assets", target / contract["assets"]["receipt_file"])
    checked = ui_selection.verify_staged(pin, selected, target)
    return {"asset_receipt": receipt, "checked": checked, "canonical_icon": icon,
            "scope": "standard PSX source manifest plus canonical icon; no title override assets"}
