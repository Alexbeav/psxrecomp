"""Run isolated PSXRTI1/PSXRTI2 playback, or a PSXRTI3 two-digital-pad route with disc events,
and retain process and input evidence.

The native observer owns input checkpoints. A clean process exit alone does
not qualify playback. Default execution has no window and uses software video.
"""
import argparse
import hashlib
import dualshock_route
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import time
from input_contexts import decode as decode_contexts, check_protected_effects


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def spu_keyoff_resume_environment(model, previous, resume_from, compatible, dma_model):
    if model not in ('default', 'octoshock-1.11.9'):
        raise ValueError('unknown SPU key-off model')
    if model != 'default' and dma_model not in ('octoshock-2.2.2-bounded-linked-list', 'octoshock-2.2.2-bounded-quad'):
        raise ValueError('older SPU key-off model requires the source-profile GPU/DMA mode')
    if previous and (previous != 'unversioned' or resume_from is None or not compatible or model != 'octoshock-1.11.9'):
        raise ValueError('SPU key-off migration requires compatible native resume and explicit octoshock-1.11.9 model')
    env = {'PSX_SPU_KEYOFF_MODEL': model} if model != 'default' else {}
    if previous:
        env['PSX_TAS_RESUME_SPU_KEYOFF_FROM'] = previous
    return env


def legacy_clock_resume_environment(old_path, expected_sha, resume_from, compatible, clock_tape):
    """Select only an explicit, content-bound legacy path migration for a native checkpoint."""
    if old_path is None and expected_sha is None:
        return {}
    if (not old_path or '\0' in old_path or not expected_sha or
            not re.fullmatch(r'[0-9a-f]{64}', expected_sha) or
            resume_from is None or not compatible or not clock_tape or
            clock_tape['sha256'] != expected_sha):
        raise ValueError('legacy clock path/hash require compatible native resume and the same current clock tape SHA256')
    # The old host path is identity text, not a resource to resolve on this host.
    return {'PSX_TAS_RESUME_LEGACY_CLOCK_PATH': old_path,
            'PSX_TAS_RESUME_LEGACY_CLOCK_SHA256': expected_sha}


def checkpoint_asset_digest(path):
    """Bind CUE content and its tracks, not just the small descriptor file."""
    value = digest(path)
    if path.suffix.lower() != '.cue':
        return value
    tracks = []
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if re.match(r'\s*FILE\b', line, re.I):
            match = re.fullmatch(r'\s*FILE\s+"([^"\r\n]+)"\s+BINARY\s*', line, re.I)
            if not match:
                raise ValueError('checkpoint media requires quoted BINARY cue tracks')
            tracks.append(digest((path.parent / match[1]).resolve(strict=True)))
    if not tracks:
        raise ValueError('checkpoint cue has no tracks')
    return hashlib.sha256('\n'.join([value, *tracks]).encode('ascii')).hexdigest()


# Who wrote a PSXRTI2 route's axis bytes decides what the pad receives. Nymashock hands the pad a
# 16-bit value per axis (byte<<8), which the device rescales to a byte; Octoshock's wrapper hands
# it the logged byte unscaled (corpus PSX-TAS-001). The runtime takes the same choice from
# PSX_INPUT_ROUTE_AXIS_MODEL and names its profile in complete.json.
AXIS_MODELS = {'nymashock-u16': 'nymashock-2.9.1-dualshock-neutral-analog',
               'octoshock-byte': 'octoshock-byte-dualshock-neutral-analog'}
DEFAULT_AXIS_MODEL = 'nymashock-u16'
# A physical MODE press in the route (the analog flag of a PSXRTI2 row) is admitted only under a named MODE model.
# 'octoshock-2.4.2': the pad evaluates the latched button when the port's DTR falls (corpus PSX-TAS-001).
# 'nymashock-1.29.0': the pad also evaluates it when an input arrives while its DTR is inactive.
# Each needs the octoshock-byte axis model (a route of protocol bytes), and the runtime reads
# PSX_INPUT_ROUTE_MODE_MODEL.
MODE_MODELS = ('none', 'octoshock-2.4.2', 'nymashock-1.29.0')


def two_digital_identity(data, start=0):
    """A PSXRTI3 two-digital-pad route (octoshock210_route): P1 and P2 words from record
    `start` on, its console events and its disc set. The runtime hashes P1 as
    applied_words_sha256 and P2 as applied_port2_words_sha256."""
    from octoshock210_route import read_psxrti3, EVENT_NAMES
    route = read_psxrti3(data)
    p1, p2 = bytearray(), bytearray()
    steps, previous = 0, None
    for index, row in enumerate(route['rows']):
        steps += row != previous; previous = row
        if index >= start:
            p1 += struct.pack('<H', row[0]); p2 += struct.pack('<H', row[1])
    return {'format': 'PSXRTI3', 'port_layout': 'two-digital', 'frames': route['frames'], 'steps': steps,
            'sha256': hashlib.sha256(data).hexdigest(),
            'words_sha256': hashlib.sha256(p1).hexdigest(),
            'port2_words_sha256': hashlib.sha256(p2).hexdigest(),
            'console_events': [{'frame': f, 'event': EVENT_NAMES[e], 'disc_index': d} for f, e, d in route['events']],
            'disc_set': [{'kind': k, 'digest': d} for k, d in route['discs']]}


