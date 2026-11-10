"""Final sharing archive inventory and fresh native-host check."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import zipfile

sys.stdout.reconfigure(encoding='utf-8',errors='replace')
if '--no-op' in sys.argv:
    print('noop: extract ZIP, verify all files, blank settings bridge and hidden native screenshot')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3];area=root/'reports/team-final-check-1';area.mkdir()
with zipfile.ZipFile(root/'dist/WorkbenchStudio-Windows-preview.zip') as z:z.extractall(area)
app=area/'WorkbenchStudio';checks=0
for line in (app/'SHA256SUMS.txt').read_text(encoding='utf-8').splitlines():
    wanted,name=line.split('  ',1)
    assert hashlib.sha256((app/name).read_bytes()).hexdigest()==wanted,name
    checks+=1
assert not any((app/p).exists() for p in ('public-studio-data','cache','history','settings.json'))
for path in app.rglob('*.py'):compile(path.read_text(encoding='utf-8'),str(path),'exec')
env=os.environ.copy();env.update(LOCALAPPDATA=str(area/'profile'),TEMP=str(root/'tmp'),TMP=str(root/'tmp'),PYTHONDONTWRITEBYTECODE='1')
request=area/'settings-request.json';data=area/'data'
request.write_text(json.dumps(dict(action='settings',data_root=str(data))),encoding='utf-8')
process=subprocess.run([str(app/'runtime/python.exe'),'-B',str(app/'native_bridge.py'),str(request)],cwd=app,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',creationflags=subprocess.CREATE_NO_WINDOW)
assert process.returncode==0,process.stderr
preferences=json.loads(process.stdout)['settings']
assert preferences['core_pin']==str(app/'versions/PIN.json') and preferences['ui_pin']==str(app/'versions/UI.json')
assert not preferences['bios_files'] and not preferences['output_folder'] and not preferences['startup_checks']
assert not (area/'profile').exists() and not data.exists()
user=ctypes.windll.user32;user.GetForegroundWindow.restype=ctypes.c_void_p;before=user.GetForegroundWindow()
user.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)];user.IsWindowVisible.argtypes=[ctypes.c_void_p]
callback_type=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p);visible=[]
child=subprocess.Popen([str(app/'Workbench Studio.exe'),'--preview',str(area/'native.png'),'--data-root',str(data)],cwd=app,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=subprocess.CREATE_NO_WINDOW)
def inspect(hwnd,data):
    owner=ctypes.c_ulong();user.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
    if owner.value==child.pid and user.IsWindowVisible(hwnd):visible.append(int(hwnd))
    return True
callback=callback_type(inspect);started=time.monotonic()
while child.poll() is None:
    user.EnumWindows(callback,0)
    if time.monotonic()-started>30:
        child.kill();child.wait();raise RuntimeError('Hidden native preview timed out')
    time.sleep(.02)
out,err=child.communicate()
assert child.returncode==0 and (area/'native.png').is_file(),err.decode('utf-8','replace')
assert not visible and before==user.GetForegroundWindow() and not (area/'profile').exists()
record=dict(status='pass',archive_files_verified=checks,blank_default_versions=True,python_sources_compile=True,
            native_exit=child.returncode,visible_windows=visible,foreground_unchanged=True,startup_payload_cache_created=False,
            settings=preferences,screenshot=str(area/'native.png'))
(area/'result.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps(record,indent=2))
