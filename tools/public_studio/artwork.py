"""Use the player's existing serial-to-cover source, with a nonblocking fallback."""
from pathlib import Path
import hashlib
import json
import re
import urllib.request
import probe_disc

COVER_BASE = "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/"


def detect(image):
    image=Path(image)
    if image.suffix.lower()=='.chd':
        import art_chd
        from pin_builder.pin_build import read_chd_boot_serial_psx_chd
        library=Path(__file__).parent/'assets/libchdr.dll'
        if not library.is_file():return ''
        return read_chd_boot_serial_psx_chd(art_chd,image,library) or ''
    return probe_disc.probe(image,identity_only=True).serial or ''


def cover(serial,data_root):
    if not re.fullmatch(r'[A-Z]{4}-\d{5}',serial):return None
    # Match the launcher cover source exactly. Failed/offline art is optional.
    target=Path(data_root)/'artwork'/(serial+'.jpg')
    local=Path(__file__).parent/'assets/artwork'/(serial+'.jpg')
    for path in (local,target):
        if path.is_file():return path
    with urllib.request.urlopen(COVER_BASE+serial+'.jpg',timeout=8) as response:
        body=response.read(8*1024*1024+1)
    if len(body)>8*1024*1024 or not body.startswith(b'\xff\xd8'):return None
    target.parent.mkdir(parents=True,exist_ok=True)
    pending=target.with_suffix('.download');pending.write_bytes(body);pending.replace(target)
    target.with_suffix('.json').write_text(json.dumps({'source':COVER_BASE+serial+'.jpg','bytes':len(body),'sha256':hashlib.sha256(body).hexdigest()},indent=2),encoding='utf-8')
    return target
