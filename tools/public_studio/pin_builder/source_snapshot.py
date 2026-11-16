"""Read a complete, checksummed source export without requiring Git history."""
import hashlib
import json
from pathlib import Path
import shutil


def validate(root, commit):
    root = Path(root)
    marker = root / 'SOURCE-SNAPSHOT.json'
    if not marker.is_file():
        return None
    record = json.loads(marker.read_text(encoding='utf-8'))
    if record['commit'] != commit or record['schema'] != 1:
        raise ValueError('Source snapshot identity differs from selected version')
    actual = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in root.rglob('*') if p.is_file() and p != marker}
    if actual != record['files']:
        raise ValueError('Source snapshot files differ from their checksums')
    return record


def export(root, commit, destination, records):
    record = validate(root, commit)
    if record is None:
        return False
    shutil.copytree(root, destination)
    (destination / 'SOURCE-SNAPSHOT.json').unlink()
    records.append(dict(repo=str(root), commit=record['commit'], tree=record['tree'],
                        destination=str(destination), provider='checksummed-source-export',
                        manifest_sha256=hashlib.sha256((Path(root)/'SOURCE-SNAPSHOT.json').read_bytes()).hexdigest()))
    return True
