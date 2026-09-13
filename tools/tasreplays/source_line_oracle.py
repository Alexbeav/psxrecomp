"""Generate authored line fixtures using unedited Nymashock source functions.

Builds extracted reference functions only in the supplied private output folder.
This is a component oracle, not an unmodified-core or full-replay qualification.
"""
import argparse, hashlib, json, random, subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[2]
line=(a.source/'gpu_line.cpp').read_text();common=(a.source/'gpu_common.inc').read_text()
plot=common[common.index('template<int BlendMode, bool MaskEval_TA, bool textured>'):common.index('static INLINE uint16 ModTexel')]
skip=common[common.index('static INLINE bool LineSkipTest'):common.index('static INLINE bool LineSkipTest')+common[common.index('static INLINE bool LineSkipTest'):].index('\n}')+2]
body=line[line.index('struct line_fxp_coord'):line.index('MDFN_HIDE extern const CTEntry')]
prefix='''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "psx_sha256.h"
using uint8=uint8_t;using uint16=uint16_t;using uint32=uint32_t;using uint64=uint64_t;
using int32=int32_t;using int64=int64_t;
#define INLINE inline
static uint16 GPURAM[512][1024];
static int ClipX0,ClipY0,ClipX1,ClipY1,OffsX,OffsY,DrawTimeAvail,dtd,dfe,MaskSetOR,MaskEvalAND;
static unsigned DisplayMode,DisplayFB_YStart,field_ram_readout;
static uint8 DitherLUT[4][4][256];
struct line_point {int32 x,y;uint8 r,g,b;};
struct PS_GPU {enum {INCMD_PLINE=1};};
static int InCmd,InCmd_CC;static line_point InPLine_PrevPoint;
static int sign_x_to_s32(unsigned bits,unsigned value){unsigned mask=1u<<(bits-1);return int((value&((mask<<1)-1))^mask)-int(mask);}
'''
dispatch='\n'.join(f'case {b}: if(mask&2)Command_DrawLine<false,false,{b},true>(words);else Command_DrawLine<false,false,{b},false>(words);break;' for b in range(-1,4))
main='''int main(){unsigned op,color,mode,mask,field;int x0,y0,x1,y1,ox,oy,clip;
const int matrix[4][4]={{-4,0,-3,1},{2,-2,3,-1},{-3,1,-4,0},{3,-1,2,-2}};
for(int y=0;y<4;y++)for(int x=0;x<4;x++)for(int c=0;c<256;c++){int v=c+matrix[y][x];if(v<0)v=0;if(v>255)v=255;DitherLUT[y][x][c]=v>>3;}
while(scanf("%u %u %d %d %d %d %d %d %u %u %u %d",&op,&color,&x0,&y0,&x1,&y1,&ox,&oy,&mode,&mask,&field,&clip)==12){
for(unsigned i=0;i<512*1024;i++)GPURAM[i/1024][i%1024]=(uint16)(i*37u+0x1234u);
ClipX0=ClipY0=clip;ClipX1=1023-clip;ClipY1=511-clip;OffsX=ox;OffsY=oy;
MaskSetOR=(mask&1)?0x8000:0;MaskEvalAND=(mask&2)?0x8000:0;dtd=!!(mode&512);dfe=!!(mode&1024);
DisplayMode=field?0x24:0;DisplayFB_YStart=0;field_ram_readout=field==2;
uint32 words[]={op<<24|color,((uint32)y0&65535u)<<16|((uint32)x0&65535u),((uint32)y1&65535u)<<16|((uint32)x1&65535u)};
DrawTimeAvail=254;switch((op&2)?int((mode>>5)&3):-1){DISPATCH}
uint8 hash[32];psx_sha256_compute((uint8*)GPURAM,sizeof(GPURAM),hash);
printf("%d ",256-DrawTimeAvail);for(unsigned i=0;i<32;i++)printf("%02x",hash[i]);puts("");}}
'''.replace('DISPATCH',dispatch)
cpp=a.output/'source-line.cpp';cpp.write_text(prefix+plot+skip+body+main)
exe=a.output/'source-line.exe'
subprocess.run(['g++','-O2','-I'+str(root/'runtime/include'),str(cpp),str(root/'runtime/src/psx_sha256.c'),'-o',str(exe)],check=True)
rng=random.Random(20371)
geometries=[(84,162,84,162),(0,0,0,0),(4,4,12,4),(4,4,4,12),(4,4,12,8),(12,8,4,4),(4,12,12,4),(-1,-1,8,8),(0,0,1023,511),(0,0,1024,0),(0,0,0,512),(1023,511,1023,511),(-1024,-1024,-1024,-1024)]
cases=[]
for i in range(256):
    xy=geometries[i%len(geometries)] if i<104 else tuple(rng.randrange(-1024,1024) for _ in range(4))
    cases.append([0x40+i%8,[0x00ff00,0xe3942b,0xffffff,0][i%4],*xy,0 if i<104 else rng.choice([-1024,-1,0,240,1023]),240 if i==0 else 0 if i<104 else rng.choice([-1024,-1,0,240,1023]),((i//8)%4)*32+(512 if i&1 else 0)+(1024 if i%7==0 else 0),(i//32)%4,(i//64)%3,0 if i%3 else 4])
cases[0]=[0x40,0x00ff00,84,162,84,162,0,240,512,0,0,0]
r=subprocess.run([str(exe)],input=''.join(' '.join(map(str,c))+'\n' for c in cases),capture_output=True,text=True,check=True,timeout=120)
rows=r.stdout.splitlines();assert len(rows)==len(cases)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
receipt={'scope':__doc__,'source_files':[{'path':str(a.source/n),'sha256':sha(a.source/n)} for n in ['gpu_line.cpp','gpu_common.inc']], 'driver_sha256':sha(Path(__file__)),'extracted_harness_sha256':sha(cpp),'cases':[{'input':c,'expected':row} for c,row in zip(cases,rows)]}
(a.output/'fixtures.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(a.output/'fixtures.json')
