"""Replay authored, hash-bound MDEC fixtures through both clean C entry paths."""
import argparse, ctypes, hashlib, json, struct
from pathlib import Path
standard = [int(v,16) for v in '''
5A82 5A82 5A82 5A82 5A82 5A82 5A82 5A82
7D8A 6A6D 471C 18F8 E707 B8E3 9592 8275
7641 30FB CF04 89BE 89BE CF04 30FB 7641
6A6D E707 8275 B8E3 471C 7D8A 18F8 9592
5A82 A57D A57D 5A82 5A82 A57D A57D 5A82
471C 8275 18F8 6A6D 9592 E707 7D8A B8E3
30FB 89BE 7641 CF04 CF04 7641 89BE 30FB
18F8 B8E3 6A6D 8275 7D8A 9592 471C E707
'''.split()]
standard = [v-65536 if v>=32768 else v for v in standard]

def scale_table(name):
    if name == 'std': return list(standard)
    if name == 'zeroAC': return standard[:8]+[0]*56
    if name == 'row1': return standard[:16]+[0]*48
    if name == 'x2': return [max(-32767,min(32767,v*2)) for v in standard]
    if name in ('dchalf','dc1p5'):
        a=list(standard)
        for i in set(range(8))|set(range(0,64,8)):
            a[i]=max(-32767,min(32767,round(a[i]*(.5 if name=='dchalf' else 1.5))))
        return a
    if name == 'row0flat16384': return [16384]*8+[0]*56
    a=[0]*64
    for entry in name.removeprefix('only').split(','):
        rc,value=entry.split('=');a[int(rc[0])*8+int(rc[1])]=int(value)
    return a

def read_rows(path):
    fields=None
    for line in path.read_text().splitlines():
        if line.startswith('#fields\t'): fields=line.split('\t')[1:]
        elif line.startswith(('M\t','B\t')):
            if fields is None: raise ValueError('Missing fields header')
            yield dict(zip(fields,line.split('\t')))

def inputs(row):
    hw=row['input_halfwords_hex'];stream=[int(hw[i:i+4],16) for i in range(0,len(hw),4)]
    if len(stream)%2:stream.append(0xfe00)
    quant=[1]*128
    if 'quant_hex' in row:quant=list(bytes.fromhex(row['quant_hex']))
    elif 'quant' in row:quant[1:64]=[int(row['quant'][1:])]*63
    elif 'qt0' in row:quant[0]=int(row['qt0'])
    if 'scale_hex' in row:scale=list(struct.unpack('<64h',bytes.fromhex(row['scale_hex'])))
    elif 'scale_halfwords_hex' in row:
        h=row['scale_halfwords_hex'];scale=[int(h[i:i+4],16) for i in range(0,len(h),4)]
        scale=[v-65536 if v>=32768 else v for v in scale]
    else:scale=scale_table(row.get('table',row.get('scale','std')))
    command=int(row['command'],16) if 'command' in row else 0x28000000|(int(row.get('signed','0'))<<26)
    expected=bytes.fromhex(row.get('output_hex',row.get('pixels_hex')))
    assert row['completed']=='True' and len(quant)==128 and len(scale)==64
    return command,quant,scale,stream,expected

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--library',type=Path,required=True)
    ap.add_argument('--receipt',type=Path);ap.add_argument('fixtures',nargs='*',type=Path);args=ap.parse_args()
    paths=args.fixtures or sorted((Path(__file__).parent/'data/mdec_clean').glob('*.tsv'))
    assert paths,'No fixtures selected'
    if not args.fixtures:
        manifest=json.loads((paths[0].parent/'manifest.json').read_text())
        assert {p.name for p in paths} == {entry['file'] for entry in manifest}
        for entry in manifest:
            assert hashlib.sha256((paths[0].parent/entry['file']).read_bytes()).hexdigest()==entry['sha256'],entry['file']
    lib=ctypes.CDLL(str(args.library.resolve()));call=lib.mdec_fixture_decode
    call.argtypes=[ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint8),ctypes.POINTER(ctypes.c_int16),ctypes.POINTER(ctypes.c_uint16),ctypes.c_uint,ctypes.POINTER(ctypes.c_uint8),ctypes.c_uint,ctypes.c_uint]
    call.restype=ctypes.c_int
    result={'library_sha256':hashlib.sha256(args.library.read_bytes()).hexdigest(),'sets':[]};failures=[]
    for path in paths:
        n=0;bad=0
        for row in read_rows(path):
            command,quant,scale,stream,expected=inputs(row)
            qa=(ctypes.c_uint8*128)(*quant);sa=(ctypes.c_int16*64)(*scale);ia=(ctypes.c_uint16*len(stream))(*stream)
            for source in (0,1):
                out=(ctypes.c_uint8*(len(expected)+768))()
                size=call(command,qa,sa,ia,len(stream),out,len(out),source)
                if size!=len(expected) or bytes(out[:max(0,size)])!=expected:
                    bad+=1
                    if len(failures)<20:failures.append({'case':row['case'],'block':row.get('block_in_case',row.get('block')),'source_callback':bool(source),'actual_size':size,'expected_size':len(expected),'first_difference':next((i for i,(a,b) in enumerate(zip(out,expected)) if a!=b),None)})
            n+=1
        assert n, f'No supported fixture rows in {path}'
        result['sets'].append({'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'cases_or_blocks':n,'entry_path_checks':n*2,'failures':bad})
        print(path.name,n,'rows,',n*2,'entry-path checks,',bad,'failures')
    result['failures']=failures
    if args.receipt:args.receipt.write_text(json.dumps(result,indent=2)+'\n')
    assert not failures,json.dumps(failures,indent=2)
if __name__=='__main__':main()
