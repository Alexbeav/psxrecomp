#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <assert.h>
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"r");assert(f);unsigned op,v[7],expected[7],cases=0;
 while(fscanf(f,"%u",&op)==1){
  for(unsigned i=0;i<7;i++)assert(fscanf(f,"%u",&v[i])==1);
  for(unsigned i=0;i<7;i++)assert(fscanf(f,"%u",&expected[i])==1);
  SourceGPUCommandProjection s;source_gpu_command_cold(&s);s.budget=(int32_t)v[0];s.phase=v[1];s.count=v[2];s.last_update=v[4];s.command=v[5];s.queue[0]=v[6];
  assert(source_gpu_command_gp1(&s,op<<24));
  unsigned actual[7]={(uint32_t)s.budget,s.phase,s.count,(unsigned)source_gpu_command_ready(&s),(unsigned)s.last_update,s.command,s.count?s.queue[0]:0};
  for(unsigned i=0;i<7;i++)assert(actual[i]==expected[i]);
  cases++;
 }
 fclose(f);assert(cases==4);
 for(unsigned kind=0;kind<2;kind++){
  SourceGPUCommandProjection s;source_gpu_command_cold(&s);assert(source_gpu_command_write(&s,0xe407ffff));assert(source_gpu_command_update(&s,128));
  if(kind==0)assert(source_gpu_command_write(&s,0xe6000001));else assert(source_gpu_command_gp1(&s,0x08000024));
  assert(source_gpu_command_write(&s,0x28000000));assert(source_gpu_command_write(&s,0));assert(source_gpu_command_write(&s,1));assert(!source_gpu_command_write(&s,0x10000));assert(s.error==SOURCE_GPU_COMMAND_UNSUPPORTED);
 }
 SourceGPUCommandProjection s;source_gpu_command_cold(&s);assert(source_gpu_command_write(&s,0xe500ffff));assert(s.offset_x==-1&&s.offset_y==31);assert(!source_gpu_command_gp1(&s,0x08000008));
 puts("PASS four source reset states, mask/interlace rejection, signed offset and PAL rejection");return 0;
}
