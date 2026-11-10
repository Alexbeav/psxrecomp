"""Settings belong exclusively to this portable public Studio edition."""
from dataclasses import asdict, dataclass, field
import json
import os
from pathlib import Path


@dataclass
class Settings:
    bios_files: list[str] = field(default_factory=list)
    bios_regions: dict[str, str] = field(default_factory=dict)
    output_folder: str = ""
    core_pin: str = ""
    ui_pin: str = ""
    core_ref: str = "master"
    ui_ref: str = "master"
    custom_core: str = ""
    custom_ui: str = ""
    portable: bool = True
    skip_bios: bool = False
    pgo: bool = False
    diagnostics: bool = False
    compiler_optimization: str = "optimized"
    lto: bool = True
    startup_checks: bool = False
    theme: str = "dark"


def load(data_root: Path):
    path = data_root / "settings.json"
    if not path.exists():
        result = Settings()
        profile = Path(__file__).parent / "versions" / "PIN.json"
        if profile.is_file():
            result.core_pin = str(profile)
            result.ui_pin = str(profile.with_name("UI.json"))
        return result
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema") != "public-studio-settings-v1":
        raise ValueError("Unsupported public Studio settings format")
    values = value.get("settings", {})
    unknown = values.keys() - Settings.__dataclass_fields__.keys()
    if unknown:
        raise ValueError("Unknown settings: " + ", ".join(sorted(unknown)))
    result = Settings(**values)
    if not isinstance(result.bios_files, list) or not all(isinstance(p, str) for p in result.bios_files):
        raise ValueError("BIOS files must be a list of paths")
    if not isinstance(result.bios_regions, dict) or any(not isinstance(k, str) or v not in {"Auto", "USA", "Europe", "Japan"} for k, v in result.bios_regions.items()):
        raise ValueError("BIOS regions must be Auto, USA, Europe or Japan")
    for name in ("portable", "skip_bios", "pgo", "diagnostics", "lto", "startup_checks"):
        if type(getattr(result, name)) is not bool:
            raise ValueError(f"{name} must be true or false")
    for name in ("output_folder", "core_pin", "ui_pin", "core_ref", "ui_ref", "custom_core", "custom_ui", "theme", "compiler_optimization"):
        if not isinstance(getattr(result, name), str):
            raise ValueError(f"{name} must be text")
    if result.compiler_optimization not in {"fast", "optimized"}:
        raise ValueError("Compiler optimization must be fast or optimized")
    if result.theme not in {"dark", "light"}:
        raise ValueError("Theme must be dark or light")
    return result


def save(data_root: Path, settings: Settings):
    data_root.mkdir(parents=True, exist_ok=True)
    path = data_root / "settings.json"
    pending = data_root / ("settings-" + os.urandom(5).hex() + ".tmp")
    try:
        pending.write_text(json.dumps({"schema": "public-studio-settings-v1", "settings": asdict(settings)}, indent=2), encoding="utf-8")
        pending.replace(path)
    finally:
        if pending.exists():
            pending.unlink()