def route_identity(path, start=0, axis_model=DEFAULT_AXIS_MODEL, mode_model='none', neutral_p2=False):
    if axis_model not in AXIS_MODELS:
        raise ValueError('unknown DualShock axis model')
    if mode_model not in MODE_MODELS or (mode_model != 'none' and axis_model != 'octoshock-byte'):
        raise ValueError('a physical MODE model needs the octoshock-byte axis model')
    if path.stat().st_size > 28 + (16 << 20) + 12 * dualshock_route.V3_MAX_FRAMES:
        raise ValueError('route exceeds frame capacity')
    data = path.read_bytes()
    if len(data) < 24:
        raise ValueError("short route")
    magic, version, size, count, reserved = struct.unpack_from("<8sIIII", data)
    if magic == b'PSXRTI3\0' and size == 12:
        # A tagged two-digital body is also twelve bytes. Try that declared
        # layout only when its mandatory port-layout tag actually exists.
        ext=struct.unpack_from('<I',data,24)[0] if len(data)>=28 else 0
        offset=28;has_ports=False
        while offset<28+ext and offset+8<=len(data):
            tag,length=struct.unpack_from('<II',data,offset)
            if tag==0x201:has_ports=True
            offset+=8+((length+3)&~3)
        if not has_ports:
            identity=dualshock_route.read_v3_dualshock(data,start,axis_model=='octoshock-byte',mode_model,neutral_p2)
            identity['controller_profile']=AXIS_MODELS[axis_model] if mode_model=='none' else (
                'octoshock-2.4.2-dualshock-mode' if mode_model=='octoshock-2.4.2' else 'nymashock-1.29.0-dualshock-mode')
            return identity
    if magic == b'PSXRTI3\0':
        if axis_model != DEFAULT_AXIS_MODEL or mode_model != 'none':
            raise ValueError('an axis or MODE model applies to a DualShock route only')
        return two_digital_identity(data, start)
    if magic == b'PSXRTI2\0':
        if (version, size, reserved) != (2, 12, 0) or not 0 < count <= 1000000 or len(data) != 24+12*count:
            raise ValueError('invalid DualShock route header/count/size')
        original, protocol = hashlib.sha256(), hashlib.sha256()
        devices = bytes([2,0,0,0,2 if neutral_p2 else 0,0,0,0])
        full = hashlib.sha256(b'TAS_SOURCE_CONTROLLER_V1\0' + devices)
        previous, steps = None, 0
        for index in range(count):
            sequence, buttons, ly, lx, ry, rx, analog, zero = struct.unpack_from('<IH6B', data, 24+12*index)
            if sequence != index+1 or zero or analog > 1 or (analog and mode_model == 'none'):
                raise ValueError('DualShock sequence/reserved bytes or unqualified physical Analog press')
            row = struct.pack('<H5B', buttons, ly, lx, ry, rx, analog)
            steps += row != previous; previous = row
            if steps > dualshock_route.MAX_STEPS:  # the encoder's and runtime's shared cap
                raise ValueError('DualShock step capacity exceeded')
            if index >= start: original.update(row)
            if axis_model == 'octoshock-byte':
                axes = (ly, lx, ry, rx)
            else:
                # Nyma expands axes to u16 with value<<8; the device rescales to u8.
                axes = [((value << 8)*255+32767)//65535 for value in (ly,lx,ry,rx)]
            if index >= start:
                applied = struct.pack('<H5B', buttons, *axes, analog)
                protocol.update(applied)
                full.update(struct.pack('<IB', sequence, 1) + applied)
                if neutral_p2:
                    full.update(struct.pack('<BH5B', 2, 0xffff, 128, 128, 128, 128, 0))
        identity = {'format':'PSXRTI2', 'frames':count, 'steps':steps,
                    'sha256':hashlib.sha256(data).hexdigest(),
                    'original_controller_sha256':original.hexdigest(),
                    'expected_protocol_sha256':protocol.hexdigest(),
                    'full_expected_applied_sha256':full.hexdigest()}
        if axis_model != DEFAULT_AXIS_MODEL:
            # Named only when not the default, so every recorded Nymashock identity is unchanged.
            identity.update(axis_model=axis_model, controller_profile=AXIS_MODELS[axis_model])
        if mode_model != 'none':
            identity.update(mode_model=mode_model, controller_profile=mode_model + '-dualshock-mode')
        return identity
    if axis_model != DEFAULT_AXIS_MODEL or mode_model != 'none':
        raise ValueError('an axis or MODE model applies to a DualShock route only')
    if (magic, version, size, reserved) != (b"PSXRTI1\0", 1, 8, 0):
        raise ValueError("unsupported route header")
    if not 0 < count <= 1000000 or len(data) != 24 + 8 * count:
        raise ValueError("route frame count or size")
    words = bytearray()
    for index in range(count):
        sequence, word, zero = struct.unpack_from("<IHH", data, 24 + 8 * index)
        if sequence != index + 1 or zero:
            raise ValueError("route sequence or reserved bytes")
        if index >= start: words.extend(struct.pack("<H", word))
    return {"frames": count, "sha256": digest(path),
            "words_sha256": hashlib.sha256(words).hexdigest()}


def card_identity(path):
    if path.stat().st_size != 131072:
        raise ValueError('card1 must be an exact raw 128 KiB card')
    return {'path':str(path), 'bytes':131072, 'sha256':digest(path)}


def checkpoint_interval(manifest, frames, tail, terminal_return=False):
    frame, consumed = manifest.get('frame'), manifest.get('input_consumed')
    terminal = frames + tail - (0 if terminal_return else 1)
    if (manifest.get('schema') != 'psx-tas-stateio-v3' or
            type(frame) is not int or type(consumed) is not int or
            not 0 < frame < terminal or not 0 < consumed or
            consumed > frames + tail or (not terminal_return and consumed==frames+tail)):
        raise ValueError('invalid checkpoint resume interval')
    return frame, consumed


def resolve_saved_states(run, requested, require_input_history=False):
    """Map each requested return to the one checkpoint that satisfies it.

    A capture requested at a return with no represented instruction continuation
    moves to the next return that has one; its manifest records requested_frame
    (the earliest request it satisfies) and frame (where it was taken). A request
    is satisfied by exactly one state with requested_frame <= request <= frame,
    and that state must be intact: its size, SHA-256 and card images."""
    states = []
    for sidecar in sorted(Path(run).glob('tas-state-*.pst.json')):
        state = sidecar.with_suffix('')
        try:
            m = json.loads(sidecar.read_text())
        except (OSError, ValueError):
            continue
        frame, first = m.get('frame'), m.get('requested_frame')
        if type(frame) is not int or type(first) is not int or not 0 < first <= frame:
            continue
        valid = (state.is_file() and state.name == f'tas-state-{frame:06d}.pst'
                 and m.get('state_bytes') == state.stat().st_size and m.get('state_sha256') == digest(state)
                 and checkpoint_cards_valid(state, m)
                 and (not require_input_history or checkpoint_input_history_valid(state,m)))
        states.append((first, frame, valid, state.name))
    resolved = []
    for request in sorted(set(requested)):
        matches = [s for s in states if s[0] <= request <= s[1]]
        entry = {'frame': request, 'state_frame': None, 'state': None, 'valid': False}
        if len(matches) == 1:
            entry.update(state_frame=matches[0][1], state=matches[0][3], valid=matches[0][2])
        resolved.append(entry)
    return resolved


def checkpoint_cards_valid(state, manifest):
    """Every card image the manifest names exists beside the state with that SHA-256."""
    for slot in (1, 2):
        expected = manifest.get(f'card{slot}_sha256')
        if expected == 'none':
            continue
        card = Path(f'{state}.card{slot}.mcd')
        if (type(expected) is not str or not card.is_file() or card.stat().st_size != 131072
                or digest(card) != expected):
            return False
    return True


def checkpoint_input_history_valid(state,manifest):
    """Check bound bytes/header; runtime validates contexts and actual cursor contents."""
    expected=manifest.get('input_observer_sha256')
    sidecar=Path(str(state)+'.input-observer')
    if (not isinstance(expected,str) or not re.fullmatch('[0-9a-f]{64}',expected) or
            not sidecar.is_file() or sidecar.stat().st_size<2048 or digest(sidecar)!=expected):
        return False
    with sidecar.open('rb') as stream:return stream.read(8)==b'PSXIOB1\0'


def validate_audited_resume(state, manifest, executable_sha256, compatible_build=False):
    """Bind the real input journal even across an explicit compatible rebuild.

    The runtime still verifies state ABI, restored RAM/cycle, BIOS, entry,
    route, configuration, cards and the actual journal contexts/cursor.
    """
    if manifest.get('exe_sha256') != executable_sha256 and not compatible_build:
        raise ValueError('audited resume binary differs without explicit compatible-build mode')
    if not checkpoint_input_history_valid(state, manifest):
        raise ValueError('audited resume requires its intact actual input observer journal')


def playback_identity_matches(complete, identity, tail):
    if (not isinstance(complete, dict) or complete.get('frame') != identity['frames']+tail or
            complete.get('input_frames') != identity['frames'] or complete.get('neutral_tail_ticks') != tail):
        return False
    if identity.get('format') == 'PSXRTI3' and identity.get('controller_layout') != 'dualshock':
        return (complete.get('port_layout') == 'two-digital' and
                complete.get('applied_words_sha256') == identity['words_sha256'] and
                complete.get('applied_port2_words_sha256') == complete.get('supplied_port2_words_sha256')
                == identity['port2_words_sha256'])
    if identity.get('format') == 'PSXRTI2' or identity.get('controller_layout') == 'dualshock':
        return (complete.get('controller_profile') == identity.get('controller_profile', AXIS_MODELS[DEFAULT_AXIS_MODEL]) and
                complete.get('original_controller_sha256') == identity['original_controller_sha256'] and
                complete.get('applied_controller_sha256') == complete.get('expected_protocol_sha256') == identity['expected_protocol_sha256'])
    return complete.get('applied_words_sha256') == identity['words_sha256']


def validate_disc_event_receipts(actual, expected_events, roster, before_first_runtime_frame):
    """Check all post-operation receipts against exact event order and media."""
    if len(actual)!=len(expected_events):raise ValueError('missing/duplicate committed disc-event receipt')
    mounted=1;opened=0
    event_names={2:'tray-open',3:'tray-close',4:'disc-select'}
    for ordinal,(wanted,got) in enumerate(zip(expected_events,actual)):
        if (got.get('ordinal')!=ordinal or got.get('route_boundary')!=wanted['frame'] or
            got.get('source_input_frame')!=wanted['frame']+1 or got.get('runtime_frame')!=before_first_runtime_frame+wanted['frame'] or
            got.get('phase')!='before_next_input' or got.get('result')!=1 or
            event_names.get(got.get('event'))!=wanted['event'] or got.get('disc_index')!=wanted['disc_index']):
            raise ValueError('disc event missing/early/late/reordered/rejected or wrong requested disc')
        before=opened
        if wanted['event']=='tray-open':opened=1
        elif wanted['event']=='disc-select':
            if not opened:raise ValueError('disc selected with closed tray')
            mounted=wanted['disc_index']
        elif wanted['event']=='tray-close':
            if not opened or mounted!=wanted['disc_index']:raise ValueError('wrong disc at tray close')
            opened=0
        else:raise ValueError('unimplemented console event in full audit')
        if (got.get('tray_open_before')!=before or got.get('tray_open_after')!=opened or
            got.get('media_ready_after')!=1-opened or got.get('actual_source_available')!=1):
            raise ValueError('committed drive readback differs from expected tray/media state')
        cue=Path(roster[mounted-1]['path'])
        expected_path=cue
        if cue.suffix.lower()=='.cue':
            tracks=re.findall(r'^\s*FILE\s+"([^"\r\n]+)"\s+BINARY\s*$',cue.read_text(encoding='utf-8-sig'),re.I|re.M)
            if not tracks:raise ValueError('CUE has no source track')
            expected_path=cue.parent/tracks[0]
        if Path(got['actual_bin_path']).resolve(strict=True)!=expected_path.resolve(strict=True):
            raise ValueError('mounted reader points at wrong disc source')
    return len(actual)


def validate_full_input_audit(run, identity, tail, neutral_p2, roster):
    """Verify every applied readback row, not selected boundary samples."""
    devices=bytes([2,0,0,0,2 if neutral_p2 else 0,0,0,0])
    source=hashlib.sha256(b'TAS_SOURCE_CONTROLLER_V1\0'+devices)
    tails=hashlib.sha256(b'TAS_TAIL_CONTROLLER_V1\0'+devices)
    boundaries=hashlib.sha256()
    count=0;first=None;last=None
    path=run/'applied-input-boundaries.bin'
    with path.open('rb') as stream:
        while record:=stream.read(32):
            if len(record)!=32:raise ValueError('incomplete applied-input boundary row')
            frame,runtime,phase,p1,p2,zero=struct.unpack_from('<IQ4B',record)
            count+=1
            if frame!=count or (phase,p1,p2,zero)!=(1,2,2 if neutral_p2 else 0,0) or any(record[30:]):
                raise ValueError('applied frame sequence/phase/topology/reserved mismatch')
            if first is None:first=runtime
            if runtime!=first+count-1:raise ValueError('applied input runtime frame early/late/duplicate')
            last=runtime
            h=source if frame<=identity['frames'] else tails
            h.update(struct.pack('<IB',frame,1)+record[16:23])
            if neutral_p2:h.update(bytes([2])+record[23:30])
            elif any(record[23:30]):raise ValueError('undeclared P2 readback')
            if frame>identity['frames'] and (record[16:23]!=struct.pack('<H5B',0xffff,128,128,128,128,0)):
                raise ValueError('nonneutral declared tail')
            boundaries.update(record)
    if count!=identity['frames']+tail:raise ValueError('incomplete full applied-input count')
    if source.hexdigest()!=identity['full_expected_applied_sha256']:
        raise ValueError('full applied input hash differs from independently checked source route')
    done=json.loads((run/'complete.json').read_text(encoding='utf-8'))
    expected={'full_applied_input_sha256':source.hexdigest(),'full_applied_original_frames':identity['frames'],
              'tail_applied_input_sha256':tails.hexdigest(),'tail_applied_frames':tail,
              'runtime_boundary_rows_sha256':boundaries.hexdigest(),'runtime_boundary_rows':count,
              'first_delivery_end_runtime_frame':first,'last_delivery_end_runtime_frame':last,
              'missing_frames':0,'duplicate_frames':0,'out_of_order_frames':0}
    if any(done.get(key)!=value for key,value in expected.items()):
        raise ValueError('full audit completion receipt differs from observed row stream')
    expected_events=identity.get('console_events',[])
    event_path=run/'disc-events.jsonl'
    actual=[json.loads(line) for line in event_path.read_text(encoding='utf-8').splitlines()] if event_path.exists() else []
    validate_disc_event_receipts(actual,expected_events,roster,first-1)
    return dict(valid=True,frames=identity['frames'],tail=tail,expected_applied_sha256=source.hexdigest(),
                boundary_rows_sha256=boundaries.hexdigest(),committed_disc_events=len(actual),
                measurement='actual SIO frontend readback at every before-next-input runtime boundary;not guest poll or gameplay proof')


def source_clock_identity(path, toc_model, seek_model, read_model):
    if path is None:
        return None
    if (toc_model, seek_model, read_model) != (
            'octoshock-2.2.2', 'octoshock-2.2.2', 'octoshock-2.2.2-pipeline'):
        raise ValueError('source clock tape requires source TOC, explicit seek and pipeline models')
    path = path.resolve(strict=True)
    data = path.read_bytes()
    if len(data) < 20 or data[:16] != b'PSX-CD-RNG1\0\0\0\0\0':
        raise ValueError('invalid source clock tape header')
    count = struct.unpack_from('<I', data, 16)[0]
    if not 1 <= count <= 1048576 or len(data) != 20 + count * 4:
        raise ValueError('invalid source clock tape count or length')
    return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'words': count}


