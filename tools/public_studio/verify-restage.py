"""Re-stage the already linked private title with a frozen repaired builder."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import sys
import time

if "--no-op" in sys.argv:
    print("noop: re-stage retained compiled Ape; no generation or executable rebuild")
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
original_run=root/"pin-runs/run-9ivnnhf7"
work=original_run/"work"
evidence=root/"reports/ape-restage-1"
evidence.mkdir()
frozen=evidence/"builder"
frozen.mkdir()
app=Path(__file__).parent
for path in app.glob("*.py"):
    shutil.copy2(path,frozen/path.name)
shutil.copytree(app/"pin_builder",frozen/"pin_builder",ignore=shutil.ignore_patterns("__pycache__"))
sys.path.insert(0,str(frozen))
from pin_builder import pin_build as pb, workbench_output as output
receipt=json.loads((work/"build-receipt.json").read_text(encoding="utf-8"))
source_rebindings=[]
for index,row in enumerate(receipt["inputs"]):
    path=Path(row["path"])
    if path.is_relative_to(app):
        copied=frozen/path.relative_to(app)
        replacement=pb.identity(copied)
        source_rebindings.append({"before":row,"after":replacement,
            "content_changed":row["sha256"]!=replacement["sha256"],"reason":"frozen repaired builder for this new staging verification"})
        receipt["inputs"][index]=replacement
pb.record_inputs([frozen/"asset_preflight.py",frozen/"backend.py",Path(__file__)],receipt)
exe=Path(receipt["executable"]["path"])
before=pb.sha(exe)
objects={str(path.relative_to(work)):pb.sha(path) for path in (work/"build-release").rglob("*.obj")}
receipt.update(status="restage-running",receipt=str(evidence/"build-receipt.json"),logs=str(evidence),
               work_directory=str(work),work_retained=True,keep_development_files=True,
               restage={"original_receipt":str(work/"build-receipt.json"),"source_rebindings":source_rebindings,
                        "native_executable_rebuilds":0,"generated_object_count":len(objects)})
started=time.monotonic()
validation=evidence/"validation"
validation.mkdir()
destination=root/"candidate/Ape-Pinned-Validated-1"
if destination.exists():
    raise ValueError("Fresh validated product destination required")
try:
    output.stage_payload(work,validation,receipt)
    assert receipt["ui_asset_receipt"]["sha256"]==receipt["pin"]["ui_host_contract"]["assets"]["receipt_sha256"]
    output.validate_payload(work,validation,evidence,receipt)
    pb.check_inputs(receipt)
    output.check_registry(root/"pin-cache/records/PIN.json",(root/"pin-cache/records/PIN.json").read_bytes(),receipt)
    clean=destination/"windows-x64"
    clean.mkdir(parents=True)
    output.collect_validated_payload(validation,clean,evidence,receipt)
    output.write_provenance(clean,receipt)
    after={str(path.relative_to(work)):pb.sha(path) for path in (work/"build-release").rglob("*.obj")}
    assert before==pb.sha(exe)==pb.sha(clean/exe.name)
    assert objects==after
    receipt.update(status="build-pass",output=str(destination),executable=pb.identity(clean/exe.name),
                   runtime_config=str(clean/"game.toml"),qualification="Linked title, unchanged executable/object hashes, exact module assets and original moved headless-start checks; no gameplay or TAS qualification")
    receipt["restage"].update(executable_unchanged=True,objects_unchanged=True)
    print(json.dumps({"status":receipt["status"],"product":str(clean),"receipt":receipt["receipt"],"executable_sha256":before}))
except Exception as error:
    receipt.update(status="human-stop",next_action=str(error))
    raise
finally:
    receipt["restage"]["seconds"]=time.monotonic()-started
    pb.write_json(evidence/"build-receipt.json",receipt)
