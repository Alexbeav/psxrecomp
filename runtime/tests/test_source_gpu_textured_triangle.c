/* Textured triangle command flags and source software texture path. */
#include "source_gpu_command_projection.h"
#include "gpu_sw_renderer.h"
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
static uint16_t vram[1024*512];static unsigned checks;static int pixels_only;
int g_ws_bd_stretch_on,g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void){abort();}
static void check(int okay,const char *message){checks++;if(!okay){fprintf(stderr,"FAIL: %s\n",message);exit(1);}}
static void run(unsigned opcode,unsigned mode,unsigned blend,unsigned mask,unsigned color){
 unsigned page=16|(mode<<7)|(blend<<5);
 uint32_t words[]={opcode<<24|color,0x00040004,300u<<22,0x00040008,(page<<16)|4,0x00060004,0};
 SourceGPUCommandProjection s;source_gpu_command_cold(&s);s.clip_x1=1023;s.clip_y1=511;s.budget=256;s.mask_bits=mask;
 if(!pixels_only){
 check(source_gpu_polygon_supported(opcode),"textured triangle opcode admitted");
 check(source_gpu_command_length(words[0])==7,"seven-word first triangle packet");
 check(source_gpu_command_feedback_length(words[0])==1,"one-word DMA feedback");
 for(unsigned i=0;i<6;i++){check(source_gpu_command_write(&s,words[i]),"partial packet queues");check(!s.dispatch.kind,"no partial rendering");}
 check(source_gpu_command_write(&s,words[6]),"complete textured triangle dispatch");
 check(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==7 && !s.phase && !s.count,"single full triangle dispatch");
 check(s.budget==256-84-180-12,"source setup and doubled six-pixel cost, independent of blend/mask");
 check((s.draw_mode&511)==page,"polygon tpage latches at dispatch");
 }
 memset(vram,0,sizeof(vram));const uint16_t texels[]={0,0x801f,0x03e0,0xfc00};
 memcpy(vram+300*1024,texels,sizeof(texels));
 for(unsigned y=4;y<6;y++)for(unsigned x=4;x<8;x++)vram[y*1024+x]=(uint16_t)(0x1234|((x&1)?0x8000:0));
 if(mode==0)vram[256*1024]=0x3210;
 else if(mode==1){vram[256*1024]=0x0100;vram[256*1024+1]=0x0302;}
 else memcpy(vram+256*1024,texels,sizeof(texels));
 sw_renderer_init(vram);sw_source_texture_control(0,0);sw_source_texture_control(3,page);
 sw_set_mask_bits(mask&1,!!(mask&2));sw_set_semi_transparency(!!(opcode&2),blend);
 int x[]={4,8,4},y[]={4,4,6},extra=-1;uint32_t colors[]={color,color,color};
 SourceGPUTexture texture={0};texture.page=page;texture.clut=300u<<6;texture.raw=!!(opcode&1);texture.load_clut=1;texture.uv[1]=4;
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra),"existing source software triangle path");
 check(extra==(mode==0?20:mode==1?260:4),"transparent/masked pixels retain full CLUT/cache work");
 uint8_t hash[32];psx_sha256_compute((uint8_t*)vram,sizeof(vram),hash);
 printf("op%02x-m%u-b%u-k%u-c%06x ",opcode,mode,blend,mask,color);for(unsigned i=0;i<32;i++)printf("%02x",hash[i]);puts("");
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==0,"repeated triangle retains cache");
}

/* PS1B-632: FFIX's first persistent credit split is an inline 15-bit to
 * 8-bit page transition at identical page X/Y. Keep shared physical cache
 * words warm while retaining the explicit E1 and cache-clear controls. */
static void depth_cache_transition(void) {
 const unsigned direct=16u|(2u<<7),indexed=16u|(1u<<7);
 memset(vram,0,sizeof vram);
 for(unsigned i=0;i<4;i++)vram[256u*1024u+i]=0x0101;
 for(unsigned i=0;i<256;i++)vram[300u*1024u+i]=0x001f;
 sw_renderer_init(vram);sw_source_texture_control(0,0);
 sw_set_mask_bits(0,0);sw_set_semi_transparency(0,0);
 SourceGPUTexture texture={0};texture.page=direct;texture.clut=300u<<6;
 texture.raw=1;texture.load_clut=1;texture.uv[1]=4;
 int x[]={4,8,4},y[]={4,4,6},extra=-1;
 uint32_t colors[]={0x808080,0x808080,0x808080};
 sw_source_texture_control(3,direct);
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==4,
       "warm physical bitmap words in 15-bit mode");
 texture.page=indexed;texture.uv[1]=8; /* Same physical VRAM word addresses. */
 sw_source_texture_control(SOURCE_GPU_TEXTURE_POLYGON_PAGE,indexed);
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==256,
       "inline depth-only transition retains bitmap cache; only CLUT loads");
 uint8_t warm[32],cold[32];psx_sha256_compute((uint8_t*)vram,sizeof vram,warm);
 sw_source_texture_control(3,direct);sw_source_texture_control(3,indexed);
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==4,
       "explicit E1 page controls still clear bitmap lines and retain CLUT");
 psx_sha256_compute((uint8_t*)vram,sizeof vram,cold);
 check(!memcmp(warm,cold,sizeof warm),"warm/cold depth transition draws identical pixels");
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==0,
       "repeated indexed triangle remains warm");
 sw_source_texture_control(1,0);
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==260,
       "explicit cache-clear still invalidates bitmap and CLUT");

 /* 4-bit and 8/15-bit use different cache indexing. Public source cache
  * snapshots at FFIX 1551147 invalidate across this inline boundary. */
 sw_source_texture_control(0,0);
 texture.page=indexed;texture.uv[1]=8;
 sw_source_texture_control(3,indexed);
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==260,
       "warm indexed bitmap and 256-entry CLUT");
 texture.page=16;texture.uv[1]=16;
 sw_source_texture_control(SOURCE_GPU_TEXTURE_POLYGON_PAGE,texture.page);
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==20,
       "inline 8-bit to 4-bit invalidates bitmap layout and loads 16-entry CLUT");
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==0,
       "unchanged 4-bit inline selection remains warm");
 texture.page=indexed;texture.uv[1]=8;
 sw_source_texture_control(SOURCE_GPU_TEXTURE_POLYGON_PAGE,texture.page);
 check(sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra) && extra==260,
       "inline 4-bit to 8-bit invalidates bitmap layout and loads 256-entry CLUT");
}

int main(int argc,char **argv){pixels_only=argc==2 && !strcmp(argv[1],"--pixels-only");
 for(unsigned opcode=0x24;opcode<=0x27;opcode++)for(unsigned mode=0;mode<3;mode++)for(unsigned blend=0;blend<4;blend++)for(unsigned mask=0;mask<4;mask++){
 run(opcode,mode,blend,mask,0x808080);run(opcode,mode,blend,mask,0xe3942b);}
 if(!pixels_only)depth_cache_transition();
 fprintf(stderr,"source textured triangle: %u checks passed\n",checks);return 0;}
