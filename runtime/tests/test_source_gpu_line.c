/* Authored input cases are compared against the retained source oracle hashes. */
#include "source_gpu_command_projection.h"
#include "gpu_sw_renderer.h"
#include "psx_sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint16_t vram[1024*512];
int g_ws_bd_stretch_on,g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void){abort();}
int main(void) {
    unsigned op,color,mode,mask,field;
    int x0,y0,x1,y1,ox,oy,clip;
    while(scanf("%u %u %d %d %d %d %d %d %u %u %u %d",&op,&color,&x0,&y0,&x1,&y1,&ox,&oy,&mode,&mask,&field,&clip)==12) {
        uint32_t words[]={op<<24|color,((uint32_t)y0&65535u)<<16|((uint32_t)x0&65535u),((uint32_t)y1&65535u)<<16|((uint32_t)x1&65535u)};
        SourceGPUCommandProjection s;source_gpu_command_cold(&s);
        s.clip_x0=s.clip_y0=clip;s.clip_x1=1023-clip;s.clip_y1=511-clip;
        s.budget=256;s.offset_x=ox;s.offset_y=oy;s.draw_mode=mode;
        s.display_mode=field?0x24:0;s.field_valid=1;s.skip_field=field==2;s.mask_bits=mask;
        assert(source_gpu_command_length(words[0])==3);
        assert(source_gpu_command_feedback_length(words[0])==1);
        for(unsigned i=0;i<2;i++){assert(source_gpu_command_write(&s,words[i]));assert(s.dispatch.kind==0);}
        assert(!source_gpu_command_ready(&s));
        assert(source_gpu_command_write(&s,words[2]));
        assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==3 && !s.count);
        for(unsigned i=0;i<1024*512;i++)vram[i]=(uint16_t)(i*37u+0x1234u);
        sw_renderer_init(vram);sw_set_mask_bits(mask&1,!!(mask&2));sw_set_semi_transparency(!!(op&2),(mode>>5)&3);
        SourceGPUBlock b={0};b.words=words;b.draw_mode=mode;
        b.x=source_gpu_command_coord(words[1],0)+ox;b.y=source_gpu_command_coord(words[1],16)+oy;
        b.clip_left=b.clip_top=clip;b.clip_right=1023-clip;b.clip_bottom=511-clip;
        b.interlace=field && !(mode&1024);b.skip_field=field==2;
        int extra=-1;assert(sw_draw_source_block(&b,&extra) && extra==0);
        uint8_t hash[32];psx_sha256_compute((uint8_t*)vram,sizeof(vram),hash);
        printf("%d ",256-s.budget);for(unsigned i=0;i<32;i++)printf("%02x",hash[i]);puts("");
    }
    SourceGPUCommandProjection s;source_gpu_command_cold(&s);
    s.budget=-1;s.clip_x1=1023;s.clip_y1=511;
    assert(source_gpu_command_write(&s,0x4000ff00));assert(source_gpu_command_write(&s,0x00a20054));
    assert(source_gpu_command_write(&s,0x00a20054));assert(s.count==3 && !s.dispatch.kind);
    assert(source_gpu_command_update(&s,1));assert(!s.count && s.budget==-17);
    source_gpu_command_cold(&s);assert(!source_gpu_command_write(&s,0x48000000));
    return 0;
}
