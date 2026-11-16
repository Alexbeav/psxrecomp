#include "source_gpu_command_projection.h"
#include "gpu_sw_renderer.h"
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
static uint16_t vram[1024*512];
int g_ws_bd_stretch_on,g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void){abort();}
static void run(unsigned op,unsigned blend,unsigned mask,unsigned dither) {
 memset(vram,0,sizeof(vram));
 for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++)vram[y*1024+x]=(uint16_t)(((x*37+y*113+1)&32767)|((x&1)?32768:0));
 int x[]={4,28,8,32},y[]={4,6,20,22};uint32_t colors[]={0xe3942b,0x285eb0,0x76c341,0x8ce921},words[8];
 for(unsigned i=0;i<4;i++){words[i*2]=colors[i]|(i?0:op<<24);words[i*2+1]=(unsigned)x[i]|(unsigned)y[i]<<16;}
 SourceGPUCommandProjection s;source_gpu_command_cold(&s);s.budget=256;s.clip_x1=1023;s.clip_y1=511;s.mask_bits=mask;s.draw_mode=blend<<5|dither<<9;
 for(unsigned i=0;i<6;i++)if(!source_gpu_command_write(&s,words[i])){fprintf(stderr,"rejected opcode %02X\n",op);exit(1);}
 if(s.dispatch.kind!=SOURCE_GPU_DISPATCH_QUAD_FIRST)abort();int first=256-s.budget;
 if(!source_gpu_command_update(&s,100000))abort();
 for(unsigned i=6;i<8;i++)if(!source_gpu_command_write(&s,words[i]))abort();
 if(s.dispatch.kind!=SOURCE_GPU_DISPATCH_QUAD_SECOND || s.phase || s.count)abort();int second=256-s.budget;
 sw_renderer_init(vram);sw_source_texture_control(0,0);sw_set_draw_area(0,0,1023,511);sw_set_mask_bits(mask&1,!!(mask&2));sw_set_semi_transparency(!!(op&2),blend);
 for(unsigned tri=0;tri<2;tri++){int extra=-1;if(!sw_draw_source_triangle(x+tri,y+tri,colors+tri,1,dither,0,0,NULL,&extra) || extra)abort();}
 uint8_t hash[32];psx_sha256_compute((uint8_t*)vram,sizeof(vram),hash);
 printf("op%02x-b%u-k%u-d%u %d %d ",op,blend,mask,dither,first,second);for(unsigned i=0;i<32;i++)printf("%02x",hash[i]);puts("");
}
int main(void){for(unsigned op=0x38;op<=0x3b;op++)for(unsigned b=0;b<4;b++)for(unsigned k=0;k<4;k++)for(unsigned d=0;d<2;d++)run(op,b,k,d);return 0;}
