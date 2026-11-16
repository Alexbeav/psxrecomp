"""Build the clean Windows sharing archive from the tested native distribution."""
import ast
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[3]
APP = Path(__file__).parent
OUT = ROOT / 'dist' / 'WorkbenchStudio'


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def source_copy(source, target):
    text = source.read_text(encoding='utf-8-sig')
    tree = ast.parse(text)
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.body and isinstance(node.body[0], ast.Expr) and isinstance(node.body[0].value, ast.Constant) and isinstance(node.body[0].value.value, str):
                node.body.pop(0)
                if not node.body:
                    node.body.append(ast.Pass())
    text = ast.unparse(tree) + '\n'
    text = re.sub(r'PS1[BG]-\d+', 'build', text)
    # Retired private registry discovery is not part of this edition.
    text = text.replace('Z:/Share/Projects/Recompilations/PS1/Programme/WORKBENCH-STUDIO.json', 'registry/Studio.json')
    text = text.replace(' Retired Z:/Share/psxrecomp is not searched.', '')
    target.write_text(text, encoding='utf-8')


def main():
    if OUT.exists():
        raise ValueError('Choose a fresh output before packaging')
    base = ROOT / 'candidate/Workbench-Studio-Test-4b'
    OUT.mkdir(parents=True)
    for path in base.iterdir():
        if path.is_dir() and path.name in {'runtime', 'assets', 'pin_builder', 'licenses', 'compatibility'}:
            shutil.copytree(path, OUT/path.name, ignore=shutil.ignore_patterns('__pycache__'))
        elif path.is_file() and path.suffix.lower() in {'.py', '.toml', '.exe', '.dll'}:
            shutil.copy2(path, OUT/path.name)
    for path in APP.glob('*.py'):
        if path.name in {p.name for p in OUT.glob('*.py')}:
            shutil.copy2(path, OUT/path.name)
    shutil.copy2(APP/'setup_public.ps1',OUT/'setup_public.ps1')
    for notice in (APP/'assets/licenses').iterdir():
        target = OUT/'assets/licenses'/notice.name
        if not target.exists():
            shutil.copy2(notice,target)
    for path in (APP/'pin_builder').glob('*.py'):
        shutil.copy2(path, OUT/'pin_builder'/path.name)
    (OUT/'pin_builder/SOURCE.json').unlink(missing_ok=True)
    for name in ('linux_build.py','linux_build.sh','linux_gate.py','macos_build.py'):
        (OUT/'pin_builder'/name).unlink(missing_ok=True)
    (OUT/'assets/NATIVE-SOURCE.json').unlink(missing_ok=True)
    (OUT/'compatibility/STATUS.txt').write_text('Included supported Windows versions use the established module builder. Latest upstream source-only builds and Linux remain unqualified.\n',encoding='utf-8')
    for path in OUT.rglob('*.py'):
        source_copy(path, path)
    versions = OUT/'versions'; versions.mkdir()
    pin = json.loads((ROOT/'pin-cache/records/PIN.json').read_text(encoding='utf-8'))
    pin = {k: pin[k] for k in ('repo','branch','commit','tree','boot_state_version','recomp_ui_ref','toolchain_pin','ui_host_contract')}
    pin['name'] = 'Supported Windows core and launcher'
    pin['status'] = 'test'
    (versions/'PIN.json').write_text(json.dumps(pin, indent=2), encoding='utf-8')
    ui = json.loads((ROOT/'pin-cache/ui-input/UI.json').read_text(encoding='utf-8'))
    ui.pop('artifacts_source', None); ui['name'] = 'Included launcher module'
    # Complete corresponding build sources/assets, without private history or test fixtures.
    blob = subprocess.run(['git','-C',str(ROOT/'pin-cache/u'),'archive','--format=tar',ui['ui']['commit']], capture_output=True, check=True).stdout
    files = {}
    archive = versions/'launcher-source.zip'
    with tarfile.open(fileobj=io.BytesIO(blob)) as source, zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as output:
        for member in source.getmembers():
            if not member.isfile() or member.name.split('/')[0] in {'docs','test_data','tests'} or member.name == 'README.md':
                continue
            data = source.extractfile(member).read()
            if Path(member.name).suffix in {'.c','.cpp','.h','.hpp','.cmake','.py','.tsv','.md','.txt'}:
                data = re.sub(rb'PS1[BG]-\d+',b'launcher',data)
            files[member.name] = hashlib.sha256(data).hexdigest()
            output.writestr(member.name, data)
        marker = dict(schema=1,commit=ui['ui']['commit'],tree=ui['ui']['tree'],files=files,
                      scope='Corresponding build sources and assets; documentation and test fixtures omitted; internal issue labels removed from comments/descriptions/diagnostics')
        output.writestr('SOURCE-SNAPSHOT.json',json.dumps(marker,sort_keys=True))
    ui['source_package'] = dict(file=archive.name,sha256=digest(archive))
    (versions/'UI.json').write_text(json.dumps(ui,indent=2),encoding='utf-8')
    shutil.copy2(ROOT/'pin-cache/ui-input/recomp-launcher-v1.dll',versions/'recomp-launcher-v1.dll')
    shutil.copytree(ROOT/'pin-cache/ui-input/sdl-install',versions/'sdl-install')
    shutil.copy2(ROOT/'pin-cache/u/LICENSE',OUT/'licenses/launcher-MIT.txt')
    shutil.copy2(ROOT/'pin-cache/u/assets/common/fonts/NOTICE.md',OUT/'licenses/launcher-font-NOTICE.md')
    shutil.copy2(ROOT/'pin-cache/u/assets/common/img/NOTICE.md',OUT/'licenses/launcher-image-NOTICE.md')
    # Only native Studio is exposed; no copied settings, caches or run records.
    (OUT/'Workbench Studio.cmd').write_text('@echo off\nset "PYTHONDONTWRITEBYTECODE=1"\nstart "" /D "%~dp0" "%~dp0Workbench Studio.exe" %*\n',encoding='utf-8')
    (OUT/'QUICKSTART.txt').write_text('Workbench Studio - Windows preview\n\nExtract the complete folder, then open Workbench Studio.cmd.\nOpen Settings and choose your BIOS files and output folder. Choose game images or playlists, then BUILD. Add matching SBI files in the same selection.\n\nGit for Windows must be installed and available on PATH. Advanced source builds also require PowerShell 7 (pwsh); the included supported versions do not. Source and compiler downloads happen after BUILD. The included launcher source and module use the selected compatible core version. First build needs internet and several GB of free disk space.\n\nCompleted games appear directly in the output folder. Logs and settings are under public-studio-data. Build caches are in %LOCALAPPDATA%\\RPT. Thin games use its persistent compiler cache; keep that cache while using those games. Portable builds include their compiler and media. Startup checks are off by default.\n\nThis is a Windows preview, not a qualification of every game or the latest upstream source. Linux is unqualified. Source versions remain selectable in Settings.\n',encoding='utf-8')
    # Strip private build-machine notes while retaining all licence texts and hashes.
    for path in OUT.rglob('*.toml'):
        text=path.read_text(encoding='utf-8-sig')
        path.write_text('\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))+'\n',encoding='utf-8')
    provenance=dict(schema=1,core=dict(repository=pin['repo'],commit=pin['commit'],tree=pin['tree']),
                    launcher=dict(commit=ui['ui']['commit'],tree=ui['ui']['tree'],source_package=ui['source_package'],
                                  module_sha256=ui['module']['sha256']),
                    shared_sdl=ui['shared_sdl'],input_media_bundled=False,settings_bundled=False,cache_bundled=False)
    (OUT/'VERSIONS.json').write_text(json.dumps(provenance,indent=2),encoding='utf-8')
    members={p.relative_to(OUT).as_posix():digest(p) for p in sorted(OUT.rglob('*')) if p.is_file()}
    (OUT/'SHA256SUMS.txt').write_text(''.join(v+'  '+k+'\n' for k,v in members.items()),encoding='utf-8')
    target=OUT.parent/'WorkbenchStudio-Windows-preview.zip'
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for p in sorted(OUT.rglob('*')):
            if p.is_file():z.write(p,p.relative_to(OUT.parent).as_posix())
    target.with_suffix('.zip.sha256').write_text(digest(target)+'  '+target.name+'\n',encoding='ascii')
    print(json.dumps(dict(archive=str(target),bytes=target.stat().st_size,files=len(members),sha256=digest(target))))


if __name__ == '__main__':
    main()
