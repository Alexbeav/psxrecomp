"""Recover only the two recorded user builds; no generation or title compilation."""
from pathlib import Path
import json
import os
import shutil
import sys
import time

if '--no-op' in sys.argv:
    print('noop: recover two retained linked user products, no title rebuild')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
batch=Path(r'I:\Documents\Desktop\Recomps\PS1\Test2\build-20261031-194221-d1b601')
source=root/'candidate/Workbench-Studio-Test-3'
evidence=root/'reports/user-recovery-3';evidence.mkdir()
frozen=evidence/'builder';frozen.mkdir()
for path in source.glob('*.py'):shutil.copy2(path,frozen/path.name)
shutil.copytree(source/'pin_builder',frozen/'pin_builder',ignore=shutil.ignore_patterns('__pycache__'))
sys.path.insert(0,str(frozen))
from pin_builder import pin_build as pb, workbench_output as output
assert pb.sha(output.LGPL_21)==output.LGPL_21_SHA256
all_results=[]
for ordinal,run_name in enumerate(('run-fhewfc39','run-b2kda9vg'),1):
    original_work=batch/'.work'/run_name/'work'
    work=original_work
    receipt=json.loads((work/'build-receipt.json').read_text(encoding='utf-8'))
    original=json.loads((work.parent/'evidence/build-receipt.json').read_text(encoding='utf-8'))
    assert receipt['status']=='build-pass' and original['failed_stage']=='stage-payload'
    job=evidence/run_name/'evidence';job.mkdir(parents=True)
    local_work=job.parent/'work'
    print('RECOVERY_COPYING '+run_name,flush=True)
    shutil.copytree(original_work,local_work)
    work=local_work
    receipt['executable']['path']=str(work/'build-release'/Path(receipt['executable']['path']).name)
    rebindings=[]
    for i,row in enumerate(receipt['inputs']):
        path=Path(row['path'])
        if path.is_relative_to(original_work):
            copied=work/path.relative_to(original_work)
            assert pb.sha(copied)==row['sha256'],str(path)
            receipt['inputs'][i]=pb.identity(copied)
            rebindings.append({'before':row,'after':receipt['inputs'][i],'content_changed':False})
        elif path.is_relative_to(source):
            copied=frozen/path.relative_to(source)
            if not copied.is_file():
                copied.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(path,copied)
            assert pb.sha(copied)==row['sha256'],str(path)
            receipt['inputs'][i]=pb.identity(copied)
            rebindings.append({'before':row,'after':receipt['inputs'][i],'content_changed':False})
    exe=Path(receipt['executable']['path']);before=pb.sha(exe)
    objects={str(p.relative_to(work)):pb.sha(p) for p in (work/'build-release').rglob('*.obj')}
    receipt.update(status='recovering',receipt=str(job/'build-receipt.json'),logs=str(job),work_directory=str(work),work_retained=True,
        recovery={'original_failure':str(original_work.parent/'evidence/build-receipt.json'),'source_rebindings':rebindings,'native_executable_rebuilds':0,'generated_object_count':len(objects)})
    pb.record_inputs([output.LGPL_21,Path(__file__)],receipt)
    validation=job/'validation';validation.mkdir()
    started=time.monotonic()
    try:
        output.stage_payload(work,validation,receipt)
        # Portable media are already copied by the original build. Check those
        # recorded bytes locally rather than rereading the live private share.
        for i,row in enumerate(receipt['inputs']):
            path=Path(row['path'])
            if str(path).startswith('\\\\'):
                candidates=list((validation/'inputs').rglob(path.name))
                match=next((p for p in candidates if pb.sha(p)==row['sha256']),None)
                if match is None:raise RuntimeError('Portable media differs from recorded input: '+path.name)
                receipt['inputs'][i]=pb.identity(match)
                rebindings.append({'before':row,'after':receipt['inputs'][i],'content_changed':False,'reason':'verified portable local media'})
        assert receipt['ui_asset_receipt']['sha256']==receipt['pin']['ui_host_contract']['assets']['receipt_sha256']
        print('RECOVERY_STAGED '+run_name,flush=True)
        parked_original=original_work.with_name('recovery-parked-work')
        if parked_original.exists() or original_work.parent != batch/'.work'/run_name:
            raise RuntimeError('Original workspace cannot be safely parked')
        original_work.rename(parked_original)
        try:
            output.validate_payload(work,validation,job,receipt)
        finally:
            parked_original.rename(original_work)
        pb.check_inputs(receipt)
        pin=batch/'PIN.json';output.check_registry(pin,pin.read_bytes(),receipt)
        original_output=Path(original['output'])
        destination=original_output.with_name(original_output.name+'-Recovered')/'windows-x64'
        destination.mkdir(parents=True)
        output.collect_validated_payload(validation,destination,job,receipt)
        output.write_provenance(destination,receipt)
        assert pb.sha(exe)==before==pb.sha(destination/exe.name)
        assert objects=={str(p.relative_to(work)):pb.sha(p) for p in (work/'build-release').rglob('*.obj')}
        receipt.update(status='build-pass',output=str(destination.parent),executable=pb.identity(destination/exe.name),qualification='Recovered unchanged linked title; original moved headless-start checks only, no gameplay or replay qualification')
        print('RECOVERY_COMPLETED '+str(destination),flush=True)
    except Exception as error:
        receipt.update(status='human-stop',next_action=str(error))
        print('RECOVERY_FAILED '+run_name+' '+str(error),flush=True)
    finally:
        receipt['recovery']['elapsed_seconds']=time.monotonic()-started
        pb.write_json(job/'build-receipt.json',receipt)
        all_results.append({'run':run_name,'status':receipt['status'],'receipt':str(job/'build-receipt.json'),'output':receipt.get('output')})
        pb.write_json(evidence/'result.json',all_results)
print(json.dumps(all_results),flush=True)
raise SystemExit(0 if all(r['status']=='build-pass' for r in all_results) else 1)

