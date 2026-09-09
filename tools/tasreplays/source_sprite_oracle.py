"""Fresh stock Octoshock2.3, authored guest GPU commands, full VRAM comparison.

This fixture observes pixels only. It does not import runtime state or use the
patched observer DLL, and makes no CPU/GPU clock or hardware accuracy claim.
"""
from pathlib import Path
import argparse, ctypes as c, hashlib, json, struct
p=argparse.ArgumentParser(description=__doc__)
for name in ['core','native-dumps','output']: p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();core=a.core.resolve(strict=True);native_root=a.native_dumps.resolve(strict=True)
D=a.output.resolve();D.mkdir(exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(core)=='749d6dd58430d010e46ae97c628e113adad5f420c05c2ada0ad35e58191781c0'
dll=c.CDLL(str(core));ptr=c.c_void_p
def bind(name,args):
    fn=getattr(dll,name);fn.restype=c.c_int;fn.argtypes=args;return fn
create=bind('shock_Create',[c.POINTER(ptr),c.c_int,ptr]);power=bind('shock_PowerOn',[ptr])
step=bind('shock_Step',[ptr,c.c_int]);destroy=bind('shock_Destroy',[ptr])
getmem=bind('shock_GetMemData',[ptr,c.POINTER(ptr),c.POINTER(c.c_int),c.c_int])
trace_type=c.CFUNCTYPE(None,ptr,c.c_uint32,c.c_uint32,c.c_char_p)
settrace=bind('shock_SetTraceCallback',[ptr,ptr,ptr])
def memory(console,kind,wanted):
    address=ptr();length=c.c_int()
    assert getmem(console,c.byref(address),c.byref(length),kind)==0 and length.value==wanted
    return address.value
def words_into(data,offset,words):
    for i,w in enumerate(words):struct.pack_into('<I',data,offset+4*i,w)
def upload(x,y,values):
    assert len(values)%2==0
    return [0xa0000000,x|(y<<16),len(values)|(1<<16)]+[values[i]|(values[i+1]<<16) for i in range(0,len(values),2)]
report={'core':str(core),'core_sha256':sha(core),'driver_sha256':sha(Path(__file__)),
        'native_dumps':str(native_root),
        'scope':'stock Octoshock2.3 full VRAM equality after ordinary authored GP0 commands; no timing claim','cases':[]}
texels=[0,0x801f,0x03e0,0xfc00]
for mode in range(3):
 for blend in range(4):
  for mask in range(4):
   for color in [0x808080,0xe3942b]:
    name=f'm{mode}-b{blend}-k{mask}-c{color:06x}'
    # Direct fixture inputs are the same explicit uploads on both sides.
    gpu=upload(0,300,texels)+upload(4,4,[0x1234,0x9234,0x1234,0x9234])
    gpu+=upload(0,256,[0x3210,0] if mode==0 else [0x0100,0x0302] if mode==1 else texels)
    gpu += [0xe3000000,0xe407ffff,0xe1000000|16|(mode<<7)|(blend<<5),0xe6000000|mask,
            0x66000000|color,0x00040004,300<<22,0x00010004]
    program=[0x3c081f80] # t0 = GPU register page
    for word in gpu:
        program += [0x3c090000|(word>>16),0x35290000|(word&65535),0xad091810]
    # A marker after all CPU writes proves that the authored route executed.
    program += [0x3c08a000,0x3c09feed,0x35291234,0xad090100,0x3c09bfc0,0x35290200,0x01200008,0]
    rom=bytearray(512*1024);ram=bytearray(0x3000)
    words_into(rom,0,[0x3c0bfffe,0x356b0130,0x240c0800,0xad6c0000,0x40806000,
                      0x3c19a000,0x37390500,0x03200008,0])
    words_into(rom,0x200,[0x0bf00080,0]);words_into(ram,0x500,program)
    (D/(name+'-rom.bin')).write_bytes(rom);(D/(name+'-ram.bin')).write_bytes(ram)
    buffer=(c.c_ubyte*len(rom)).from_buffer_copy(rom);console=ptr()
    assert create(c.byref(console),1,buffer)==0
    try:
        assert power(console)==0
        address=memory(console,0,2097152);c.memmove(address,bytes(ram),len(ram))
        reached=[];trace_calls=[0]
        @trace_type
        def trace(_,pc,opcode,message):
            trace_calls[0]+=1
            if pc==0xbfc00200:
                reached.append(pc);settrace(console,None,None)
            elif trace_calls[0]>1024:settrace(console,None,None)
        assert settrace(console,None,trace)==0
        assert step(console,0)==0
        assert c.c_uint32.from_address(address+0x100).value==0xfeed1234
        assert reached==[0xbfc00200], 'authored program did not reach the BIOS wait loop'
        raw=c.string_at(memory(console,3,1048576),1048576)
        output=D/(name+'.bin');output.write_bytes(raw)
        native=native_root/output.name
        actual=native.read_bytes()
        assert len(actual)==len(raw), 'native VRAM must be exactly 1 MiB'
        changed=[i//2 for i in range(0,len(raw),2) if raw[i:i+2]!=actual[i:i+2]]
        report['cases'].append({'case':name,'gp0':gpu,'wait_loop_pc':reached[0],'trace_calls':trace_calls[0],'source_sha256':sha(output),
                                'native_sha256':sha(native),'changed_words':len(changed),'first_words':changed[:16],
                                'authored_rom_sha256':sha(D/(name+'-rom.bin')),'authored_ram_sha256':sha(D/(name+'-ram.bin'))})
        (D/'receipt.json').write_text(json.dumps(report,indent=2))
        print(name,'PASS' if not changed else 'FAIL',len(changed),flush=True)
        if changed:raise AssertionError('stock/native VRAM mismatch')
    finally:destroy(console)
report['status']='pass';(D/'receipt.json').write_text(json.dumps(report,indent=2))
