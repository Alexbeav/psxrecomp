"""Lossless bounded PSXRTI2 and explicit capacity-v1 PSXRTI3 DualShock input.

Legacy readers/writers keep their original format and bounds. The full
Octoshock decoder uses original LogKey column names, raw axes and MODE,
refuses non-neutral P2, and retains the original console fields. PSXRTI3
carries ordered physical tray/select/close events and a digest-bound roster.
Controller/core/timing equivalence remains separate from delivery accounting.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import zipfile

MAX_FRAMES = 1_000_000
# Must equal INPUT_ROUTE_MAX_STEPS in runtime/include/input_route_file.h; the
# runtime refuses a route past it. See that header for why it moved off 4096.
MAX_STEPS = 65536
HEADER = struct.Struct('<8sIIII')
RECORD = struct.Struct('<IH6B')
CONTROLLER = struct.Struct('<H5B')
LOGKEY = ('LogKey:#Disk Index|Power|Reset|Open Tray|Close Tray|'
          '#P1 Left Stick Up / Down|P1 Left Stick Left / Right|'
          'P1 Right Stick Up / Down|P1 Right Stick Left / Right|'
          'P1 D-Pad Up|P1 D-Pad Down|P1 D-Pad Left|P1 D-Pad Right|'
          'P1 Select|P1 Start|P1 △|P1 X|P1 □|P1 ○|P1 L1|P1 L2|'
          'P1 R1|P1 R2|P1 Left Stick, Button|P1 Right Stick, Button|P1 Analog|')
BUTTON_BITS = (4, 6, 7, 5, 0, 3, 12, 14, 15, 13, 10, 8, 11, 9, 1, 2)
MEMBERS = {'BizState 1.0', 'BizVersion.txt', 'Header.txt', 'Comments.txt',
           'Subtitles.txt', 'SyncSettings.json', 'Input Log.txt'}
# BizHawk2.8 Nymashock P1 layout: four console bools, then RX,LX,RY,LY as u16
# and the same seventeen button characters. 2.9.1 lands its log byte in the
# high byte of the same u16 field, so 2.8 v16 is exactly v16>>8 only when the
# low byte is zero; anything else has no lossless 2.9.1 spelling.
LOGKEY_28 = ('LogKey:#Power|Reset|Previous Disk|Next Disk|'
             '#P1 Right Stick Left / Right|P1 Left Stick Left / Right|'
             'P1 Right Stick Up / Down|P1 Left Stick Up / Down|'
             'P1 D-Pad Up|P1 D-Pad Down|P1 D-Pad Left|P1 D-Pad Right|'
             'P1 Select|P1 Start|P1 △|P1 X|P1 □|P1 ○|P1 L1|P1 L2|'
             'P1 R1|P1 R2|P1 Left Stick, Button|P1 Right Stick, Button|P1 Analog|')
MEMBERS_28 = {'Header.txt', 'Comments.txt', 'Subtitles.txt', 'SyncSettings.json', 'Input Log.txt'}
# BizHawk 2.7 Octoshock with a DualShock on port 1 (FIOConfig Devices8[0] = 2): a disc-select
# number and Open/Close/Reset, then LX,LY,RX,RY and seventeen button characters in IO_Dualshock
# order. Octoshock hands the stick bytes to the pad unscaled (Nymashock rescales them), and the
# MODE check happens only when DTR drops; neither difference can show while every stick is 128
# and MODE is never pressed, so only those records have an exact PSXRTI2 spelling.
LOGKEY_OCTO27_DUALSHOCK = ('LogKey:#Disc Select|Open|Close|Reset|'
                           '#P1 LStick X|P1 LStick Y|P1 RStick X|P1 RStick Y|'
                           'P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|'
                           'P1 Square|P1 Triangle|P1 Circle|P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|'
                           'P1 L3|P1 R3|P1 MODE|')
BUTTON_BITS_OCTO = (4, 6, 7, 5, 0, 3, 15, 12, 13, 14, 10, 11, 8, 9, 1, 2)


def read_movie(movie, emu_version='Version 2.9.1', container_version='Version 2.9.1'):
    """Return (buttons-active-low, LY,LX,RY,RX,physical-Analog) per input.

    emu_version is the declared header provenance; a 2.8 movie re-encoded
    into this layout keeps its original header, so its reader passes 'Version 2.8'.
    container_version is the BizVersion.txt marker the host wrote; a 2.10 Nymashock
    movie uses the same log key and container layout as 2.9.1, only this marker differs.
    """
    with zipfile.ZipFile(movie) as archive:
        if len(archive.namelist()) != len(MEMBERS) or set(archive.namelist()) != MEMBERS:
            raise ValueError('Unsupported movie payload; cold input-only movie required')
        if (archive.read('BizState 1.0') != b'2\r\n' or
                archive.read('BizVersion.txt') != (container_version + '\r\n').encode()):
            raise ValueError('Unsupported movie container version')
        header = dict(line.split(' ', 1) for line in archive.read('Header.txt').decode().splitlines()
                      if ' ' in line)
        if (header.get('Core') != 'Nymashock' or header.get('Platform') != 'PSX' or
                header.get('emuVersion') != emu_version):
            raise ValueError('Only the declared Nymashock2.9.1 PSX log layout is supported')
        for field in ('StartsFromSavestate', 'StartsFromSaveRam', 'StartsFromSaveRAM'):
            if header.get(field, '').lower() not in ('', 'false', '0'):
                raise ValueError('Anchored movie is not a cold controller route')
        lines = archive.read('Input Log.txt').decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    rows = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '    0,....':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if len(controller) != 5 or len(controller[4]) != 17:
            raise ValueError('Malformed controller record')
        axes = tuple(int(value) for value in controller[:4])
        if any(not 0 <= value <= 255 for value in axes):
            raise ValueError('Axis outside byte range')
        word = 0xFFFF
        for character, bit in zip(controller[4][:16], BUTTON_BITS):
            if character != '.':
                word &= ~(1 << bit)
        rows.append((word, *axes, int(controller[4][16] != '.')))
        if len(rows) > MAX_FRAMES:
            raise ValueError('Frame capacity exceeded')
    if not rows:
        raise ValueError('Empty controller route')
    return rows


def read_movie_28(movie):
    """Return read_movie rows directly from a cold BizHawk2.8 Nymashock P1 movie."""
    with zipfile.ZipFile(movie) as archive:
        if len(archive.namelist()) != len(MEMBERS_28) or set(archive.namelist()) != MEMBERS_28:
            raise ValueError('Unsupported movie payload; cold 2.8 input-only movie required')
        header = dict(line.split(' ', 1) for line in archive.read('Header.txt').decode().splitlines()
                      if ' ' in line)
        if (header.get('Core') != 'Nymashock' or header.get('Platform') != 'PSX' or
                header.get('emuVersion') != 'Version 2.8'):
            raise ValueError('Only the declared Nymashock2.8 PSX log layout is supported')
        if any(field.startswith('StartsFrom') for field in header):
            raise ValueError('Anchored movie is not a cold controller route')
        settings = json.loads(archive.read('SyncSettings.json').decode())
        options = settings.get('o') if isinstance(settings, dict) else None
        if (not isinstance(options, dict) or options.get('MednafenValues') != {} or
                options.get('PortDevices') != {}):
            raise ValueError('Non-default Nymashock sync settings are unsupported')
        lines = archive.read('Input Log.txt').decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY_28] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    rows = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '....':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if (len(controller) != 5 or len(controller[4]) != 17 or
                not all(value.strip().isdigit() for value in controller[:4])):
            raise ValueError('Malformed controller record')
        rx, lx, ry, ly = (int(value) for value in controller[:4])
        if any(not 0 <= value <= 65535 for value in (rx, lx, ry, ly)):
            raise ValueError('Axis outside 16-bit range')
        if any(value & 0xFF for value in (rx, lx, ry, ly)):
            raise ValueError('2.8 axis value has no exact 2.9.1 byte; lossy conversion refused')
        word = 0xFFFF
        for character, bit in zip(controller[4][:16], BUTTON_BITS):
            if character != '.':
                word &= ~(1 << bit)
        rows.append((word, ly >> 8, lx >> 8, ry >> 8, rx >> 8, int(controller[4][16] != '.')))
        if len(rows) > MAX_FRAMES:
            raise ValueError('Frame capacity exceeded')
    if not rows:
        raise ValueError('Empty controller route')
    return rows


def read_movie_octoshock27(movie):
    """Return read_movie rows from a cold BizHawk 2.7 Octoshock movie with a DualShock on P1.

    One disc, never ejected or reset; no multitap or memory card; both sticks neutral and
    MODE never pressed (see LOGKEY_OCTO27_DUALSHOCK for why only those records are exact)."""
    with zipfile.ZipFile(movie) as archive:
        if len(archive.namelist()) != len(MEMBERS_28) or set(archive.namelist()) != MEMBERS_28:
            raise ValueError('Unsupported movie payload; cold 2.7 input-only movie required')
        header = dict(line.split(' ', 1) for line in archive.read('Header.txt').decode().splitlines()
                      if ' ' in line)
        if (header.get('Core') != 'Octoshock' or header.get('Platform') != 'PSX' or
                header.get('emuVersion') != 'Version 2.7.0'):
            raise ValueError('Only the declared Octoshock 2.7 PSX log layout is supported')
        if any(field.startswith('StartsFrom') for field in header):
            raise ValueError('Anchored movie is not a cold controller route')
        settings = json.loads(archive.read('SyncSettings.json').decode())
        options = settings.get('o') if isinstance(settings, dict) else None
        fio = options.get('FIOConfig') if isinstance(options, dict) else None
        if (not isinstance(fio, dict) or fio.get('Devices8') != [2, 0, 0, 0, 0, 0, 0, 0] or
                fio.get('Memcards') != [False, False] or fio.get('Multitaps') != [False, False]):
            raise ValueError('Only a lone DualShock on port 1 with no card or multitap is supported')
        lines = archive.read('Input Log.txt').decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY_OCTO27_DUALSHOCK] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    rows = []
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '    1,...':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if (len(controller) != 5 or len(controller[4]) != 17 or
                not all(value.strip().isdigit() for value in controller[:4])):
            raise ValueError('Malformed controller record')
        if any(int(value) != 128 for value in controller[:4]) or controller[4][16] != '.':
            raise ValueError('Octoshock stick or MODE input has no exact PSXRTI2 spelling')
        word = 0xFFFF
        for character, bit in zip(controller[4][:16], BUTTON_BITS_OCTO):
            if character != '.':
                word &= ~(1 << bit)
        rows.append((word, 128, 128, 128, 128, 0))
        if len(rows) > MAX_FRAMES:
            raise ValueError('Frame capacity exceeded')
    if not rows:
        raise ValueError('Empty controller route')
    return rows


def read_movie_octoshock_dualshock(movie, emu_versions=('Version 2.3.0', 'Version 2.7.0'),
                                   allow_card=False, allow_resaved=False):
    """Return (rows, facts) from a cold Octoshock movie with a DualShock on P1, sticks and MODE kept.

    read_movie_octoshock27 above accepts only records that are exact for a route with no pad model
    of its own: every stick at 128 and MODE never pressed. The Spyro movies are not like that, so
    this reader keeps what they carry and records what a runtime must therefore model:

    - Octoshock hands the four stick bytes to the pad unscaled (Mednafen converts them from
      16-bit values), so a log byte IS the byte the guest reads and passes into PSXRTI2
      unchanged. The log order is LX, LY, RX, RY; PSXRTI2 stores LY, LX, RY, RX.
    - The physical Analog (MODE) button is retained in its own field, never interpreted here.
      The oracle looks at that button only while the pad is deselected: a rising edge then
      toggles the pad's analog mode, and the button's previous state is updated only then.
      A route carrying a press is therefore refused downstream (run_native.route_identity)
      until a runtime models exactly that rule; the refusal is deliberate, not an oversight.

    emu_versions are the accepted declared header versions. allow_resaved additionally accepts a
    container re-saved by a later BizHawk (Spyro the Dragon 5523M was recorded on 2.7.0 and saved
    by 2.9.1), which only adds BizState/BizVersion members and rewrites emuVersion; the input log
    and sync settings are unchanged. allow_card accepts FIOConfig Memcards [True, False], which
    Spyro 3's 100-eggs movie declares."""
    with zipfile.ZipFile(movie) as archive:
        names = set(archive.namelist())
        resaved = allow_resaved and names == MEMBERS
        if not resaved and (len(archive.namelist()) != len(MEMBERS_28) or names != MEMBERS_28):
            raise ValueError('Unsupported movie payload; cold Octoshock input-only movie required')
        header = dict(line.split(' ', 1) for line in archive.read('Header.txt').decode().splitlines()
                      if ' ' in line)
        declared = header.get('OriginalEmuVersion') if resaved else header.get('emuVersion')
        if (header.get('Core') != 'Octoshock' or header.get('Platform') != 'PSX' or
                declared not in emu_versions):
            raise ValueError('Only the declared Octoshock PSX log layouts are supported')
        if any(field.startswith('StartsFrom') for field in header):
            raise ValueError('Anchored movie is not a cold controller route')
        settings = json.loads(archive.read('SyncSettings.json').decode())
        options = settings.get('o') if isinstance(settings, dict) else None
        fio = options.get('FIOConfig') if isinstance(options, dict) else None
        cards = [True, False] if allow_card else [False, False]
        if (not isinstance(fio, dict) or fio.get('Devices8') != [2, 0, 0, 0, 0, 0, 0, 0] or
                fio.get('Memcards') not in ([False, False], cards) or fio.get('Multitaps') != [False, False]):
            raise ValueError('Only a lone DualShock on port 1 with the declared card layout is supported')
        lines = archive.read('Input Log.txt').decode().splitlines()
    if len(lines) < 4 or lines[:2] != ['[Input]', LOGKEY_OCTO27_DUALSHOCK] or lines[-1] != '[/Input]':
        raise ValueError('Unsupported controller or frontend input layout')
    rows, sticks_used, mode_presses = [], 0, 0
    for line in lines[2:-1]:
        fields = line.split('|')
        if len(fields) != 4 or fields[0] or fields[3] or fields[1] != '    1,...':
            raise ValueError('Console events, disc changes and other ports are unsupported')
        controller = fields[2].split(',')
        if (len(controller) != 5 or len(controller[4]) != 17 or
                not all(value.strip().isdigit() for value in controller[:4])):
            raise ValueError('Malformed controller record')
        axes = tuple(int(value) for value in controller[:4])   # LX, LY, RX, RY
        if any(not 0 <= value <= 255 for value in axes):
            raise ValueError('Octoshock stick byte outside byte range')
        word = 0xFFFF
        for character, bit in zip(controller[4][:16], BUTTON_BITS_OCTO):
            if character != '.':
                word &= ~(1 << bit)
        analog = int(controller[4][16] != '.')
        sticks_used += any(value != 128 for value in axes)
        mode_presses += analog
        rows.append((word, axes[1], axes[0], axes[3], axes[2], analog))
        if len(rows) > MAX_FRAMES:
            raise ValueError('Frame capacity exceeded')
    if not rows:
        raise ValueError('Empty controller route')
    return rows, {'declared_emu_version': declared, 'resaved_container': bool(resaved),
                  'memcards': fio.get('Memcards'), 'stick_frames': sticks_used,
                  'mode_press_frames': mode_presses,
                  'stick_bytes': 'Octoshock passes log bytes to the pad unscaled',
                  'axis_order': 'log LX,LY,RX,RY -> PSXRTI2 LY,LX,RY,RX',
                  'runtime_requirement': ('a MODE press needs the pad model that toggles analog mode on the '
                                          'button rising edge while DTR is low' if mode_presses else None)}


