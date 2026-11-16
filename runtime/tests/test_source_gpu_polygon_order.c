/* Authored triangle traversal/cache fixture; no retail input or state. */
#include "source_gpu_polygon_projection.h"
#include "gpu_sw_renderer.h"
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t vram[1024*512];
int g_ws_bd_stretch_on,g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void){abort();}
static void run(unsigned core,unsigned mode,unsigned feedback) {
    memset(vram,0,sizeof(vram));
    unsigned ty=feedback?0:256;
    for(unsigned y=0;y<128;y++)for(unsigned x=0;x<256;x++)
        vram[(y+ty)*1024+x]=(uint16_t)(((x*37u+y*113u+1u)&32767u)|32768u);
    for(unsigned i=0;i<256;i++)vram[400*1024+i]=(uint16_t)(((i*97u+1u)&32767u)|32768u);
    int x[3]={20,60,20},y[3]={4,40,100};x[core]=4;
    uint32_t colors[3]={0x808080,0x808080,0x808080};
    SourceGPUTexture texture={0};texture.page=(feedback?0:16)|(mode<<7);texture.clut=400<<6;
    texture.load_clut=1;texture.uv[0]=0x0301;texture.uv[1]=0x65f7;texture.uv[2]=0x7d0e;
    sw_renderer_init(vram);sw_source_texture_control(0,0);sw_source_texture_control(3,texture.page);
    sw_set_draw_area(0,0,255,127);sw_set_mask_bits(0,0);sw_set_semi_transparency(0,0);
    printf("core%u-m%u-f%u",core,mode,feedback);
    for(unsigned pass=0;pass<2;pass++) {
        int extra=-1; if(!sw_draw_source_triangle(x,y,colors,0,0,0,0,&texture,&extra))abort();
        int work=264+source_poly_cost(x,y,0,0,255,127,1,0,0,0)+extra;
        uint8_t hash[32];psx_sha256_compute((uint8_t*)vram,sizeof(vram),hash);
        printf(" %d ",work);for(unsigned i=0;i<32;i++)printf("%02x",hash[i]);
    }
    puts("");
}
int main(void){for(unsigned core=0;core<3;core++)for(unsigned mode=0;mode<3;mode++)for(unsigned feedback=0;feedback<2;feedback++)run(core,mode,feedback);return 0;}