def disc_roster(route, boot_disc, roster, game, relocate=False, resume=False, compatible=False):
    """Bind a PSXRTI3 disc set to --roster-disc (in order) and to game.toml [game] discs.

    The runtime binds the route to the discs game.toml names and mounts the boot disc
    from --disc, so all three must name the same images, disc 1 first."""
    if relocate and not (resume and compatible):
        raise ValueError('disc relocation requires explicit compatible native resume')
    discs = route.get('disc_set') or []
    if not discs:
        if roster or relocate:
            raise ValueError('--roster-disc needs a route with a disc set')
        return None
    if len(roster) != len(discs):
        raise ValueError(f'the route names {len(discs)} discs; give each one with --roster-disc')
    roster = [p.resolve(strict=True) for p in roster]
    if roster[0] != boot_disc:
        raise ValueError('--disc must be the first --roster-disc')
    entries = []
    for index, (path, disc) in enumerate(zip(roster, discs), 1):
        if disc['kind'] != 1 or checkpoint_asset_digest(path) != disc['digest']:
            raise ValueError(f'roster disc {index} differs from the route disc set: {path}')
        entries.append({'index': index, 'path': str(path), 'kind': 'cue_asset_digest', 'digest': disc['digest']})
    import tomllib
    listed = tomllib.loads(Path(game).read_text(encoding='utf-8')).get('game', {}).get('discs')
    if not isinstance(listed, list) or len(listed) != len(entries) or any(not isinstance(d, str) for d in listed):
        raise ValueError('game.toml [game] discs must list the roster discs in route order')
    if relocate:
        for entry, declared in zip(entries, listed):
            entry['original_path'] = os.path.abspath(Path(game).parent / declared)
        disc_relocation_text(digest(game), route['sha256'], entries)  # Validate all bounds before creating output.
    elif [str((Path(game).parent / d).resolve()) for d in listed] != [e['path'] for e in entries]:
        raise ValueError('game.toml [game] discs must list the roster discs in route order')
    return entries


def disc_relocation_text(game_sha, route_sha, entries):
    """Bounded versioned mapping; original game bytes are never rewritten."""
    if not 1 <= len(entries) <= 8 or any(not re.fullmatch('[0-9a-f]{64}', h) for h in (game_sha, route_sha)):
        raise ValueError('invalid relocation identity/count')
    old, new = set(), set()
    lines = ['PSX-ROSTER-RELOCATION-1', game_sha, route_sha, str(len(entries))]
    for index, entry in enumerate(entries, 1):
        before, after, sha = entry['original_path'], entry['path'], entry['digest']
        if (entry['index'] != index or not re.fullmatch('[0-9a-f]{64}', sha)
                or any(not p or len(p.encode('utf-8')) >= 4096 or any(c in p for c in '\t\r\n\0') for p in (before, after))
                or not Path(after).is_absolute() or before in old or after in new):
            raise ValueError('invalid relocation path/order/digest')
        old.add(before); new.add(after)
        lines.append(f'{index}\t{sha}\t{before}\t{after}')
    return '\n'.join(lines) + '\n'


