"""Check the CLI bridge against actual retained original-builder receipts."""
from pathlib import Path
import contextlib
import io
import json
import runpy
import sys
from unittest import mock
from pin_builder import pin_build

if "--no-op" in sys.argv:
    print("noop: original-builder status bridge")
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
run=root/"pin-runs/run-9ivnnhf7"
results=[]
for path,expected in [(run/"work/build-receipt.json",0),(run/"evidence/build-receipt.json",1)]:
    receipt=json.loads(path.read_text(encoding="utf-8"))
    arguments=["pin_entry.py","build","--pin",str(root/"pin-cache/records/PIN.json"),
               "--bios",str(root/"inputs/title/SCPH1001.BIN"),"--disc",str(root/"inputs/title/Ape Escape (USA).cue"),
               "--output",str(root/"tmp/status-bridge-not-created")]
    with mock.patch.object(sys,"argv",arguments),mock.patch.object(pin_build,"build",return_value=receipt),contextlib.redirect_stdout(io.StringIO()):
        try:
            runpy.run_path(str(Path(__file__).with_name("pin_entry.py")),run_name="__main__")
        except SystemExit as result:
            assert result.code==expected,(receipt["status"],result.code,expected)
            results.append({"source":str(path),"actual_original_status":receipt["status"],"exit_code":result.code})
        else:
            raise AssertionError("CLI did not return a status")
assert not (root/"tmp/status-bridge-not-created").exists()
record={"passed":2,"build_started":False,"cases":results}
(root/"reports/status-bridge-1.json").write_text(json.dumps(record,indent=2),encoding="utf-8")
print(json.dumps(record))
