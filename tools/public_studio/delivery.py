"""Deliver only a game folder; keep work and diagnostic records in Studio data."""
import hashlib
import os
import stat
from pathlib import Path
import re
import shutil


def digest(path):
    value=hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda:stream.read(1024*1024),b''):value.update(data)
    return value.hexdigest()


def name_for(name):
    name=re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', name)
    name=re.sub(r'\s+', ' ', name).strip(' .')[:100] or 'Game'
    if re.fullmatch(r'(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?',name):name='Game '+name
    return name


def deliver(payload, output, name, members=None):
    payload,output=Path(payload).resolve(),Path(output).expanduser().resolve()
    output.mkdir(parents=True,exist_ok=True)
    stem=name_for(name)
    for ordinal in range(1,10001):
        target=output/(stem if ordinal==1 else f'{stem} ({ordinal})')
        try:target.mkdir()
        except FileExistsError:continue
        break
    else:raise RuntimeError('No unused game folder name is available')
    copied=[]
    try:
        for member in sorted(payload.iterdir()):
            if members is not None and member.name not in members:continue
            if member.is_symlink() or (hasattr(member,'is_junction') and member.is_junction()):
                raise RuntimeError('Game payload contains a symbolic link')
            destination=target/member.name
            if member.is_dir():shutil.copytree(member,destination)
            else:shutil.copy2(member,destination)
        for source in payload.rglob('*'):
            if source.is_file() and (members is None or source.relative_to(payload).parts[0] in members):
                relative=source.relative_to(payload)
                if digest(source)!=digest(target/relative):
                    raise RuntimeError('Delivered file differs: '+str(relative))
                copied.append(str(relative))
    except Exception:
        # This exclusively-created destination belongs to this delivery.
        shutil.rmtree(target)
        raise
    return target,{'files_verified':len(copied),'collision_suffix':ordinal}


def remove_owned_tree(path, root):
    path, root = Path(path).resolve(), Path(root).resolve()
    if path == root or not path.is_relative_to(root):
        raise RuntimeError("Refusing to remove scratch outside its owned root")
    def readonly(function, name, info):
        if not isinstance(info[1], PermissionError):
            raise info[1]
        os.chmod(name, stat.S_IREAD | stat.S_IWRITE)
        function(name)
    shutil.rmtree(path, onerror=readonly)