def settings_text(renderer, fast_boot, hle, bios, disc, card1, card2=False):
    """The settings.toml of one run.

    settings_format = 2 says that this file's fast_boot and bios_hle lines were
    written on purpose. Without that line the loader takes both for an old
    launcher's echo and drops them (PS1B-360), and --fast-boot did nothing
    (PS1B-394). The key is top-level, so it stands before the first table.
    """
    return f'''settings_format = 2
[video]
renderer = "{renderer}"
supersampling = 1
window_width = 960
antialiasing = false
texture_filtering = "nearest"
crt_filter = "raw"
auto_skip_fmv = false
turbo_loads = false
fast_boot = {str(bool(fast_boot)).lower()}
bios_hle = {str(bool(hle)).lower()}
fullscreen = 0
frame_interpolation = false
aspect_ratio = "4:3"
adaptive_view = false
[audio]
spu_hq = false
[launcher]
skip_launcher = true
[bios]
path = "{Path(bios).as_posix()}"
[disc]
path = "{Path(disc).as_posix()}"
[memcard]
dir = "cards"
card1 = "cards/card1.mcd"
card2 = "cards/card2.mcd"
enable1 = {str(bool(card1)).lower()}
enable2 = {str(bool(card2)).lower()}
[controller]
p1_device = "keyboard"
p1_mode = "digital"
p2_device = "none"
p2_mode = "digital"
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    for name in ("exe", "game", "route", "disc", "bios"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument('--roster-disc', type=Path, action='append', default=[],
                        help='each disc of a PSXRTI3 route disc set, in route order; the first is --disc')
    parser.add_argument('--card1', type=Path, help='raw initial card1; staged as a fresh writable copy and verified in the loaded peripheral')
    parser.add_argument('--card2', type=Path, help='raw initial card2; independently copied and observed')
    parser.add_argument('--port2-controller', choices=('none', 'dualshock-neutral'), default='none')
    parser.add_argument('--full-input-audit', action='store_true', help='hash every applied original input and record every runtime boundary; cold DualShock routes only')
    parser.add_argument('--card-model', choices=('default','nymashock-1.29.0','nymashock-1.32.1','nymashock-2.8'), default='default')
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--show", action="store_true")
    parser.add_argument("--speed", choices=("1", "2", "4", "8", "16", "32", "64", "max"), default="1",
                        help="visible replay fast-forward cap; headless runs are always uncapped")
    parser.add_argument("--renderer", choices=("software", "opengl"), default="software",
                        help="rendering backend; window visibility is independent")
    parser.add_argument("--fast-boot", action="store_true")
    parser.add_argument("--hle", action="store_true")
    parser.add_argument("--scheduler", choices=("hle", "lle"), default="hle")
    parser.add_argument("--neutral-tail", type=int, default=0,
                        help="verified neutral SIO samples after unchanged route EOF")
    parser.add_argument("--debug-port", type=int, default=4394)
    parser.add_argument("--sio-trace", action="store_true")
    parser.add_argument("--update-profile", type=Path,
                        help="explicit experimental accepted-update clock configuration and route receipt")
    parser.add_argument("--update-decisions",type=Path,
                        help="measured poll acceptance decisions from a bounded clean-boot compilation")
    parser.add_argument("--update-predictor",choices=("gate", "previous-accept"),default="gate",
                        help="experimental prediction only; actual packet/context acceptance stays authoritative")
    parser.add_argument("--checkpoint-every", type=int, default=300)
    parser.add_argument('--save-state-at', type=int, nargs='+', metavar='RETURN',
                        help='TAS checkpoint: save a full-machine state once at each listed frontend return')
    parser.add_argument('--resume-from', type=Path, metavar='FILE',
                        help='TAS checkpoint: restore this saved state and continue the route from its frame')
    parser.add_argument('--resume-compatible-build', action='store_true',
                        help='explicit checkpoint continuation across rebuilds; runtime compatibility/ABI and all state/input identities still required')
    parser.add_argument('--resume-relocate-discs', action='store_true',
                        help='explicitly map original game roster paths to the same route-bound disc contents for compatible native resume')
    parser.add_argument('--resume-legacy-clock-path',
                        help='exact recorded clock tape path for explicit legacy checkpoint migration; requires paired SHA256 and compatible resume')
    parser.add_argument('--resume-legacy-clock-sha256',
                        help='SHA256 of the unchanged current and recorded clock tape; never disables other checkpoint identities')
    parser.add_argument('--continue-on-divergence', action='store_true',
                        help='record the first oracle mismatch but continue full input playback; never changes the input or comparison verdict')
    parser.add_argument('--e-survey', action='store_true',
                        help='diagnostic: report source-GPU/source-DMA quiescence counts at frame boundaries')
    parser.add_argument('--perturb-restore', metavar='FIELD',
                        help='negative control: corrupt exactly one restored field '
                             '(raster_fraction, raster_cycle, raster_rises, '
                             'service_cycle, service_frame_returns, service_budget)')
    parser.add_argument('--dump-at-frame', type=int, metavar='FRAME',
                        help='diagnostic: one full freeze dump at the first heartbeat sample at or after this frame '
                             '(PSX_DUMP_AT_FRAME); it needs no automatic dump slot and a diagnostic product')
    parser.add_argument('--storage-budget-mib',type=int,help='Stop with host_storage_budget if diagnostic output exceeds this bound')
    parser.add_argument('--expected-exe-sha256',help='refuse to launch unless the staged executable copy has exactly this SHA-256')
    parser.add_argument("--watch-u16", type=lambda x: int(x, 0), action="append", default=[],
                        help="read a physical RAM u16 before each next input (max32)")
    parser.add_argument("--record-frame", type=int,
                        help="arm existing ordered access recorder for one runtime frame; export watched accesses")
    parser.add_argument("--cpu-state", action="store_true",
                        help="read-only CPU and IRQ state at each before-next-input boundary")
    parser.add_argument("--video-state", action="store_true",
                        help="passive display mode, field and video-clock state at each before-next-input boundary")
    parser.add_argument("--instruction-histogram",type=int,nargs=2,metavar=("FIRST","LAST"),
                        help="passive instruction-fetch counts for one to eight runtime input-boundary intervals")
    parser.add_argument("--instruction-site",type=lambda x:int(x,0),action="append",default=[],
                        help="ordered passive timestamps for up to 16 selected fetch PCs, capped at 8192 per interval")
    parser.add_argument('--cpu-boundary-window',type=int,nargs=2,metavar=('LOW','HIGH'),
                        help='source-profile passive pre-fetch CPU scalars in a bounded absolute cycle interval')
    parser.add_argument('--gpu-command-window',type=int,nargs=2,metavar=('LOW','HIGH'),
                        help='Bounded read-only GPU command diagnostic')
    parser.add_argument('--device-history-probe',action='store_true',help='passive existing CD trace and SPU scalar copy at CPU returns')
    parser.add_argument('--cpu-return-probe',action='store_true',
                        help='passive CPU scalars at original-model frontend return boundaries')
    parser.add_argument('--ram-page-probe',action='store_true',
                        help='passive hashes of all512 RAM pages at original-model frontend returns')
    parser.add_argument('--terminal-return-probe',action='store_true',
                        help='finish the current source-profile CPU instruction and observe the final return without another input or tail')
    parser.add_argument('--stock-reference', type=Path,
                        help='original stock capture directory (pages.tsv and cpu-actual.tsv); stop on first CPU/RAM mismatch')
    parser.add_argument('--stock-ram-only', action='store_true',
                        help='compare all original RAM pages when the retained original explicitly lacks CPU observation; CPU/clock stay unmeasured')
    parser.add_argument('--ram-snapshot-frame',type=int,action='append',default=[],
                        help='also save raw RAM at a selected frontend return (requires page probe; max64)')
    parser.add_argument("--read-watch", type=lambda x: int(x, 0), nargs=2,
                        metavar=("LOW", "HIGH"), help="record existing RAM reads in physical [LOW,HIGH)")
    parser.add_argument("--cd-read-start-model", choices=("default", "octoshock-2.2.2-pipeline"), default="default",
                        help="explicit source-core pipeline comparison; not full timing compatibility")
    parser.add_argument('--cd-tray-model', choices=('default', 'octoshock-2.10'), default='default',
                        help='exact Legacy Octoshock2.10 deferred tray notification; separate from qualified2.7/2.4.2 behavior')
    parser.add_argument('--cd-drive-model',choices=('default','nymashock-1.29.0','nymashock-1.32.1','octoshock-2.4.2','octoshock-2.6.2','octoshock-2.7'),default='default',
                        help='experimental staged physical drive; exact Octoshock2.4.2/2.6.2 bindings observed for SeekL; '
                             'other operations and full movie remain comparison obligations; Octoshock uses jitter-first Init draws')
    parser.add_argument('--cd-init-draw-order',choices=('default','jitter-first','floor-first'),default='default',
                        help='order of the two Init random draws when a route has observed it; default keeps '
                             "the drive model's own order")
    parser.add_argument('--cd-source-clock-tape', type=Path,
                        help='experimental command/seek clock using an immutable raw random-word tape')
    parser.add_argument('--cd-source-clock-tape-extension', type=Path,
                        help='longer raw tape with an identical complete base prefix; base and checkpoint identities still verified')
    parser.add_argument('--cd-cdda-model',choices=('default','octoshock-2.3'),default='default',
                        help='source CDDA single-speed seek, two-sector pipe and report ownership')
    parser.add_argument('--mdec-source-model',choices=('default','octoshock-2.3','nymashock-1.29.0','nymashock-1.32.1','octoshock-2.4.2','octoshock-2.6.2'),default='default',
                        help='source MDEC FIFO/work and request DMA model; color output only')
    parser.add_argument('--cd-dma-model',choices=('default','octoshock-2.2.2'),default='default',
                        help='experimental manual CD DMA service and CPU wait; cold boot only')
    parser.add_argument("--cd-toc-seek-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="ReadTOC deterministic source seek lower bound; jitter and exact head position omitted")
    parser.add_argument("--cd-explicit-seek-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="SeekL/P source lower bound with paused/standby state; physical head and jitter remain approximate")
    parser.add_argument("--cd-firmware-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="source PU-18 Test20 identity; does not change seek timing")
    parser.add_argument("--cd-xa-eof-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="XA audio end-of-file sector: octoshock-2.2.2 raises no data-end interrupt and keeps reading")
    parser.add_argument("--cd-cold-status-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="source cold sticky shell-open bit cleared after first GetStat; cold boot only")
    parser.add_argument("--syscall-model", choices=("default", "guest-exception"), default="default",
                        help="route every syscall through the installed guest exception handler")
    parser.add_argument("--critical-section-model", choices=("direct", "exception"), default="direct",
                        help="experimental non-nested SYS01/02 guest exception execution")
    parser.add_argument("--field-model", choices=("default", "octoshock-2.2.2-ntsc-fields", "octoshock-2.2.2-ntsc-raster"), default="default",
                        help="experimental cold-boot source field durations; no state restore or PAL")
    parser.add_argument('--gpu-status-model', choices=('default','octoshock-2.2.2-raster'), default='default',
                        help='source GPUSTAT field/line bits only; requires the NTSC raster clock')
    parser.add_argument("--legacy-card-repair", choices=("default", "off"), default="default",
                        help="Disable inherited global Ape Escape fixed-address card repair explicitly")
    parser.add_argument('--axis-model', choices=tuple(AXIS_MODELS), default=DEFAULT_AXIS_MODEL,
                        help='who wrote the DualShock route axis bytes: Nymashock u16 (rescaled) or Octoshock (unscaled bytes)')
    parser.add_argument('--mode-button-model', choices=MODE_MODELS, default='none',
                        help='admit physical MODE presses in a DualShock route: the pad evaluates the latched button on the DTR falling edge; needs --axis-model octoshock-byte')
    parser.add_argument("--pad-ack-model", choices=("default", "octoshock-2.2.2-digital", "nymashock-1.29.0-dualshock", "nymashock-1.32.1-dualshock", "nymashock-2.8-gamepad"), default="default",
                        help="Experimental source pad ACK delay/pulse; checkpoints retain the pad, DualShock and card state")
    parser.add_argument("--dma-model", choices=("default", "octoshock-2.2.2-otc"), default="default",
                        help="experimental source OTC service and CPU-wait rule; cold boot only")
    parser.add_argument('--spu-keyoff-model', choices=('default','octoshock-1.11.9'), default='default',
                        help='explicit older source-provider key-off handling; default retains newer startup holdoff')
    parser.add_argument('--resume-spu-keyoff-from', choices=('unversioned',),
                        help='explicit compatible native resume changing only the formerly absent SPU key-off model')
    parser.add_argument('--gpu-dma-model', choices=('default','octoshock-2.2.2-vram-upload','octoshock-2.2.2-bounded-linked-list','octoshock-2.2.2-bounded-quad'), default='default',
                        help='source GPU DMA timing; bounded-quad adds the cold automatic FIFO/work projection with strict draw-state guards')
    parser.add_argument("--timer1-model",choices=("default","octoshock-2.2.2"),default="default",
                        help="experimental source timer1 clock/sync; IRQ-enabled modes and restore reject")
    parser.add_argument("--timer2-model",choices=("default","octoshock-2.2.2"),default="default",
                        help="experimental source timer2 clock/IRQ state; requires source timer1; timer0 IRQ modes and restore reject")
    parser.add_argument("--precise-slice", choices=("off", "on"), default="off",
                        help="Experimental existing instruction-boundary IRQ slicer; RAM BIOS guard requires the qualified candidate emitter")
    parser.add_argument("--slice-gpu-deadline", choices=("tick", "phase"), default="tick",
                        help="precise-slice GPU deadline: the service clock's 128-cycle tick (qualified) or the raster phase edge (step-2 candidate)")
    parser.add_argument("--slice-bound", choices=("steal", "nosteal", "icache", "none"), default="steal",
                        help="precise-slice block bound: 240 cycles per access (qualified); 40 while no source DMA transfer is live (step-2); "
                             "icache = nosteal + refill charged per I-cache line from the live tags (step-3); none = no deadline slicing (step-3 probe)")
    parser.add_argument("--deadline-cache", choices=("off", "on"), default="off",
                        help="step-4b: reuse device deadlines while the device-state generation is unchanged (PSX_DEADLINE_CACHE=1)")
    parser.add_argument("--slot-take", choices=("off", "on"), default="off",
                        help="take interrupts at compiled delay-slot boundaries the way exec_delay_slot does (PSX_SLICE_SLOT_TAKE=1)")
    args = parser.parse_args()
    if args.speed != "1" and not args.show:
        parser.error('--speed requires --show; headless playback is already uncapped')
    if args.ram_page_probe and args.field_model!='octoshock-2.2.2-ntsc-raster':
        raise ValueError('RAM page probe requires source raster frontend boundaries')
    if args.ram_snapshot_frame and (not args.ram_page_probe or len(args.ram_snapshot_frame)>64 or
            len(set(args.ram_snapshot_frame))!=len(args.ram_snapshot_frame) or
            any(f<1 for f in args.ram_snapshot_frame)):
        raise ValueError('RAM snapshots require page probe and 1..64 unique positive frames')
    if args.timer2_model!="default" and args.timer1_model!="octoshock-2.2.2":
        raise ValueError("timer2 model requires source timer1 model")
    if args.timer1_model!="default" and args.field_model!="octoshock-2.2.2-ntsc-raster":
        raise ValueError("timer1 model requires NTSC raster field model")
    if args.gpu_status_model!='default' and args.field_model!='octoshock-2.2.2-ntsc-raster':
        raise ValueError('GPU status model requires NTSC raster field model')
    if args.syscall_model!='default' and args.gpu_dma_model!='octoshock-2.2.2-bounded-quad':
        raise ValueError('guest syscall model requires the source CPU/service profile')
    if args.cd_drive_model!='default':
        if not args.cd_source_clock_tape:
            raise ValueError('source drive model requires a clock tape')
    if args.cd_init_draw_order!='default' and args.cd_drive_model=='default':
        raise ValueError('an Init draw order requires a source drive model')
    if args.cd_cdda_model!='default' and not args.cd_source_clock_tape:
        raise ValueError('source CDDA requires an explicit source clock tape')
    if args.mdec_source_model!='default' and args.gpu_dma_model!='octoshock-2.2.2-bounded-quad':
        raise ValueError('source MDEC requires the qualified source service scheduler')
    if args.gpu_dma_model=='octoshock-2.2.2-bounded-quad' and args.field_model!='octoshock-2.2.2-ntsc-raster':
        raise ValueError('GPU quad model requires NTSC raster field model')
    if (args.update_decisions or args.update_predictor != "gate") and not args.update_profile:
        raise ValueError('update decisions require an explicit update profile')
    if not 0 <= args.neutral_tail <= 60000:
        raise ValueError("neutral tail outside 0..60000 ticks")
    if not 1 <= args.checkpoint_every <= 10000:
        raise ValueError("checkpoint interval outside 1..10000")
    if args.storage_budget_mib is not None and not 1<=args.storage_budget_mib<=16384:
        raise ValueError('storage budget must be in 1..16384 MiB')
    if args.dump_at_frame is not None and args.dump_at_frame < 1:
        raise ValueError('dump-at-frame must be a frame count of 1 or more')
    if args.expected_exe_sha256 is not None:
        args.expected_exe_sha256=args.expected_exe_sha256.lower()
        if len(args.expected_exe_sha256)!=64 or any(c not in '0123456789abcdef' for c in args.expected_exe_sha256):
            raise ValueError('expected executable SHA-256 must be 64 hex digits')
    if len(args.watch_u16) > 32 or len(set(args.watch_u16)) != len(args.watch_u16) or any(
            a < 0 or a > 0x1ffffe or a % 2 for a in args.watch_u16):
        raise ValueError("watch addresses must be unique aligned physical RAM u16 offsets (max32)")
    if args.record_frame is not None and (args.record_frame < 0 or not args.watch_u16):
        raise ValueError("record-frame requires nonnegative frame and configured watches")
    if args.instruction_histogram:
        first,last=args.instruction_histogram
        if not 0<=first<=last<=60000 or last-first>7:
            raise ValueError('histogram requires one to eight bounded runtime intervals')
    if args.instruction_site and (not args.instruction_histogram or len(args.instruction_site)>16 or len(set(args.instruction_site))!=len(args.instruction_site) or any(p<0 or p>0xfffffffc or p%4 for p in args.instruction_site)):
        raise ValueError('instruction sites require histogram and unique aligned 32-bit PCs (max16)')
    if args.read_watch and (args.record_frame is None or
                           not 0 <= args.read_watch[0] < args.read_watch[1] <= 0x200000 or
                           args.read_watch[1] - args.read_watch[0] > 4096):
        raise ValueError("read-watch requires record-frame and a RAM range of at most 4096 bytes")
    spu_keyoff_env = spu_keyoff_resume_environment(args.spu_keyoff_model, args.resume_spu_keyoff_from,
                                                  args.resume_from, args.resume_compatible_build, args.gpu_dma_model)
    paths = {name: getattr(args, name).resolve(strict=True)
             for name in ("exe", "game", "route", "disc", "bios")}
    clock_tape = source_clock_identity(args.cd_source_clock_tape, args.cd_toc_seek_model,
                                       args.cd_explicit_seek_model, args.cd_read_start_model)
    legacy_clock_env = legacy_clock_resume_environment(
        args.resume_legacy_clock_path, args.resume_legacy_clock_sha256,
        args.resume_from, args.resume_compatible_build, clock_tape)
    if clock_tape:
        paths['cd_source_clock_tape'] = Path(clock_tape['path'])
    clock_extension = None
    if args.cd_source_clock_tape_extension:
        if not clock_tape:
            raise ValueError('source clock extension requires its original base tape')
        clock_extension = source_clock_identity(args.cd_source_clock_tape_extension,
            args.cd_toc_seek_model,args.cd_explicit_seek_model,args.cd_read_start_model)
        base = paths['cd_source_clock_tape'].read_bytes()
        extended = Path(clock_extension['path']).read_bytes()
        if clock_extension['words'] <= clock_tape['words'] or extended[20:len(base)] != base[20:]:
            raise ValueError('source clock extension differs from original prefix')
        paths['cd_source_clock_tape_extension'] = Path(clock_extension['path'])
    identity = route_identity(paths["route"], axis_model=args.axis_model, mode_model=args.mode_button_model, neutral_p2=args.port2_controller != 'none')
    resume_frame = resume_inputs = 0
    if args.resume_compatible_build and args.resume_from is None:
        raise ValueError('--resume-compatible-build requires --resume-from')
    if args.resume_from is not None:
        resume_manifest = json.loads(Path(str(args.resume_from)+'.json').read_text())
        resume_frame, resume_inputs = checkpoint_interval(resume_manifest, identity['frames'], args.neutral_tail,args.terminal_return_probe)
        if (args.resume_from.stat().st_size != resume_manifest.get('state_bytes') or
                digest(args.resume_from) != resume_manifest.get('state_sha256')):
            raise ValueError('checkpoint size or SHA256 mismatch')
        if not checkpoint_cards_valid(args.resume_from, resume_manifest):
            raise ValueError('checkpoint card image missing or SHA256 mismatch')
        if args.full_input_audit:
            validate_audited_resume(args.resume_from,resume_manifest,digest(paths['exe']),args.resume_compatible_build)
    completion_identity = identity if args.full_input_audit else route_identity(paths['route'], resume_inputs, args.axis_model, args.mode_button_model, args.port2_controller != 'none')
    roster = disc_roster(identity, paths['disc'], args.roster_disc, paths['game'],
                         args.resume_relocate_discs, args.resume_from, args.resume_compatible_build)
    if identity.get('format') == 'PSXRTI3' and identity.get('controller_layout') != 'dualshock' and (args.resume_from or args.save_state_at or args.update_profile
                                                or args.card1 or args.card2 or args.port2_controller != 'none'
                                                or args.pad_ack_model.endswith('dualshock')):
        raise ValueError('a two-pad route admits no resume, save state, retiming, card or DualShock ACK model')

    dualshock = identity.get('format') == 'PSXRTI2' or identity.get('controller_layout') == 'dualshock'
    if args.full_input_audit and not dualshock:
        raise ValueError('full input audit requires a complete DualShock route')
    if dualshock and (args.update_profile or args.pad_ack_model in ('octoshock-2.2.2-digital', 'nymashock-2.8-gamepad')):
        raise ValueError('DualShock does not admit retiming, the digital Octoshock ACK model or the 2.8 plain-gamepad ACK model')
    initial_card = None
    if args.card1:
        if args.card_model not in ('nymashock-1.29.0','nymashock-1.32.1','nymashock-2.8') or args.legacy_card_repair != 'off':
            raise ValueError('card replay requires an explicit source card model and legacy repair off')
        paths['card1'] = args.card1.resolve(strict=True)
        initial_card = card_identity(paths['card1'])
    elif args.card_model != 'default':
        raise ValueError('card model requires an explicit initial card1')
    initial_card2 = None
    if args.card2:
        if not initial_card:
            raise ValueError('card2 requires an explicit initial card1')
        paths['card2'] = args.card2.resolve(strict=True)
        initial_card2 = card_identity(paths['card2'])
    if args.port2_controller != 'none' and not dualshock:
        raise ValueError('neutral DualShock P2 requires a DualShock route')
    if (initial_card2 or args.port2_controller != 'none') and (args.resume_from or args.save_state_at) and not args.full_input_audit:
        raise ValueError('two-card/neutral-P2 continuation requires the complete actual input audit checkpoint')
    # The completion hook exits before the final return observer. Bind every
    # enabled RAM capture to the exact declared number of observed returns.
    if args.terminal_return_probe and (not args.cpu_return_probe or not args.ram_page_probe or
            args.neutral_tail or (args.resume_from and not args.full_input_audit) or args.update_profile or
            args.field_model!='octoshock-2.2.2-ntsc-raster' or args.gpu_dma_model!='octoshock-2.2.2-bounded-quad'):
        raise ValueError('terminal return probe requires original raster/GPU profile,both CPU/RAM probes,zero tail,and verified audit history on resume')
    ram_returns = identity['frames'] + args.neutral_tail - (0 if args.terminal_return_probe else 1)
    watcher = None
    if args.stock_ram_only and args.stock_reference is None:
        raise ValueError('RAM-only comparison requires an original stock reference')
    if args.stock_reference is not None:
        if not args.cpu_return_probe or not args.ram_page_probe or (resume_frame and not args.full_input_audit):
            raise ValueError('stock comparison requires both return probes and original boot or verified actual audit history')
        from stream_compare import Watcher, stock_reference, stock_ram_reference
        source_pages = args.stock_reference / 'pages.tsv'
        if args.stock_ram_only and (args.stock_reference/'pages-segments.json').is_file():
            source_pages=args.stock_reference/'pages-segments.json'
        source_cpu = args.stock_reference / 'cpu-actual.tsv'
        if args.stock_ram_only:
            if source_cpu.exists() or not (args.stock_reference / 'original-registers-unavailable.json').is_file():
                raise ValueError('RAM-only comparison requires an explicit original missing-CPU receipt and no CPU table')
        reference_rows = (lambda: stock_ram_reference(source_pages,ram_returns,first_frame=resume_frame+1)) if args.stock_ram_only else (lambda: stock_reference(source_pages,source_cpu,ram_returns,first_frame=resume_frame+1))
        comparison_kind = 'stock_ram_pages' if args.stock_ram_only else 'stock_cpu_pages'
        # Validate retained reference coverage before any native process starts.
        if sum(1 for _ in reference_rows()) != ram_returns-resume_frame:
            raise ValueError('incomplete original stock reference')
        watcher = Watcher(args.run_directory.resolve(),reference_rows(),ram_returns,
                          kind=comparison_kind,stop_on_divergence=not args.continue_on_divergence,first_frame=resume_frame+1)
    observer_limit=dualshock_route.V3_MAX_FRAMES if identity.get('capacity_version')==1 else 1000000
    if (args.ram_page_probe or args.cpu_return_probe) and not 1 <= ram_returns <= observer_limit:
        raise ValueError(f'RAM capture requires 1..{observer_limit} declared completed returns')
    if any(frame > ram_returns for frame in args.ram_snapshot_frame):
        raise ValueError('RAM snapshot exceeds the declared completed-return boundary')
    if args.save_state_at and (len(set(args.save_state_at)) != len(args.save_state_at) or
            any(not resume_frame < frame <= ram_returns for frame in args.save_state_at)):
        raise ValueError('save-state returns must be unique and inside the executed interval')
    if args.resume_from and args.update_profile:
        raise ValueError('checkpoint resume does not support input retiming')
    update_profile = None
    update_contexts = None
    context_values = None
    protected_mask = None
    if args.update_profile:
        update_profile=json.loads(args.update_profile.read_text())
        if update_profile['schema']!='psx-input-update-profile-v1':
            raise ValueError('unsupported update profile')
        receipt=Path(update_profile['derivation_receipt'])
        derived=json.loads(receipt.read_text())
        if digest(receipt)!=update_profile['derivation_sha256'] or derived['route']!=identity:
            raise ValueError('update route/derivation identity mismatch')
        if derived.get('contexts'):
            update_contexts=derived['contexts']
            context_path=Path(update_contexts['path'])
            if digest(context_path)!=update_contexts['sha256']:
                raise ValueError('source context sidecar changed')
            context_values=decode_contexts(context_path.read_bytes(),identity['frames'])
        if 'protected_raw_mask' in update_profile:
            protected_mask=update_profile['protected_raw_mask']
            if context_values is None or not isinstance(protected_mask,int) or not 0<protected_mask<=65535:
                raise ValueError('protected raw mask requires source contexts and a nonzero u16 mask')
    refresh_guard = None
    if update_profile and 'neutral_refresh_guard' in update_profile:
        refresh_guard=update_profile['neutral_refresh_guard']
        if protected_mask is None or not isinstance(refresh_guard,list) or len(refresh_guard)!=4 or any(
            not isinstance(v,int) or v<0 or (v>0xffffffff or v%4 if i==1 else v>0x1ffffe or v%2)
            for i,v in enumerate(refresh_guard)):
            raise ValueError('invalid neutral refresh guard')
    run = args.run_directory.resolve()
    run.mkdir(exist_ok=False, parents=True)
    relocation_path = None
    if args.resume_relocate_discs:
        relocation_path = run / 'disc-relocation.tsv'
        relocation_path.write_text(disc_relocation_text(digest(paths['game']), identity['sha256'], roster), encoding='utf-8', newline='\n')
    # This runtime loads settings and card paths beside argv[0], not CWD.
    # A private copy also freezes the executable while later builds proceed.
    launch_exe = run / paths["exe"].name
    shutil.copy(paths["exe"], launch_exe)
    staged_sha = digest(launch_exe)
    if staged_sha != digest(paths["exe"]):
        raise ValueError("staged executable differs")
    # Binary identity gate: after staging, before settings, manifest and launch.
    if args.expected_exe_sha256 is not None and staged_sha != args.expected_exe_sha256:
        raise ValueError(f'staged executable {launch_exe} has SHA-256 {staged_sha}; expected {args.expected_exe_sha256}')
    staged_card = None
    if initial_card:
        (run / 'cards').mkdir()
        staged_card = run / 'cards/card1.mcd'
        shutil.copyfile(paths['card1'], staged_card)
        if digest(staged_card) != initial_card['sha256']:
            raise ValueError('staged initial card differs')
    staged_card2 = None
    if initial_card2:
        staged_card2 = run / 'cards/card2.mcd'
        shutil.copyfile(paths['card2'], staged_card2)
        if digest(staged_card2) != initial_card2['sha256']:
            raise ValueError('staged second card differs')
    route_path = paths['route']
    if dualshock:
        route_path = run / 'input.psxrti2'
        shutil.copyfile(paths['route'], route_path)
        if digest(route_path) != identity['sha256']:
            raise ValueError('staged controller route differs')
    renderer = args.renderer
    settings = settings_text(renderer, args.fast_boot, args.hle, paths['bios'], paths['disc'],
                             initial_card is not None, initial_card2 is not None)
    (run / "settings.toml").write_text(settings, encoding="utf-8")
    command = [str(launch_exe), "--game", str(paths["game"]),
               "--disc", str(paths["disc"]), "--bios", str(paths["bios"]),
               "--no-launcher", "--debug-port", str(args.debug_port),
               "--renderer", renderer]
    if not args.show:
        command.append("--headless")
    # A replay cannot inherit unrelated diagnostic, load-state or enhancement
    # switches from another task. Retain the complete selected PSX environment.
    env = {k: v for k, v in os.environ.items() if not k.startswith("PSX_")}
    selected_env = {"PSX_INPUT_ROUTE_FILE": str(route_path),
                    "PSX_INPUT_ROUTE_CAPTURE_DIR": str(run),
                    "PSX_HLE_SCHEDULER": "1" if args.scheduler == "hle" else "0",
                    "PSX_LOW_LATENCY_INPUT": "0",
                    "PSX_INPUT_ROUTE_NEUTRAL_TAIL": str(args.neutral_tail),
                    "PSX_INPUT_ROUTE_TRACE": "1" if args.sio_trace else "0",
                    "PSX_INPUT_ROUTE_CAPTURE_EVERY": str(args.checkpoint_every),
                    "PSX_BIOS_HLE": "1" if args.hle else "0",
                    # The runtime's 4 s starvation watchdog is wall-clock. A host stall killed
                    # two unrelated multi-hour replays in the same second (2026-09-17 06:10);
                    # the launcher's own --timeout bounds a real hang.
                    "PSX_STARVATION_TIMEOUT_US": "120000000"}
    selected_env.update(legacy_clock_env)
    if relocation_path is not None:
        selected_env['PSX_TAS_RESUME_DISC_RELOCATION'] = str(relocation_path)
    if args.full_input_audit:
        selected_env['PSX_INPUT_ROUTE_FULL_AUDIT']='1'
    if identity.get('capacity_version')==1:
        selected_env['PSX_SOURCE_OBSERVER_CAPACITY_V1']='1'
    if args.speed != "1":
        selected_env["PSX_FAST_FORWARD"] = "1"
        selected_env["PSX_FAST_FORWARD_SPEED"] = args.speed
    if refresh_guard is not None:
        selected_env["PSX_INPUT_UPDATE_REFRESH_GUARD"]=','.join(map(str,refresh_guard))
    if args.cd_read_start_model != "default":
        selected_env["PSX_CD_READ_START_MODEL"] = args.cd_read_start_model
    if clock_tape:
        selected_env['PSX_CD_SOURCE_CLOCK_TAPE'] = clock_tape['path']
    if clock_extension:
        selected_env['PSX_CD_SOURCE_CLOCK_TAPE_EXTENSION'] = clock_extension['path']
    if args.cd_drive_model!='default':
        selected_env['PSX_CD_DRIVE_MODEL']=args.cd_drive_model
    if args.cd_tray_model != 'default':
        if args.cd_drive_model != 'octoshock-2.7':
            raise ValueError('source2.10 tray requires the existing measured2.7 drive profile')
        selected_env['PSX_CD_TRAY_MODEL']=args.cd_tray_model
    if args.cd_init_draw_order!='default':
        selected_env['PSX_CD_INIT_DRAW_ORDER']=args.cd_init_draw_order
    if args.cd_cdda_model!='default':
        selected_env['PSX_CD_CDDA_MODEL']=args.cd_cdda_model
    if args.mdec_source_model!='default':
        selected_env['PSX_MDEC_SOURCE_MODEL']=args.mdec_source_model
    if args.cd_dma_model!='default':
        selected_env['PSX_CD_DMA_MODEL']=args.cd_dma_model
    if args.cd_toc_seek_model != "default":
        selected_env["PSX_CD_TOC_SEEK_MODEL"] = args.cd_toc_seek_model
    if args.cd_explicit_seek_model != "default":
        selected_env["PSX_CD_EXPLICIT_SEEK_MODEL"] = args.cd_explicit_seek_model
    if args.cd_firmware_model != "default":
        selected_env["PSX_CD_FIRMWARE_MODEL"] = args.cd_firmware_model
    if args.cd_xa_eof_model != "default":
        selected_env["PSX_CD_XA_EOF_MODEL"] = args.cd_xa_eof_model
    if args.cd_cold_status_model != "default":
        selected_env["PSX_CD_COLD_STATUS_MODEL"] = args.cd_cold_status_model
    if args.syscall_model != "default":
        selected_env["PSX_SYSCALL_MODEL"] = args.syscall_model
    if args.critical_section_model != "direct":
        selected_env["PSX_CRITICAL_SECTION_MODEL"] = args.critical_section_model
    if args.field_model != "default":
        selected_env["PSX_INPUT_ROUTE_FIELD_MODEL"] = args.field_model
    if args.gpu_status_model!='default':
        selected_env['PSX_GPU_STATUS_MODEL']=args.gpu_status_model
    if args.timer1_model!='default':
        selected_env['PSX_TIMER1_MODEL']=args.timer1_model
    if args.timer2_model!='default':
        selected_env['PSX_TIMER2_MODEL']=args.timer2_model
    selected_env['PSX_PRECISE_SLICE']='1' if args.precise_slice=='on' else '0'
    if args.slice_gpu_deadline != 'tick':
        selected_env['PSX_SLICE_GPU_DEADLINE'] = args.slice_gpu_deadline
    if args.slice_bound != 'steal':
        selected_env['PSX_SLICE_BOUND'] = args.slice_bound
    if args.deadline_cache == 'on':
        selected_env['PSX_DEADLINE_CACHE'] = '1'
    if args.slot_take == 'on':
        selected_env['PSX_SLICE_SLOT_TAKE'] = '1'
    if args.save_state_at:
        selected_env['PSX_TAS_SAVE_STATE_AT'] = ','.join(str(f) for f in args.save_state_at)
    if args.resume_from is not None:
        selected_env['PSX_TAS_RESUME_STATE'] = str(args.resume_from)
    if args.resume_compatible_build:
        selected_env['PSX_TAS_RESUME_COMPATIBLE_BUILD'] = '1'
    if args.save_state_at or args.resume_from:
        for asset in ('game', 'bios', 'disc'):
            selected_env['PSX_TAS_ASSET_' + asset.upper() + '_SHA256'] = checkpoint_asset_digest(paths[asset])
    if args.perturb_restore is not None:
        selected_env['PSX_TAS_PERTURB_RESTORE'] = args.perturb_restore
    if args.e_survey:
        selected_env['PSX_E_SURVEY'] = '1'
    if args.dump_at_frame is not None:
        # The shell's own PSX_DUMP_AT_FRAME was removed above with the rest; only this option sets it.
        selected_env['PSX_DUMP_AT_FRAME'] = str(args.dump_at_frame)
    selected_env.update(spu_keyoff_env)
    if args.gpu_dma_model!='default':
        selected_env['PSX_GPU_DMA_MODEL']=args.gpu_dma_model
    if args.legacy_card_repair == "off":
        selected_env["PSX_APE_CARD_UNSTICK"] = "0"
    if args.pad_ack_model != "default":
        selected_env["PSX_INPUT_ROUTE_PAD_ACK_MODEL"] = args.pad_ack_model
    if args.axis_model != DEFAULT_AXIS_MODEL:
        selected_env["PSX_INPUT_ROUTE_AXIS_MODEL"] = args.axis_model
    if args.mode_button_model != 'none':
        if not dualshock or (args.resume_from and not args.full_input_audit):
            raise ValueError('the MODE model needs a DualShock route with verified full input-state continuation')
        selected_env['PSX_INPUT_ROUTE_MODE_MODEL'] = args.mode_button_model
    if initial_card:
        selected_env['PSX_INPUT_ROUTE_CARD_MODEL'] = args.card_model
        selected_env['PSX_INPUT_ROUTE_CARD1_SHA256'] = initial_card['sha256']
    if initial_card2:
        selected_env['PSX_INPUT_ROUTE_CARD2_SHA256'] = initial_card2['sha256']
    if args.port2_controller != 'none':
        selected_env['PSX_INPUT_ROUTE_PORT2'] = args.port2_controller
    if args.dma_model != "default":
        selected_env["PSX_INPUT_ROUTE_DMA_MODEL"] = args.dma_model
    if args.instruction_histogram:
        selected_env['PSX_INPUT_HISTOGRAM_RANGE']=','.join(map(str,args.instruction_histogram))
    if args.instruction_site:
        selected_env['PSX_INPUT_HISTOGRAM_SITES']=','.join(f'{p:08X}' for p in args.instruction_site)
    if args.cpu_boundary_window:
        low,high=args.cpu_boundary_window
        if low<0 or high<=low or high-low>1000000:raise ValueError('invalid CPU boundary window')
        selected_env['PSX_SOURCE_CPU_BOUNDARY_WINDOW']=f'{low},{high}'
    if args.gpu_command_window:
        low,high=args.gpu_command_window
        if low<0 or high<=low or high-low>1000000:raise ValueError('invalid GPU command window')
        selected_env['PSX_SOURCE_GPU_COMMAND_WINDOW']=f'{low},{high}'
    if args.device_history_probe:
        if not args.cpu_return_probe:raise ValueError('Device history requires CPU return probe')
        selected_env['PSX_SOURCE_DEVICE_HISTORY_PROBE']='1'
    if args.cpu_return_probe:
        selected_env['PSX_SOURCE_CPU_RETURN_PROBE']='1'
        selected_env['PSX_SOURCE_CPU_MAX_FRAMES']=str(ram_returns)
    if args.ram_page_probe:
        selected_env['PSX_SOURCE_RAM_PAGE_PROBE']='1'
        selected_env['PSX_SOURCE_RAM_MAX_FRAMES']=str(ram_returns)
    if args.ram_snapshot_frame:
        selected_env['PSX_SOURCE_RAM_SNAPSHOT_FRAMES']=','.join(map(str,args.ram_snapshot_frame))
    if args.update_predictor != "gate":
        selected_env["PSX_INPUT_UPDATE_PREDICTOR"] = args.update_predictor
    if args.watch_u16:
        selected_env["PSX_INPUT_ROUTE_WATCH_U16"] = ",".join(hex(a) for a in args.watch_u16)
    if args.record_frame is not None:
        selected_env["PSX_RECORD_FRAME"] = str(args.record_frame)
    if args.cpu_state:
        selected_env["PSX_INPUT_ROUTE_CPU_STATE"] = "1"
    if args.video_state:
        selected_env["PSX_INPUT_ROUTE_VIDEO_STATE"] = "1"
    if args.read_watch:
        selected_env["PSX_READ_WATCH"] = ",".join(hex(a) for a in args.read_watch)
    if update_profile:
        fields=('marker_address','marker_pc','marker_ra','gate_address','packet_address',
                'header_address','expected_header','max_vblank_ticks','stall_vblank_ticks',
                'marker_register','marker_register_value','auxiliary_register_value')
        selected_env['PSX_INPUT_UPDATE_CLOCK']=','.join(str(update_profile[k]) for k in fields)
    if args.update_decisions:
        shutil.copyfile(args.update_decisions,run/'update-decisions.txt')
        selected_env['PSX_INPUT_UPDATE_DECISIONS']=str(run/'update-decisions.txt')
    if update_contexts:
        shutil.copyfile(update_contexts['path'],run/'input.contexts')
        selected_env['PSX_INPUT_UPDATE_CONTEXTS']=str(run/'input.contexts')
    if protected_mask is not None:
        selected_env['PSX_INPUT_UPDATE_PROTECTED_RAW_MASK']=str(protected_mask)
    env.update(selected_env)
    if args.terminal_return_probe:
        selected_env['PSX_SOURCE_TERMINAL_RETURN']='1'
        env['PSX_SOURCE_TERMINAL_RETURN']='1'
    write_json(run / "manifest.json", {
        "schema": "psx-native-tas-run-v1", "command": command,
        "terminal_return_probe": args.terminal_return_probe,
        "resume": {"from":str(args.resume_from) if args.resume_from else None,
                   "compatible_build":args.resume_compatible_build,
                   "saved_exe_sha256":resume_manifest.get('exe_sha256') if args.resume_from else None,
                   "running_exe_sha256":digest(paths['exe']),"actual_input_audit":args.full_input_audit},
        "continue_on_divergence":args.continue_on_divergence,
        "stock_reference": ({"directory": str(args.stock_reference.resolve()),
                              "pages_sha256": digest(source_pages), "cpu_sha256": None if args.stock_ram_only else digest(source_cpu),
                              "comparison_scope": "RAM only;source CPU unavailable" if args.stock_ram_only else "CPU and RAM",
                              "comparator_sha256": digest(Path(__file__).with_name('stream_compare.py')),
                              "source_absolute_cycles": "unavailable; not compared or inferred"}
                             if watcher is not None else None),
        "requested_speed": args.speed if args.show else "headless-uncapped",
        "update_profile":update_profile,
        "update_contexts":update_contexts,
        "update_profile_sha256":digest(args.update_profile) if update_profile else None,
        "update_decisions_sha256":digest(args.update_decisions) if args.update_decisions else None,
        "inputs": {name: {"path": str(path), "sha256": digest(path)}
                   for name, path in paths.items()},
        "staged_executable": {"path": str(launch_exe), "sha256": staged_sha},
        "expected_exe_sha256": args.expected_exe_sha256,
        "route": identity, "disc_roster": roster, "psx_environment": selected_env,
        "initial_card1":initial_card, "initial_card2":initial_card2,
        "staged_card2":str(staged_card2) if staged_card2 else None,
        "port2_controller":args.port2_controller,
        "staged_card1":str(staged_card) if staged_card else None,
        "settings_sha256": digest(run / "settings.toml"),
        "launcher_sha256": digest(Path(__file__)),
        "boundary": "N records supplied before the next normal VBlank sample",
        "guest_controller_polls_measured": False,
        "source_timing_equivalence": "unqualified", "timeout_seconds": args.timeout,
        "storage_budget_mib":args.storage_budget_mib})
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 1 if args.show else 0
    start = time.monotonic()
    timed_out = False
    with (run / "stdout.log").open("xb") as out, (run / "stderr.log").open("xb") as err:
        process = subprocess.Popen(command, cwd=run, env=env, startupinfo=startup,
                                   stdout=out, stderr=err)
        write_json(run / "process.json", {"pid": process.pid})
        print(f"Native PID {process.pid}; evidence: {run}", flush=True)
        if watcher is not None:
            watcher.start()
        from process_budget import wait_budgeted
        # A harness (e.g. stream_compare) stops playback by publishing <run>/stop-request.json.
        stop_path=run/'stop-request.json';stop_request={}
        def harness_stop():
            if not stop_path.exists():return None
            try:stop_request['content']=json.loads(stop_path.read_text(encoding='utf-8'))
            except (OSError,ValueError) as error:stop_request['content']={'unreadable':str(error)}
            return 'harness_stop'
        budget=wait_budgeted(process,run,args.timeout,
                             args.storage_budget_mib*1024**2 if args.storage_budget_mib is not None else None,
                             stop_requested=harness_stop)
        code=budget['exit_code']
        timed_out=budget['stop_reason']=='host_timeout'
    write_json(run / "exit.json", {"pid": process.pid, "exit_code": code,
               "stop_reason":budget['stop_reason'],"stop_request":stop_request.get('content'),
               "diagnostic_inventory":budget['last_inventory'],
               "timed_out": timed_out, "host_seconds": time.monotonic() - start,
               "completion_exists": (run / "complete.json").exists()})
    comparison = None
    if watcher is not None:
        comparison = watcher.finish(30)
        write_json(run / 'stock-stream-comparison.json', comparison)
        final_watcher = Watcher(run,reference_rows(),ram_returns,
                                kind=comparison_kind,poll=0.01,first_frame=resume_frame+1)
        final_watcher.start()
        # This is a bounded FILE comparison after exit, not a live worker.
        # A million-row CPU/RAM table can legitimately take longer than30s.
        comparison = final_watcher.finish(None)
        write_json(run / 'stock-final-comparison.json', comparison)
    complete = None
    if (run / "complete.json").exists():
        complete = json.loads((run / "complete.json").read_text())
    qualified = (budget['stop_reason'] is None and code == 0 and
                 playback_identity_matches(complete, completion_identity, args.neutral_tail) and
                 complete.get("resumed_inputs", 0) == resume_inputs)
    if args.full_input_audit:
        try:
            audit_validation=validate_full_input_audit(run,completion_identity,args.neutral_tail,args.port2_controller!='none',roster)
        except (OSError,ValueError,KeyError,IndexError) as error:
            audit_validation={'valid':False,'error':str(error)}
        write_json(run/'full-input-audit-validation.json',audit_validation)
        qualified=qualified and audit_validation['valid']
    if args.save_state_at:
        saved = resolve_saved_states(run,args.save_state_at,require_input_history=args.full_input_audit)
        write_json(run / 'saved-states.json', saved)
        qualified = qualified and all(item['valid'] for item in saved)
    if args.port2_controller != 'none':
        qualified = qualified and complete is not None and complete.get('port2_controller') == args.port2_controller
    if initial_card or initial_card2 or dualshock:
        initial_path = run / 'initial-cards.json'
        actual_cards = json.loads(initial_path.read_text()) if initial_path.exists() else None
        expected_cards = {'card1_present':bool(initial_card), 'card2_present':bool(initial_card2),
                          'card1_sha256':initial_card['sha256'] if initial_card else None,
                          'bytes':131072 * (bool(initial_card) + bool(initial_card2))}
        if initial_card2:
            expected_cards['card2_sha256'] = initial_card2['sha256']
        qualified = qualified and actual_cards == expected_cards
    if update_profile:
        endpath=run/'update-input-end.json'
        end=json.loads(endpath.read_text()) if endpath.exists() else None
        qualified=(budget['stop_reason'] is None and code==0 and complete is not None and end is not None
                   and end['accepted_samples']==identity['frames']
                   and end['accepted_words_sha256']==identity['words_sha256']
                   and complete['frame']==end['frame']+args.neutral_tail
                   and complete['neutral_tail_ticks']==args.neutral_tail)
        if context_values is not None:
            trace=run/'update-clock.jsonl'
            events=[json.loads(x) for x in trace.read_text().splitlines()] if trace.exists() else []
            actual=[(x['sequence'],x['context']) for x in events if x['kind']=='accepted_context']
            context_match=actual==list(enumerate(context_values,1))
            sequence_match=[i for i,_ in actual]==list(range(1,len(context_values)+1))
            data=paths['route'].read_bytes()
            words=[struct.unpack_from('<H',data,24+8*i+4)[0] for i in range(identity['frames'])]
            effects_match=(context_match if protected_mask is None else sequence_match and
                           check_protected_effects(context_values,[c for _,c in actual],words,protected_mask))
            write_json(run/'context-check.json',{'match':effects_match,'strict_context_match':context_match,
                       'protected_raw_mask':protected_mask,'actual_events':len(actual),
                       'context_differences':sum(c!=expected for (_,c),expected in zip(actual,context_values)),
                       'normal_events':sum(c==0 for _,c in actual),'auxiliary_events':sum(c==1 for _,c in actual),
                       'expected_sha256':update_contexts['sha256'],
                       'additional_neutral_refreshes':sum(x['kind']=='neutral_refresh' for x in events)})
            qualified=qualified and effects_match
    if args.gpu_command_window:
        low,high=args.gpu_command_window
        if low<0 or high<=low or high-low>1000000:raise ValueError('invalid GPU command window')
        selected_env['PSX_SOURCE_GPU_COMMAND_WINDOW']=f'{low},{high}'
    if args.cpu_return_probe:
        from observation_evidence import validate_cpu_capture
        try:
            cpu_validation=validate_cpu_capture(run,ram_returns,first_frame=resume_frame+1)
        except (ValueError,OSError) as error:
            cpu_validation={'valid':False,'error':str(error)}
        write_json(run/'cpu-capture-validation.json',cpu_validation)
        qualified=qualified and cpu_validation['valid']
    if args.ram_page_probe:
        from compare_ram_pages import validate_capture
        try:
            # The completion hook exits before the terminal return observer.
            ram_validation = validate_capture(run, ram_returns, args.ram_snapshot_frame, first_frame=resume_frame+1)
        except (ValueError, OSError, StopIteration) as error:
            ram_validation = {'valid': False, 'error': str(error)}
        write_json(run / 'ram-capture-validation.json', ram_validation)
        qualified = qualified and ram_validation['valid']
    if comparison is not None:
        qualified = (qualified and comparison['complete_through_endpoint'] and comparison['first_divergence'] is None
                     and not comparison['observation_error'] and not comparison['watcher_error'])
    print(json.dumps({"input_playback_complete": qualified, "exit_code": code,
                      "stock_comparison_complete": (comparison is not None and comparison['complete_through_endpoint']
                          and comparison['first_divergence'] is None and not comparison['observation_error']
                          and not comparison['watcher_error']),
                      "gameplay_equivalence": "unclassified",
                      "resumed_inputs": resume_inputs, "observed_from_return": resume_frame+1,
                      "full_from_boot":resume_frame==0,
                      "comparison_scope":("RAM only;original CPU unavailable" if args.stock_ram_only else "this process's observed CPU/RAM interval;resumed prefixes need their retained earlier comparison records")}), flush=True)
    return 0 if qualified else 1


if __name__ == "__main__":
    raise SystemExit(main())
