"""Exact selected-pin TWINE path regression, using verified local media."""
from pathlib import Path
import json
import os
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

if '--no-op' in sys.argv:
    print('noop: exact TWINE pin pipeline, optimized/no-LTO/fast-boot/thin, no startup tests')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
app=root/'candidate/Workbench-Studio-Test-4b'
sys.path.insert(0,str(app))
import settings,selection,backend
area=root/'reports/twine-short-build-2';area.mkdir()
data=area/'data'
prefs=settings.load(app/'public-studio-data')
prefs.bios_files=[str(root/'inputs/title/SCPH1001.BIN')];prefs.bios_regions={}
prefs.output_folder=str(area/'games')
assert not prefs.portable and prefs.skip_bios and not prefs.lto and not prefs.pgo and not prefs.startup_checks and prefs.compiler_optimization=='optimized'
image=root/'inputs/twine/007 - The World Is Not Enough (USA).chd'
expected='643508a56b9c746c41ea4a72a2df30572ad6c7b0970cd0fe9ca2f7f17f5717ab'
assert backend.sha256(image)==expected
os.environ['WORKBENCH_PUBLIC_CACHE_ROOT']=str(root/'candidate/Workbench-Studio-Test-2/public-studio-data/cache')
os.environ['WORKBENCH_PUBLIC_WORK_ROOT']=str(root/'tmp/wq2')
os.environ['WORKBENCH_BUILD_JOBS']='1'
runner=backend.Runner(lambda line:print(line,flush=True),lambda event:print('BATCH_EVENT '+json.dumps(event),flush=True))
started=time.monotonic();result={}
try:
    result=backend.build_batch(selection.group_files([image]),prefs,data,runner)
    assert result['state']=='completed',result
finally:
    result['verification']={'elapsed_seconds':time.monotonic()-started,'input_unchanged':backend.sha256(image)==expected,'selected_options':{'portable':prefs.portable,'skip_bios':prefs.skip_bios,'lto':prefs.lto,'optimization':prefs.compiler_optimization,'startup_checks':prefs.startup_checks},'short_work_root':os.environ['WORKBENCH_PUBLIC_WORK_ROOT']}
    (area/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('TWINE_SHORT_BUILD_PASS',flush=True)
