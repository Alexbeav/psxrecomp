"""Bounded headless startup of the freshly produced packaging-check game."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding='utf-8',errors='replace')
if '--no-op' in sys.argv:
    print('noop: fresh produced game, unchanged compiler hint, 10-second muted headless start')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[3];area=root/'reports/team-clean-build-1'
build=json.loads((area/'result.json').read_text(encoding='utf-8'))
assert build['state']=='completed'
product=Path(build['games'][0]['output']);exe=next(product.glob('*.exe'))
hint=(product/'overlay_toolchain/compiler.txt').read_bytes()
env=os.environ.copy()
for name in ('PSX_EXIT_HALT','PSX_EXIT_AFTER_FRAMES','PSX_STOP_AFTER_FRAME','PSX_STOP_AFTER_FRAMES','PSX_MAX_FRAMES','PSX_FRAME_LIMIT','PSX_OVERLAY_AUTOCOMPILE_CMD','PSX_OVERLAY_BACKEND','PSX_REPLAY_FILE','PSX_SOURCE_TAS_REPLAY'):env.pop(name,None)
env.update(PSX_HEADLESS='1',PSX_NO_LAUNCHER='1',PSX_HOST_MUTE='1',PSX_EXIT_AFTER_MS='10000',SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='windows',WORKBENCH_BUILD_JOBS='1')
user=ctypes.windll.user32;user.GetForegroundWindow.restype=ctypes.c_void_p;before=user.GetForegroundWindow()
user.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)];user.IsWindowVisible.argtypes=[ctypes.c_void_p]
callback_type=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p);visible=[];start=time.monotonic()
with (area/'startup.stdout.log').open('wb') as out,(area/'startup.stderr.log').open('wb') as err:
    child=subprocess.Popen([str(exe)],cwd=product,env=env,stdout=out,stderr=err,creationflags=subprocess.CREATE_NO_WINDOW)
    def inspect(hwnd,data):
        owner=ctypes.c_ulong();user.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        if owner.value==child.pid and user.IsWindowVisible(hwnd):visible.append(int(hwnd))
        return True
    callback=callback_type(inspect)
    while child.poll() is None:
        user.EnumWindows(callback,0)
        if time.monotonic()-start>75:
            subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],capture_output=True,text=True,encoding='utf-8',errors='replace',creationflags=subprocess.CREATE_NO_WINDOW)
            child.wait();break
        time.sleep(.02)
report=json.loads((product/'psx_last_run_report.json').read_text(encoding='utf-8'))
result=dict(exit_code=child.returncode,seconds=time.monotonic()-start,visible_windows=visible,
            foreground_unchanged=before==user.GetForegroundWindow(),start_refused=report.get('start_refused'),
            overlay_compile=report.get('overlay_compile'),compiler_hint_unchanged=hint==(product/'overlay_toolchain/compiler.txt').read_bytes())
(area/'startup-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result,indent=2))
assert result['exit_code']==0 and not visible and result['foreground_unchanged'] and result['compiler_hint_unchanged']
assert not result['start_refused'] and result['overlay_compile']['configured']==1 and result['overlay_compile']['units_failed']==0
