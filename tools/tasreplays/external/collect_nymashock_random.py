# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the existing raw cold sequence against exact Nymashock source.

Extracts the complete original generator type into a private build directory.
No retail input, source emulator state, CD command capture or native feedback.
This qualifies raw words only; command ordering/range consumption is separate.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,struct,subprocess
from source_random_tape import words

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mednafen',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--compiler',default='g++');args=parser.parse_args()
    source=args.mednafen.resolve(strict=True)
    for path,revision in [(source,'ddf225cf63b7b355cb2ac7772450cf473f4b53ac'),(source.parent,'745efb1dd8eb82f31ba9201a79cdfc5bcaf1f5d1')]:
        if subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True).strip()!=revision or subprocess.check_output(['git','-C',str(path),'status','--porcelain'],text=True).strip():
            raise ValueError('exact clean pinned source required')
    compiler=Path(shutil.which(args.compiler) or args.compiler).resolve(strict=True)
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    original=source/'src/psx/psx.cpp';text=original.read_text()
    start=text.index('struct MDFN_PseudoRNG');end=text.index('static MDFN_PseudoRNG PSX_PRNG;')
    region=text[start:end]
    fixture=out/'source-random.cpp'
    fixture.write_text('#include <src/psx/psx.h>\n#include <cstdio>\nnamespace MDFN_IEN_PSX {\n'+region+'\n}\n'+r'''
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    FILE *file=fopen(argv[1],"wb"); if(!file) return 3;
    MDFN_IEN_PSX::MDFN_PseudoRNG generator;
    for(unsigned i=0;i<65536;++i) {
        unsigned value=generator.RandU32();
        for(unsigned shift=0;shift<32;shift+=8)
            if(fputc((value>>shift)&255,file)==EOF) return 4;
    }
    return fclose(file) ? 5 : 0;
}
''')
    env=dict(os.environ);env['PATH']=str(compiler.parent)+os.pathsep+env['PATH']
    expected=b''.join(struct.pack('<I',word) for word in words(65536));results=[]
    for mode in ('O0','O2'):
        binary=out/('source-'+mode+('.exe' if os.name=='nt' else ''));raw=out/('words-'+mode+'.bin')
        argv=[str(compiler),'-std=c++17','-'+mode,'-UNDEBUG','-DHAVE_CONFIG_H','-DMDFN_DISABLE_NO_OPT_ERRWARN',
              '-I'+str(source.parent/'common'),'-I'+str(source),'-I'+str(source/'src/trio'),str(fixture),'-o',str(binary)]
        with (out/('build-'+mode+'.log')).open('xb') as log:
            result=subprocess.run(argv,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120)
        if result.returncode:raise ValueError('source generator compilation failed')
        subprocess.run([str(binary),str(raw)],env=env,check=True,timeout=30)
        if raw.read_bytes()!=expected:raise ValueError('raw cold generator sequence differs')
        results.append({'argv':argv,'binary_sha256':sha(binary),'raw_words_sha256':sha(raw),'words':65536})
    tape=out/'nymashock-cold-random.psxrng'
    tape.write_bytes(b'PSX-CD-RNG1\0\0\0\0\0'+struct.pack('<I',65536)+expected)
    if sha(tape)!='d85f0dec13b00e50b10a52ce0fda3cdaa9797f058ad16dd57caa8e926fad7a6a':raise ValueError('tape format differs')
    receipt={'status':'pass','source_commit':'ddf225cf63b7b355cb2ac7772450cf473f4b53ac','source':str(original),
             'source_sha256':sha(original),'source_region_sha256':hashlib.sha256(region.encode()).hexdigest(),
             'collector_sha256':sha(__file__),'compiler_sha256':sha(compiler),'exporter_sha256':sha(Path(__file__).with_name('source_random_tape.py')),
             'tape':str(tape),'tape_sha256':sha(tape),'results':results,
             'scope':'65536 raw cold words match unmodified source type at O0/O2. Range rejection/command timing/consumption and retail equivalence remain separate.'}
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({'status':'pass','words':65536,'tape_sha256':sha(tape),'receipt':str(out/'receipt.json')}))

if __name__=='__main__':main()
