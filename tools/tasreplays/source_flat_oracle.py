"""Compare flat polygon pixels with unmodified Octoshock2.3 using authored MMIO.

Full VRAM is compared; this pixel fixture makes no timing or hardware claim.
The native test never imports emulator memory or a retail game state.
"""
import argparse
import ctypes as c
import hashlib
import itertools
import json
from pathlib import Path
import struct

p=argparse.ArgumentParser(description=__doc__)
for name in ['core','native-dumps','output']:
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();core=a.core.resolve(strict=True);native_root=a.native_dumps.resolve(strict=True)
D=a.output.resolve();D.mkdir(exist_ok=False)
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
assert sha(core)=='749d6dd58430d010e46ae97c628e113adad5f420c05c2ada0ad35e58191781c0'
# Preserve the actual executable driver, before its first source-core call.
(D/'frozen-driver.py').write_bytes(Path(__file__).read_bytes())
dll=c.CDLL(str(core));ptr=c.c_void_p
def bind(name,args):
    fn=getattr(dll,name);fn.restype=c.c_int;fn.argtypes=args;return fn
create=bind('shock_Create',[c.POINTER(ptr),c.c_int,ptr]);power=bind('shock_PowerOn',[ptr])
step=bind('shock_Step',[ptr,c.c_int]);destroy=bind('shock_Destroy',[ptr])
getmem=bind('shock_GetMemData',[ptr,c.POINTER(ptr),c.POINTER(c.c_int),c.c_int])
settrace=bind('shock_SetTraceCallback',[ptr,ptr,ptr])
trace_type=c.CFUNCTYPE(None,ptr,c.c_uint32,c.c_uint32,c.c_char_p)
def memory(console,kind,wanted):
    address=ptr();length=c.c_int()
    assert getmem(console,c.byref(address),c.byref(length),kind)==0 and length.value==wanted
    return address.value
def put(data,offset,words):
    for i,w in enumerate(words):struct.pack_into('<I',data,offset+4*i,w)
report={'core':str(core),'core_sha256':sha(core),'driver_sha256':sha(Path(__file__)),
        'native_dumps':str(native_root),'scope':__doc__,'status':'running','cases':[]}
for opcode,blend,mask in itertools.product([0x20,0x21,0x22,0x23,0x28,0x29,0x2a,0x2b],range(4),range(4)):
    name=f'op{opcode:02x}-b{blend}-k{mask}'
    gpu=[]
    for y in [4,5]:gpu += [0xa0000000,4|(y<<16),0x00010004,0x92341234,0x92341234]
    gpu += [0xe3000000,0xe407ffff,0xe1000000|(blend<<5),0xe6000000|mask,
            (opcode<<24)|0xffffff,0x00040004,0x00040008,0x00060004]
    if opcode&8:gpu.append(0x00060008)
    program=[0x3c081f80]
    for word in gpu:program += [0x3c090000|(word>>16),0x35290000|(word&65535),0xad091810]
    program += [0x3c08a000,0x3c09feed,0x35291234,0xad090100,0x3c09bfc0,0x35290200,0x01200008,0]
    rom=bytearray(512*1024);ram=bytearray(0x3000)
    put(rom,0,[0x3c0bfffe,0x356b0130,0x240c0800,0xad6c0000,0x40806000,
               0x3c19a000,0x37390500,0x03200008,0])
    put(rom,0x200,[0x0bf00080,0]);put(ram,0x500,program)
    (D/(name+'-rom.bin')).write_bytes(rom);(D/(name+'-ram.bin')).write_bytes(ram)
    buffer=(c.c_ubyte*len(rom)).from_buffer_copy(rom);console=ptr()
    assert create(c.byref(console),1,buffer)==0
    try:
        assert power(console)==0
        address=memory(console,0,2097152);c.memmove(address,bytes(ram),len(ram))
        reached=[];calls=[0]
        @trace_type
        def trace(_,pc,word,message):
            calls[0]+=1
            if pc==0xbfc00200:reached.append(pc);settrace(console,None,None)
            elif calls[0]>1024:settrace(console,None,None)
        assert settrace(console,None,trace)==0 and step(console,0)==0
        assert reached==[0xbfc00200] and c.c_uint32.from_address(address+0x100).value==0xfeed1234
        raw=c.string_at(memory(console,3,1048576),1048576)
        output=D/(name+'.bin');output.write_bytes(raw)
        native=native_root/output.name;actual=native.read_bytes()
        assert len(actual)==len(raw),'native VRAM must be exactly 1 MiB'
        changed=[i//2 for i in range(0,len(raw),2) if raw[i:i+2]!=actual[i:i+2]]
        report['cases'].append({'case':name,'gp0':gpu,'wait_loop_pc':reached[0],
                                'source_sha256':sha(output),'native_sha256':sha(native),
                                'changed_words':len(changed),'first_words':changed[:16],
                                'authored_rom_sha256':sha(D/(name+'-rom.bin')),
                                'authored_ram_sha256':sha(D/(name+'-ram.bin'))})
        (D/'receipt.json').write_text(json.dumps(report,indent=2))
        print(name,'PASS' if not changed else 'FAIL',len(changed),flush=True)
        if changed:raise AssertionError('stock/native VRAM mismatch')
    finally:destroy(console)
report['status']='pass';(D/'receipt.json').write_text(json.dumps(report,indent=2))
