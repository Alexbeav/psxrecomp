"""Extracted distribution build using its blank-profile supported versions."""
import json
import os
from pathlib import Path
import shutil
import sys
import time
import zipfile

sys.stdout.reconfigure(encoding='utf-8',errors='replace')
if '--no-op' in sys.argv:
    print('noop: fresh archive extraction, blank settings, public core fetch, included UI source, local TWINE build')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
area=root/'reports/team-clean-build-1';area.mkdir(exist_ok=True)
with zipfile.ZipFile(root/'dist/WorkbenchStudio-Windows-preview.zip') as z:z.extractall(area)
app=area/'WorkbenchStudio';sys.path.insert(0,str(app))
import settings,selection,backend
data=area/'data';prefs=settings.load(data)
assert not (data/'settings.json').exists() and prefs.core_pin==str(app/'versions/PIN.json') and prefs.ui_pin==str(app/'versions/UI.json')
assert not prefs.bios_files and not prefs.output_folder and not prefs.startup_checks
cache=root/'tmp/tc'
# Reuse approved immutable SDK bytes in a separate cache, not any candidate settings/source/profile.
sdk=cache/'pins/toolchains/1.0.14';sdk.parent.mkdir(parents=True,exist_ok=True)
def link(source,target):
    if not Path(target).exists():
        os.link(source,target)
shutil.copytree(root/'inputs/toolchain',sdk,copy_function=link,dirs_exist_ok=True)
os.environ.update(WORKBENCH_PUBLIC_CACHE_ROOT=str(cache),WORKBENCH_PUBLIC_WORK_ROOT=str(root/'tmp/tw'),
                  WORKBENCH_SHARED_COMPILER_ROOT=str(root.parents[2]/'Shared-inputs/toolchain'))
prefs.bios_files=[str(root/'inputs/title/SCPH1001.BIN')];prefs.output_folder=str(area/'games')
prefs.portable=False;prefs.skip_bios=True;prefs.lto=False;prefs.compiler_optimization='fast'
image=root/'inputs/twine/007 - The World Is Not Enough (USA).chd'
expected='643508a56b9c746c41ea4a72a2df30572ad6c7b0970cd0fe9ca2f7f17f5717ab'
assert backend.sha256(image)==expected
runner=backend.Runner(lambda s:print(s,flush=True),lambda e:print('EVENT '+json.dumps(e),flush=True))
started=time.monotonic();result={}
try:
    result=backend.build_batch(selection.group_files([image]),prefs,data,runner)
    assert result['state']=='completed',result
    product=Path(result['games'][0]['output'])
    hint=(product/'overlay_toolchain/compiler.txt').read_text(encoding='utf-8').strip()
    assert 'candidate' not in hint and len(str(Path(hint).parent.parent/'SHA256SUMS.txt'))<200
    result['produced_compiler_hint']=hint
finally:
    result['verification']=dict(seconds=time.monotonic()-started,input_unchanged=backend.sha256(image)==expected,
                                sdk='approved 1.0.14 immutable bytes reused; core anonymously fetched; UI from shipped checksummed source')
    (area/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('TEAM_CLEAN_BUILD_PASS')
