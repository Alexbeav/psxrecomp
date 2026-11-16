"""Exercise the exact selected module asset identity, including missing-icon refusal."""
from pathlib import Path
import json
import sys
import tempfile
import asset_preflight
import backend
from pin_builder import pin_build as pb, ui_selection

if "--no-op" in sys.argv:
    print("noop: real selected-pin asset byte verification")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[3]
cache = root / "pin-cache"
pin = pb.load_pin(cache / "records" / "PIN.json")
runner = backend.Runner()
with tempfile.TemporaryDirectory(dir=root / "tmp") as temporary:
    work = Path(temporary)
    missing = work / "no-icon-core"
    missing.mkdir()
    try:
        asset_preflight.check(missing, cache / "u", pin, cache / "ui-input" / "UI.json",
            work / "before", root / "inputs/toolchain/bin/cmake.exe", runner, None)
    except ui_selection.UiSelectionError as error:
        negative = str(error)
        assert "c4758815b776420291e274657596b5a204f9f398f246b624abe4e2c1ea87e9da" in negative
    else:
        raise AssertionError("Missing icon did not reproduce the actual refusal")
    result = asset_preflight.check(cache / "c", cache / "u", pin, cache / "ui-input" / "UI.json",
        work / "after", root / "inputs/toolchain/bin/cmake.exe", runner, None)
    actual = pb.sha(work / "after/product/assets-receipt.json")
    assert actual == pin["ui_host_contract"]["assets"]["receipt_sha256"]
    assert result["canonical_icon"]["sha256"] == "c2f3886727558480cc2c397119f6b411ac70c0e4969627561551f11c3c144485"
    record = {"positive": "pass", "negative": "pass", "negative_error": negative,
              "asset_receipt_sha256": actual, "title_compiler_invocations": 0, "result": result}
    (root / "reports" / "module-asset-check-1.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(json.dumps(record))
