/* The original Octoshock2.3 sprite table admits 0x66 as a four-word packet
 * with three-word FIFO feedback. DrawSprite charges 16 + clipped width +
 * aligned halfword blend/mask work, plus the renderer's texture-cache work.
 * These are source compatibility checks, not PS1 hardware timing claims. */
#include "source_gpu_command_projection.h"
#include "gpu_sw_renderer.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static int dump_vram;
static uint16_t vram[1024*512];
/* Enhancement-only integration is absent from this native sprite fixture.
 * Fail if the tested path unexpectedly enters it. */
int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { abort(); }
static void check(int okay,const char *message) {
    checks++;
    if(!okay){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
}

/* Per-channel reference arithmetic derived from the source PlotPixel and
 * ModTexel contracts. Tested separately from the production pixel function. */
static uint16_t expected_pixel(uint16_t back,uint16_t texel,unsigned color,
                               unsigned blend,unsigned mask) {
    if(!texel || ((mask&2) && (back&0x8000)))return back;
    unsigned pixel=texel&0x8000;
    for(unsigned c=0;c<3;c++) {
        int fore=(((texel>>(5*c))&31)*((color>>(8*c))&255))>>7;
        if(fore>31)fore=31;
        int bg=(back>>(5*c))&31;
        if(texel&0x8000) {
            if(blend==0)fore=(bg+fore)/2;
            else if(blend==1)fore=bg+fore;
            else if(blend==2)fore=bg-fore;
            else fore=bg+fore/4;
        }
        if(fore<0)fore=0;if(fore>31)fore=31;
        pixel|=(unsigned)fore<<(5*c);
    }
    return (uint16_t)(pixel|((mask&1)?0x8000:0));
}

static void pixels(unsigned mode,unsigned blend,unsigned mask,unsigned color) {
    memset(vram,0,sizeof(vram));
    const uint16_t texels[]={0,0x801f,0x03e0,0xfc00};
    uint16_t original[4];
    for(unsigned i=0;i<4;i++) {
        vram[300*1024+i]=texels[i];
        original[i]=vram[4*1024+4+i]=(uint16_t)(0x1234|((i&1)?0x8000:0));
    }
    if(mode==0)vram[256*1024]=0x3210;
    else if(mode==1){vram[256*1024]=0x0100;vram[256*1024+1]=0x0302;}
    else memcpy(vram+256*1024,texels,sizeof(texels));
    sw_renderer_init(vram);sw_source_texture_control(0,0);
    sw_set_mask_bits(mask&1,!!(mask&2));
    sw_set_semi_transparency(1,blend);
    uint32_t words[]={0x66000000|color,0x00040004,300u<<22,0x00010004};
    SourceGPUBlock b={0};b.words=words;b.x=4;b.y=4;b.clip_right=1023;b.clip_bottom=511;
    b.draw_mode=16|(mode<<7)|(blend<<5);
    int extra=-1;
    check(sw_draw_source_block(&b,&extra),"0x66 software renderer admission");
    check(extra==(mode==0?20:mode==1?260:4),"full CLUT/texture cache charge, including transparent or masked pixels");
    for(unsigned i=0;i<4;i++)check(vram[4*1024+4+i]==expected_pixel(original[i],texels[i],color,blend,mask),
                                  "textured blend / opaque / transparent / destination mask pixel");
    check(vram[4*1024+3]==0 && vram[4*1024+8]==0,"rectangle does not write neighbors");
    if(dump_vram) {
        char name[80];snprintf(name,sizeof(name),"m%u-b%u-k%u-c%06x.bin",mode,blend,mask,color);
        FILE *f=fopen(name,"wb");check(f!=NULL,"open authored VRAM snapshot");
        check(fwrite(vram,1,sizeof(vram),f)==sizeof(vram) && fclose(f)==0,"write full authored VRAM snapshot");
    }
    check(sw_draw_source_block(&b,&extra) && extra==0,"repeat uses retained texture/CLUT cache");
}

int main(int argc,char **argv) {
    dump_vram=argc==2 && strcmp(argv[1],"--dump")==0;
    const uint32_t words[]={0x66808080,0x00040004,300u<<22,0x00010004};
    check(source_gpu_command_length(words[0])==4,"0x66 packet length");
    check(source_gpu_command_feedback_length(words[0])==3,"0x66 FIFO feedback length");
    SourceGPUCommandProjection s;source_gpu_command_cold(&s);
    s.clip_x1=1023;s.clip_y1=511;s.budget=100;
    for(unsigned i=0;i<3;i++) {
        check(source_gpu_command_write(&s,words[i]),"incomplete 0x66 packet is admitted");
        check(s.dispatch.kind==SOURCE_GPU_DISPATCH_NONE && s.budget==100,"no partial sprite dispatch or charge");
    }
    check(!source_gpu_command_ready(&s),"three-word feedback blocks DMA before fourth word");
    check(source_gpu_command_write(&s,words[3]),"complete 0x66 packet dispatches");
    check(s.dispatch.kind==SOURCE_GPU_DISPATCH_COMMAND && s.dispatch.count==4 && !s.count && s.budget==76,
          "four words dispatch once, with two setup clocks plus 22 sprite clocks");
    s.clip_x0=5;s.clip_x1=6;check(source_gpu_command_block_cost(&s,words)==20,"clipped odd-start blend work");
    s.clip_x0=0;s.clip_x1=1023;s.budget=-2;
    for(unsigned i=0;i<4;i++)check(source_gpu_command_write(&s,words[i]),"negative-credit packet queues");
    check(s.count==4 && s.dispatch.kind==SOURCE_GPU_DISPATCH_NONE,"negative credit cannot render");
    check(source_gpu_command_update(&s,1) && s.budget==-24 && s.count==0,"exact zero credit releases sprite");
    for(unsigned mode=0;mode<3;mode++)for(unsigned blend=0;blend<4;blend++)for(unsigned mask=0;mask<4;mask++) {
        pixels(mode,blend,mask,0x808080);
        pixels(mode,blend,mask,0xe3942b);
    }
    printf("source_gpu_sprite_blend: %u checks passed\n",checks);
}
