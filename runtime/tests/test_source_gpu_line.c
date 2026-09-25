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
    unsigned op,color,mode,mask,field,color1;
    int x0,y0,x1,y1,ox,oy,clip;
    while(scanf("%u %u %d %d %d %d %d %d %u %u %u %d %u",&op,&color,&x0,&y0,&x1,&y1,&ox,&oy,&mode,&mask,&field,&clip,&color1)==13) {
        unsigned n=3+!!(op&0x10);
        uint32_t words[4]={op<<24|color,((uint32_t)y0&65535u)<<16|((uint32_t)x0&65535u),color1,0};
        words[n-1]=((uint32_t)y1&65535u)<<16|((uint32_t)x1&65535u);
        SourceGPUCommandProjection s;source_gpu_command_cold(&s);
        s.clip_x0=s.clip_y0=clip;s.clip_x1=1023-clip;s.clip_y1=511-clip;
        s.budget=256;s.offset_x=ox;s.offset_y=oy;s.draw_mode=mode;
        s.display_mode=field?0x24:0;s.field_valid=1;s.skip_field=field==2;s.mask_bits=mask;
        assert(source_gpu_command_length(words[0])==n);
        assert(source_gpu_command_feedback_length(words[0])==1);
        for(unsigned i=0;i<n-1;i++){assert(source_gpu_command_write(&s,words[i]));assert(s.dispatch.kind==0);}
        assert(!source_gpu_command_ready(&s));
        assert(source_gpu_command_write(&s,words[n-1]));
        assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==n && !s.count);
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
    assert(source_gpu_command_update(&s,1));assert(!s.count && s.budget==-43); /* No$PSX Lines (a3b2131f3774...): 40 + 1 px + 2 */
    /* Poly-lines. LINE_HELPER gives every entry in 0x40-0x5F the same len, 3 + goraud, so an
     * opening poly-line packet is its two-vertex counterpart's shape; INCMD_PLINE then takes
     * 1 + goraud words per segment until a terminator word, tested before the segment length.
     * The main FIFO tail's -2 is never reached from that branch, so only the opening packet
     * pays it: the opening line here costs 2 + 16 + 2*16 = 50 and each segment 16 + 2*16 = 48. */
    source_gpu_command_cold(&s);
    s.budget=4096;s.clip_x1=1023;s.clip_y1=511;
    assert(source_gpu_command_length(0x48000000u)==3);
    assert(source_gpu_command_feedback_length(0x48000000u)==1);
    assert(source_gpu_command_write(&s,0x48804020u));
    assert(source_gpu_command_write(&s,0x00000000u));
    assert(source_gpu_command_write(&s,0x00000010u));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==3 && !s.count);
    assert(s.pline && s.pline_command==0x48u && s.budget==4096-59); /* No$PSX Lines: 40 + 17 px + 2 */
    assert(!source_gpu_command_ready(&s));
    assert(source_gpu_command_write(&s,0x00000020u));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==3 && !s.count);
    assert(s.dispatch.words[0]==((0x48u<<24)|0x804020u));
    assert(s.dispatch.words[1]==0x00000010u && s.dispatch.words[2]==0x00000020u);
    assert(s.pline && s.budget==4096-59-59); /* No$PSX Lines, per segment */
    assert(source_gpu_command_write(&s,0x55555555u));
    assert(!s.pline && !s.count && s.dispatch.kind==SOURCE_GPU_DISPATCH_NONE);
    assert(source_gpu_command_ready(&s));
    /* Shaded poly-line: len 4, and each segment carries its own colour word first. */
    source_gpu_command_cold(&s);
    s.budget=4096;s.clip_x1=1023;s.clip_y1=511;
    assert(source_gpu_command_length(0x58000000u)==4);
    assert(source_gpu_command_write(&s,0x58111111u));
    assert(source_gpu_command_write(&s,0x00000000u));
    assert(source_gpu_command_write(&s,0x00222222u));
    assert(source_gpu_command_write(&s,0x00000010u));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==4);
    assert(s.pline && s.pline_command==0x58u && s.pline_color==0x222222u);
    assert(source_gpu_command_write(&s,0x00333333u));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_NONE && s.count==1);
    assert(source_gpu_command_write(&s,0x00000020u));
    assert(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==4 && !s.count);
    assert(s.dispatch.words[0]==((0x58u<<24)|0x222222u) && s.dispatch.words[1]==0x00000010u);
    assert(s.dispatch.words[2]==0x00333333u && s.dispatch.words[3]==0x00000020u);
    assert(s.pline && s.pline_color==0x333333u);
    /* A terminator is recognised by its nibble pattern, not an exact word, and ends the
     * command even with a full segment already queued behind it. */
    assert(source_gpu_command_write(&s,0x5fff5abcu));
    assert(!s.pline && !s.count);
    return 0;
}