def write_route(rows, output):
    """Validate completely before creating a new route; never replace one."""
    if not 1 <= len(rows) <= MAX_FRAMES:
        raise ValueError('Frame capacity exceeded')
    steps, previous = 0, None
    digest = hashlib.sha256()
    for row in rows:
        if (len(row) != 6 or any(type(value) is not int for value in row) or
                not 0 <= row[0] <= 65535 or any(not 0 <= value <= 255 for value in row[1:5]) or
                row[5] not in (0, 1)):
            raise ValueError('Invalid complete controller state')
        if row != previous:
            steps += 1
            previous = row
        if steps > MAX_STEPS:
            raise ValueError('Step capacity exceeded')
        digest.update(CONTROLLER.pack(*row))
    with Path(output).open('xb') as stream:
        stream.write(HEADER.pack(b'PSXRTI2\0', 2, RECORD.size, len(rows), 0))
        for index, row in enumerate(rows, 1):
            stream.write(RECORD.pack(index, *row, 0))
    return {'schema': 'psxrti2-controller-route-v1', 'frames': len(rows), 'steps': steps,
            'canonical_controller_sha256': digest.hexdigest(),
            'canonical_encoding': 'LE u16 active-low buttons;u8 LY,LX,RY,RX,physical Analog',
            'qualification': 'Lossless controller encoding only; native delivery and core compatibility separate'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('movie', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    receipt = write_route(read_movie(args.movie), args.output)
    receipt['movie_sha256'] = hashlib.sha256(args.movie.read_bytes()).hexdigest()
    receipt['route_sha256'] = hashlib.sha256(args.output.read_bytes()).hexdigest()
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()


# The legacy PSXRTI2 limits above stay frozen. Capacity-v1 is an explicit
# PSXRTI3 contract; the runtime admits up to four million frames and 131072
# compressed controller runs only when this mandatory tag is present.
V3_MAX_FRAMES = 4_000_000
V3_MAX_STEPS = 131072
V3_TAG_CAPACITY = 0x204
V3_TAG_EVENTS = 0x202
V3_TAG_DISC_SET = 0x203
EVENT_NAMES = {1: 'reset', 2: 'tray-open', 3: 'tray-close', 4: 'disc-select'}


def read_v3_dualshock(data, start=0, raw_axes=False, mode_model='none', neutral_p2=False):
    """Read the actual single-P1 DualShock PSXRTI3 body plus console events.

    A separately bound neutral P2 remains the original topology adapter;
    no P2 input is silently discarded or manufactured by this reader.
    """
    if len(data)<28:
        raise ValueError('short PSXRTI3 header')
    magic,version,size,frames,flags,ext=struct.unpack_from('<8sIIIII',data)
    if (magic,version,size,flags)!=(b'PSXRTI3\0',3,12,0):
        raise ValueError('not an implicit P1 DualShock PSXRTI3 route')
    if not 0<frames<=V3_MAX_FRAMES or ext%4 or ext>16*1024*1024 or len(data)!=28+ext+12*frames:
        raise ValueError('PSXRTI3 frame/extension/file size')
    entries={};offset=28
    while offset<28+ext:
        if 28+ext-offset<8:raise ValueError('short extension entry')
        tag,length=struct.unpack_from('<II',data,offset);offset+=8
        padded=(length+3)&~3
        if padded>28+ext-offset or any(data[offset+length:offset+padded]):
            raise ValueError('extension length/padding')
        payload=data[offset:offset+length];offset+=padded
        if tag in entries:raise ValueError('duplicate extension tag')
        if tag not in (V3_TAG_CAPACITY,V3_TAG_EVENTS,V3_TAG_DISC_SET):
            if not tag&0x80000000:raise ValueError('unknown mandatory extension tag')
            if tag&0xffffff00==0x80000100:raise ValueError('identity/marker tag requires its identity reader')
        entries[tag]=payload
    capacity=entries.get(V3_TAG_CAPACITY)
    if capacity is not None and capacity!=struct.pack('<II',1,V3_MAX_FRAMES):
        raise ValueError('capacity admission')
    if frames>MAX_FRAMES and capacity is None:raise ValueError('long route lacks capacity admission')
    events=[];discs=[]
    payload=entries.get(V3_TAG_DISC_SET)
    if payload is not None:
        if len(payload)<4:raise ValueError('disc set length')
        count,=struct.unpack_from('<I',payload)
        if not 1<=count<=8 or len(payload)!=4+36*count:raise ValueError('disc set length')
        for i in range(count):
            kind=payload[4+36*i];reserved=payload[5+36*i:8+36*i]
            if kind not in (1,2) or any(reserved):raise ValueError('disc set kind/reserved')
            discs.append({'kind':kind,'digest':payload[8+36*i:40+36*i].hex()})
    payload=entries.get(V3_TAG_EVENTS)
    if payload is not None:
        if len(payload)<4:raise ValueError('console events length')
        count,=struct.unpack_from('<I',payload)
        if count>256 or len(payload)!=4+8*count:raise ValueError('console events length')
        previous=0
        for i in range(count):
            frame,event,disc,reserved=struct.unpack_from('<IBBH',payload,4+8*i)
            if frame>frames or frame<previous or reserved or event not in EVENT_NAMES:
                raise ValueError('console event frame/kind/order/reserved')
            if (event in (1,2) and disc) or (event in (3,4) and not 1<=disc<=len(discs)):
                raise ValueError('console event disc')
            previous=frame;events.append({'frame':frame,'event':EVENT_NAMES[event],'disc_index':disc})
    if events and not discs:raise ValueError('console events require disc set')
    source=hashlib.sha256();protocol=hashlib.sha256()
    devices=bytes([2,0,0,0,2 if neutral_p2 else 0,0,0,0])
    full=hashlib.sha256(b'TAS_SOURCE_CONTROLLER_V1\0'+devices)
    steps=0;previous=None
    for i in range(frames):
        seq,buttons,ly,lx,ry,rx,mode,zero=RECORD.unpack_from(data,28+ext+12*i)
        if seq!=i+1 or zero or mode>1 or (mode and mode_model=='none'):
            raise ValueError('DualShock sequence/MODE/reserved')
        row=CONTROLLER.pack(buttons,ly,lx,ry,rx,mode)
        steps+=row!=previous;previous=row
        if steps>(V3_MAX_STEPS if capacity is not None else MAX_STEPS):raise ValueError('step capacity')
        if i<start:continue
        source.update(row)
        axes=(ly,lx,ry,rx) if raw_axes else [((x<<8)*255+32767)//65535 for x in (ly,lx,ry,rx)]
        applied=CONTROLLER.pack(buttons,*axes,mode);protocol.update(applied)
        full.update(struct.pack('<IB',seq,1)+applied)
        if neutral_p2:full.update(struct.pack('<BH5B',2,0xffff,128,128,128,128,0))
    return dict(format='PSXRTI3',controller_layout='dualshock',frames=frames,steps=steps,
                sha256=hashlib.sha256(data).hexdigest(),original_controller_sha256=source.hexdigest(),
                expected_protocol_sha256=protocol.hexdigest(),full_expected_applied_sha256=full.hexdigest(),
                capacity_version=1 if capacity else 0,console_events=events,disc_set=discs)


def write_v3_dualshock(rows,events,discs,path):
    """Write unchanged controller rows and explicit ordered boundary events."""
    if not 0<len(rows)<=V3_MAX_FRAMES:raise ValueError('full route frame capacity')
    extensions=[]
    def entry(tag,payload):
        extensions.append(struct.pack('<II',tag,len(payload))+payload+b'\0'*((-len(payload))%4))
    entry(V3_TAG_CAPACITY,struct.pack('<II',1,V3_MAX_FRAMES))
    if discs:
        entry(V3_TAG_DISC_SET,struct.pack('<I',len(discs))+b''.join(
            struct.pack('<B3x',d['kind'])+bytes.fromhex(d['digest']) for d in discs))
    if events:
        entry(V3_TAG_EVENTS,struct.pack('<I',len(events))+b''.join(
            struct.pack('<IBBH',e['frame'],e['kind'],e['disc_index'],0) for e in events))
    ext=b''.join(extensions)
    data=struct.pack('<8sIIIII',b'PSXRTI3\0',3,12,len(rows),0,len(ext))+ext+b''.join(
        RECORD.pack(i+1,*row,0) for i,row in enumerate(rows))
    # Complete validation uses the real reader, with MODE admission only when
    # the caller later binds its named production model.
    read_v3_dualshock(data,raw_axes=True,mode_model='octoshock-2.4.2')
    with Path(path).open('xb') as stream:stream.write(data)
    return {'path':str(path),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}


def read_full_octoshock_movie(path):
    """Decode the named original log columns, preserving all original rows.

    P2 is admitted only as the explicit connected-neutral DualShock topology;
    any non-neutral P2 record is rejected, never dropped.
    """
    bits={'Up':4,'Down':6,'Left':7,'Right':5,'Select':0,'Start':3,'Square':15,
          'Triangle':12,'Circle':13,'Cross':14,'L1':10,'R1':11,'L2':8,'R2':9,'L3':1,'R3':2}
    before=hashlib.sha256(Path(path).read_bytes()).hexdigest()
    rows=[];actions=[];columns=None;previous_disc=None
    with zipfile.ZipFile(path) as archive:
        header=archive.read('Header.txt').decode('utf-8-sig')
        if 'Core Octoshock' not in header or any(line.startswith('StartsFrom') for line in header.splitlines()):
            raise ValueError('full Octoshock input needs its original cold-boot header')
        sync=json.loads(archive.read('SyncSettings.json'))['o'];fio=sync['FIOConfig']
        devices=fio['Devices8'];neutral_p2=devices==[2,0,0,0,2,0,0,0]
        if devices!=[2,0,0,0,0,0,0,0] and not neutral_p2:
            raise ValueError('unsupported original controller topology')
        if fio['Multitaps']!=[False,False] or sync['EnableLEC']:
            raise ValueError('unbound original multitap/LEC topology')
        source_hash=hashlib.sha256(b'TAS_SOURCE_CONTROLLER_V1\0'+bytes(devices))
        for raw in archive.open('Input Log.txt'):
            line=raw.decode('utf-8-sig').rstrip('\r\n')
            if line.startswith('LogKey:'):
                columns=line[7:].replace('#','').split('|')
                if columns[-1]=='':columns.pop()
                if len(columns)!=len(set(columns)):raise ValueError('duplicate original log column')
                continue
            if not line.startswith('|'):continue
            if columns is None:raise ValueError('absent original LogKey')
            values=[]
            for segment in line.split('|')[1:-1]:
                fields=segment.split(',');values.extend(int(v.strip()) for v in fields[:-1]);values.extend(fields[-1])
            if len(values)!=len(columns):raise ValueError('original log column/row mismatch')
            record=dict(zip(columns,values));frame=len(rows)+1
            source_hash.update(struct.pack('<I',frame))
            for port in (1,2) if neutral_p2 else (1,):
                get=lambda name:record[f'P{port} {name}']
                buttons=0xffff
                for name,bit in bits.items():
                    if get(name)!='.':buttons &= ~(1<<bit)
                axes=[get('LStick Y'),get('LStick X'),get('RStick Y'),get('RStick X')]
                if any(type(x)is not int or not 0<=x<=255 for x in axes):raise ValueError('original raw axis outside byte range')
                row=(buttons,*axes,int(get('MODE')!='.'))
                source_hash.update(bytes([port])+CONTROLLER.pack(*row))
                if port==1:rows.append(row)
                elif row!=(0xffff,128,128,128,128,0):raise ValueError(f'unsupported non-neutral P2 at original frame{frame}')
            disc=record['Disc Select'];flags={name:record[name]!='.' for name in ('Open','Close','Reset')}
            changed=previous_disc is not None and disc!=previous_disc
            if any(flags.values()) or changed:
                actions.append(dict(original_input_frame=frame,selected_disc=disc,selected_disc_changed=changed,**flags))
            previous_disc=disc
    after=hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if before!=after:raise ValueError('original movie changed during full decode')
    return rows,dict(movie_sha256=before,header=header,sync_settings=sync,neutral_p2=neutral_p2,
                     original_console_records=actions,source_controller_sha256=source_hash.hexdigest())


def original_console_events(actions, frame_count, disc_count):
    """Existing Octoshock frontend open/select/close order at before-input boundaries.

    The Disc Select field is a choice while closed, not a request to remount
    the drive. Retain those source-only choice changes explicitly. A later
    Open uses the row's selected disc even if the choice did not change then.
    Original-core CPU/disc timing equivalence is not established by this adapter.
    """
    opened=False;mounted=1;events=[];choice_only=[]
    for row in actions:
        frame=row['original_input_frame']-1;selected=row['selected_disc']
        if not 0<=frame<frame_count or not 1<=selected<=disc_count:
            raise ValueError('original console frame/disc is outside its bound roster')
        if row['Reset']:raise ValueError('original console reset requires an unimplemented reset bridge')
        if row['Open'] and not opened:
            opened=True;events.append(dict(frame=frame,kind=2,disc_index=0))
        if opened and selected!=mounted:
            mounted=selected;events.append(dict(frame=frame,kind=4,disc_index=mounted))
        elif not opened and row['selected_disc_changed']:
            choice_only.append(dict(source_input_frame=frame+1,selected_disc_field=selected,
                                    mounted_disc=mounted,physical_core_event=False))
        if row['Close'] and opened:
            opened=False;events.append(dict(frame=frame,kind=3,disc_index=mounted))
    if opened:raise ValueError('original movie ends with its tray open')
    return events,choice_only
