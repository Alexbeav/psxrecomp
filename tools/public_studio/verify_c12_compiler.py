"""Bounded, never-mapped C12 check after compiler-store repair, local inputs only."""
from pathlib import Path
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
if '--no-op' in sys.argv:
 print('noop: copied C12, approved local media, 10s muted headless start, no launcher/window')
 raise SystemExit(0)
r=Path(__file__).resolve().parents[3];sys.path.insert(0,str(r/'candidate/Workbench-Studio-Test-4b'))
from pin_builder import pin_build as pb
area=r/'reports/c12-compiler-headless-1';area.mkdir();product=area/'game';source=Path(r'I:\Documents\Desktop\Recomps\PS1\Test2\C-12 - Final Resistance (USA)');shutil.copytree(source,product)
for name in ('psx_last_run_report.json','psx_crash.txt','psx_freeze_heartbeat.json'):(product/name).unlink(missing_ok=True)
disc=Path(r'I:\Projects\Recompilations\PS1\Shared-inputs\disc\C12-Final-Resistance-USA-49cab2ea3fc1\C-12 - Final Resistance (USA).chd');bios=r/'inputs/title/SCPH1001.BIN'
assert pb.sha(disc)=='49cab2ea3fc1f442ecfb49c1e6855d8ea77a57d92661bf9f3b3ee65d12762787' and pb.sha(bios)=='71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3'
config=area/'source-game.toml';shutil.copy2(product/'game.toml',config);serial=pb.product_roster(product)[1] if hasattr(pb,'product_roster') else ['SCUS-94627']
# Preserve the actual game's recorded serial; do not infer it from the filename.
import tomllib
serial=tomllib.loads(config.read_text(encoding='utf-8'))['game']['disc_serials']
pb.write_runtime_media(config,product,[disc],bios,serial,'thin',fast_boot=True)
env=os.environ.copy()
for key in ('PSX_EXIT_HALT','PSX_EXIT_AFTER_FRAMES','PSX_STOP_AFTER_FRAME','PSX_STOP_AFTER_FRAMES','PSX_MAX_FRAMES','PSX_FRAME_LIMIT','PSX_OVERLAY_AUTOCOMPILE_CMD','PSX_OVERLAY_BACKEND','PSX_REPLAY_FILE','PSX_SOURCE_TAS_REPLAY'):env.pop(key,None)
env.update(PSX_HEADLESS='1',PSX_NO_LAUNCHER='1',PSX_HOST_MUTE='1',PSX_EXIT_AFTER_MS='10000',SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='windows',WORKBENCH_BUILD_JOBS='1')
u=ctypes.windll.user32;u.GetForegroundWindow.restype=ctypes.c_void_p;before=u.GetForegroundWindow();callback_type=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p);u.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)];u.IsWindowVisible.argtypes=[ctypes.c_void_p]
exe=next(product.glob('*.exe'));visible=[];started=time.monotonic()
with (area/'stdout.log').open('wb') as stdout,(area/'stderr.log').open('wb') as stderr:
 child=subprocess.Popen([str(exe)],cwd=product,env=env,stdout=stdout,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
 def inspect(hwnd,data):
  owner=ctypes.c_ulong();u.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
  if owner.value==child.pid and u.IsWindowVisible(hwnd):visible.append(int(hwnd))
  return True
 callback=callback_type(inspect)
 while child.poll() is None:
  u.EnumWindows(callback,0)
  if time.monotonic()-started>75:
   subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],capture_output=True,text=True,encoding='utf-8',errors='replace',creationflags=subprocess.CREATE_NO_WINDOW);child.wait();break
  time.sleep(.02)
report=product/'psx_last_run_report.json';v=json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
record={'exit_code':child.returncode,'seconds':time.monotonic()-started,'visible_windows':visible,'foreground_unchanged':before==u.GetForegroundWindow(),'report':str(report),'start_refused':v.get('start_refused'),'overlay_compile':v.get('overlay_compile'),'exe_unchanged':pb.sha(exe)==pb.sha(next(source.glob('*.exe'))),'scope':'bounded overlay-cache/startup repair only; no gameplay or replay qualification'}
(area/'result.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps(record,indent=2));assert not visible and record['foreground_unchanged'] and record['exe_unchanged'];assert 'overlay_cache' not in json.dumps(v.get('start_refused',''))
