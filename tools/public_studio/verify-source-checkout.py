"""Exercise the actual short-cache Git/submodule path independently of Build."""
import json
import os
from pathlib import Path
import sys
from unittest import mock
import backend

root = Path(__file__).resolve().parents[3]
os.environ.update(TEMP=str(root / "tmp"), TMP=str(root / "tmp"),
                  PYTHONDONTWRITEBYTECODE="1", GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="Never")
if "--no-op" in sys.argv:
    print("noop: source checkout verification")
    raise SystemExit(0)
base = root / "tmp" / "source-checkout-1" / "Workbench-Studio-UI-Test-1" / "public-studio-data"
cache = base / "cache"
cache.mkdir(parents=True)
runner = backend.Runner()
with mock.patch.object(backend.preservation, "apply"):
    identity = backend.snapshot_source("core", "a7b37e079e8c6e98e04cd1cd42ba559e95911ea7", "", cache, base / "snapshot", runner)
source = cache / "c-a7b37e079e8c"
assert (source / ".git").is_dir()
modules = {}
for name, expected in [("recomp-net", "e53059306824edf4f0aec4aae5ae65151b18535f"),
                       ("retcomm-rbengine", "b29bdde642bc087f21303b827fadba40496b502d")]:
    path = source / "lib" / name
    actual = runner.run(["git", "-C", path, "rev-parse", "HEAD"], source)
    assert actual == expected, (name, actual)
    assert (base / "snapshot" / "lib" / name / "CMakeLists.txt").is_file()
    modules[name] = actual
record = {"scope": "source checkout only; source adaptation and full Build not exercised", "core": identity,
          "submodules": modules, "independent_gitdir": True, "credentials_required": False}
(root / "reports" / "source-checkout-1.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
print(json.dumps({"passed": 5, "submodules": modules}))
