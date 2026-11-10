#include "source_gpu_command_projection.h"
#include "gpu_sw_renderer.h"
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
static uint16_t vram[1024*512];
int g_ws_bd_stretch_on,g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void){abort();}
static void run(unsigned op,unsigned mode,unsigned blend,unsigned mask,unsigned dither,unsigned feedback) {
 memset(vram,0,sizeof(vram));unsigned ty=feedback?0:256,page=(feedback?0:16)|(mode<<7)|(blend<<5);
 for(unsigned y=0;y<128;y++)for(unsigned x=0;x<256;x++)vram[(y+ty)*1024+x]=(uint16_t)((x+y)%13==0?0:((x*37+y*113+1)&32767)|((x&1)?32768:0));
 for(unsigned i=0;i<256;i++)vram[400*1024+i]=(uint16_t)(i%17==0?0:((i*97+1)&32767)|((i&1)?32768:0));
 if(!feedback)for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++)vram[y*1024+x]=(uint16_t)(((x*19+y*57+123)&32767)|((x&1)?32768:0));
 int x[]={4,28,8,32},y[]={4,6,20,22};uint32_t colors[]={0xe3942b,0x285eb0,0x76c341,0x8ce921},uv[]={0x0301,0x65f7,0x7d0e,0x165f},words[12];
 unsigned vertices=(op&8)?4:3;
 for(unsigned i=0;i<vertices;i++){words[i*3]=colors[i]|(i?0:op<<24);words[i*3+1]=(unsigned)x[i]|(unsigned)y[i]<<16;words[i*3+2]=uv[i]|((i==0?400u<<6:i==1?page:0)<<16);}
 SourceGPUCommandProjection s;source_gpu_command_cold(&s);s.budget=256;s.clip_x1=1023;s.clip_y1=511;s.mask_bits=mask;s.draw_mode=page|dither<<9;
 sw_renderer_init(vram);sw_source_texture_control(0,0);sw_set_draw_area(0,0,1023,511);sw_set_mask_bits(mask&1,!!(mask&2));sw_set_semi_transparency(!!(op&2),blend);
 printf("op%02x-m%u-b%u-k%u-d%u-f%u",op,mode,blend,mask,dither,feedback);
 for(unsigned pass=0;pass<2;pass++)for(unsigned second=0;second<((op&8)?2u:1u);second++) {
  unsigned start=second?9:0,end=second?12:9;
  for(unsigned i=start;i<end;i++)if(!source_gpu_command_write(&s,words[i])){fprintf(stderr,"rejected opcode %02X\n",op);exit(1);}
  unsigned kind=(op&8)?(second?SOURCE_GPU_DISPATCH_QUAD_SECOND:SOURCE_GPU_DISPATCH_QUAD_FIRST):SOURCE_GPU_DISPATCH_COMMAND;
  if(s.dispatch.kind!=kind || s.dispatch.count!=end || s.phase!=((op&8)&&!second?2u:0u) || s.count)abort();
  if(!second)sw_source_texture_control(3,page);
  SourceGPUTexture texture={0};texture.page=page;texture.clut=400u<<6;texture.raw=!!(op&1);texture.load_clut=!second;
  for(unsigned i=0;i<3;i++)texture.uv[i]=uv[i+second];
  int extra=-1;if(!sw_draw_source_triangle(x+second,y+second,colors+second,1,dither,0,0,&texture,&extra) || extra<0)abort();
  s.budget-=extra;printf(" %d",256-s.budget);
  if(!source_gpu_command_update(&s,s.last_update+100000))abort();
 }
 uint8_t hash[32];psx_sha256_compute((uint8_t*)vram,sizeof(vram),hash);printf(" ");for(unsigned i=0;i<32;i++)printf("%02x",hash[i]);puts("");
}
int main(void){unsigned ops[]={0x34,0x35,0x36,0x37,0x3c,0x3d,0x3e,0x3f};for(unsigned o=0;o<8;o++)for(unsigned m=0;m<3;m++)for(unsigned b=0;b<4;b++)for(unsigned k=0;k<4;k++)for(unsigned d=0;d<2;d++)for(unsigned f=0;f<2;f++)run(ops[o],m,b,k,d,f);return 0;}
