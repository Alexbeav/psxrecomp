"""Relocate an exact compiler store; retain the pinned runtime integrity contract."""
from pathlib import Path
import ctypes
import hashlib
import json
import os
import stat
import sys
import time

sys.stdout.reconfigure(encoding='utf-8',errors='replace')
if '--no-op' in sys.argv:
 print('noop: verify shared compiler manifest/files, copy to short immutable provider, retarget thin hints only')
 raise SystemExit(0)
root=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root/'candidate/Workbench-Studio-Test-4b'))
from pin_builder import workbench_output as output
from pin_builder import pin_build as pb
base=Path(r'I:\Documents\Desktop\Recomps\PS1\Test2')
example=base/'C-12 - Final Resistance (USA)'
compiler=Path((example/'overlay_toolchain/compiler.txt').read_text(encoding='utf-8').strip())
source=compiler.parent.parent
manifest=(source/'SHA256SUMS.txt').read_bytes()
expected=(example/'overlay_toolchain/compiler-store.sha256').read_text(encoding='ascii').strip()
assert hashlib.sha256(manifest).hexdigest()==expected
entries=[]
for line in manifest.decode('utf-8').splitlines():
 digest,relative=line.split('  ',1)
 path=source/relative
 assert len(digest)==64 and not Path(relative).is_absolute() and '..' not in Path(relative).parts
 assert pb.sha(path)==digest,relative
 entries.append({'path':relative,'sha256':digest,'bytes':path.stat().st_size})
provider=Path(r'I:\Projects\Recompilations\PS1\Shared-inputs\toolchain')
started=time.monotonic()
installed=output.install_shared_compiler(source,entries,provider)
target=Path(installed['root']);assert (target/'SHA256SUMS.txt').read_bytes()==manifest
crt=ctypes.CDLL('ucrtbase');crt.fopen.argtypes=[ctypes.c_char_p,ctypes.c_char_p];crt.fopen.restype=ctypes.c_void_p;crt.fclose.argtypes=[ctypes.c_void_p]
def opens(path):
 handle=crt.fopen(str(path).encode('ascii'),b'rb')
 if handle:crt.fclose(handle)
 return bool(handle)
old_open=opens(source/'SHA256SUMS.txt');new_open=opens(target/'SHA256SUMS.txt');assert new_open
repairs=[]
for game in base.iterdir():
 hint=game/'overlay_toolchain/compiler.txt';marker=game/'overlay_toolchain/compiler-store.sha256'
 if hint.is_file() and marker.is_file() and marker.read_text(encoding='ascii').strip()==expected:
  before=hint.read_text(encoding='utf-8')
  binary=next(game.glob('*.exe'));sha=pb.sha(binary)
  hint.write_text(str(target/'bin/clang.exe')+'\n',encoding='utf-8')
  assert pb.sha(binary)==sha and marker.read_text(encoding='ascii').strip()==expected
  repairs.append({'game':str(game),'before':before.strip(),'after':str(target/'bin/clang.exe'),'exe_sha256':sha,'integrity_marker_unchanged':True})
for file in target.rglob('*'):
 if file.is_file():os.chmod(file,stat.S_IREAD)
record={'status':'compiler-provisioned','original_manifest_length':len(str(source/'SHA256SUMS.txt')),'short_manifest_length':len(str(target/'SHA256SUMS.txt')),'manifest_sha256':expected,'files_verified':len(entries),'payload_bytes':installed['payload_bytes'],'old_ucrt_fopen':old_open,'short_ucrt_fopen':new_open,'repairs':repairs,'elapsed_seconds':time.monotonic()-started,'gameplay_checked':False}
(root/'reports/thin-compiler-repair-1.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps(record,indent=2))

