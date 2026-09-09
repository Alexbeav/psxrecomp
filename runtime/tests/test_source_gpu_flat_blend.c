/* Flat polygon command flags: triangle/quad, blend enable, ignored raw bit.
 * The full authored VRAM oracle is tools/tasreplays/source_flat_oracle.py. */
#include "source_gpu_command_projection.h"
#include "gpu_sw_renderer.h"
#include <stdio.h>
#include <stdlib.h>

static uint16_t vram[1024*512];
static unsigned checks;
static int dump_vram;
int g_ws_bd_stretch_on,g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) {abort();}
static void check(int okay,const char *message) {
    checks++;if(!okay){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
}
static uint16_t expected_pixel(uint16_t back,unsigned opcode,unsigned blend,unsigned mask) {
    if((mask&2) && (back&0x8000))return back;
    unsigned pixel=(mask&1)?0x8000:0;
    for(unsigned c=0;c<3;c++) {
        int value=31,bg=(back>>(5*c))&31;
        if(opcode&2) {
            if(blend==0)value=(bg+31)/2;
            else if(blend==1)value=bg+31;
            else if(blend==2)value=bg-31;
            else value=bg+7;
        }
        if(value<0)value=0;if(value>31)value=31;
        pixel|=(unsigned)value<<(5*c);
    }
    return (uint16_t)pixel;
}
static void run(unsigned opcode,unsigned blend,unsigned mask) {
    uint32_t words[]={opcode<<24|0xffffff,0x00040004,0x00040008,0x00060004,0x00060008};
    SourceGPUCommandProjection s;source_gpu_command_cold(&s);
    s.clip_x1=1023;s.clip_y1=511;s.budget=256;s.mask_bits=mask;s.draw_mode=blend<<5;
    check(source_gpu_polygon_supported(opcode),"flat polygon family admitted");
    check(source_gpu_command_length(words[0])==4,"first polygon packet has three vertices");
    for(unsigned i=0;i<3;i++) {
        check(source_gpu_command_write(&s,words[i]),"incomplete polygon queues");
        check(s.dispatch.kind==SOURCE_GPU_DISPATCH_NONE,"incomplete polygon cannot render");
    }
    check(source_gpu_command_write(&s,words[3]),"first triangle dispatch");
    unsigned first_cost=((opcode&2)||(mask&2))?9:6;
    check(s.budget==256-84-(int)first_cost,"first triangle retains exact setup and pixel work");
    check(s.dispatch.kind==((opcode&8)?SOURCE_GPU_DISPATCH_QUAD_FIRST:SOURCE_GPU_DISPATCH_COMMAND),"triangle versus split-quad dispatch");
    if(opcode&8) {
        check(source_gpu_command_write(&s,words[4]),"quad second triangle dispatch");
        check(s.dispatch.kind==SOURCE_GPU_DISPATCH_QUAD_SECOND && s.dispatch.count==5 && !s.phase,
              "quad retains original fourth vertex and split completion");
        unsigned second_cost=((opcode&2)||(mask&2))?3:2;
        check(s.budget==256-84-(int)first_cost-46-(int)second_cost,"second triangle exact work");
    }
    memset(vram,0,sizeof(vram));
    for(unsigned y=4;y<6;y++)for(unsigned x=4;x<8;x++)vram[y*1024+x]=(uint16_t)(0x1234|((x&1)?0x8000:0));
    sw_renderer_init(vram);sw_source_texture_control(0,0);sw_set_mask_bits(mask&1,!!(mask&2));
    sw_set_semi_transparency(!!(opcode&2),blend);
    int x[]={4,8,4,8},y[]={4,4,6,6},extra=-1;
    uint32_t colors[]={0xffffff,0xffffff,0xffffff,0xffffff};
    check(sw_draw_source_triangle(x,y,colors,0,0,0,0,NULL,&extra) && extra==0,"flat first triangle rendering");
    if(opcode&8)check(sw_draw_source_triangle(x+1,y+1,colors+1,0,0,0,0,NULL,&extra) && extra==0,"flat second triangle rendering");
    for(unsigned yy=4;yy<6;yy++)for(unsigned xx=4;xx<8;xx++) {
        uint16_t back=(uint16_t)(0x1234|((xx&1)?0x8000:0));
        int covered=(opcode&8) || xx<8-2*(yy-4);
        check(vram[yy*1024+xx]==(covered?expected_pixel(back,opcode,blend,mask):back),"flat pixel coverage/blend/mask/raw-bit behavior");
    }
    if(dump_vram) {
        char name[64];snprintf(name,sizeof(name),"op%02x-b%u-k%u.bin",opcode,blend,mask);
        FILE *f=fopen(name,"wb");check(f!=NULL,"open authored flat VRAM");
        check(fwrite(vram,1,sizeof(vram),f)==sizeof(vram) && fclose(f)==0,"write entire authored VRAM");
    }
}
int main(int argc,char **argv) {
    dump_vram=argc==2 && strcmp(argv[1],"--dump")==0;
    const unsigned opcodes[]={0x20,0x21,0x22,0x23,0x28,0x29,0x2a,0x2b};
    for(unsigned op=0;op<8;op++)for(unsigned blend=0;blend<4;blend++)for(unsigned mask=0;mask<4;mask++)run(opcodes[op],blend,mask);
    SourceGPUCommandProjection s;source_gpu_command_cold(&s);
    check(!source_gpu_command_write(&s,0x40000000) && s.error==SOURCE_GPU_COMMAND_UNSUPPORTED,"unqualified line commands still fail closed");
    printf("source_gpu_flat_blend: %u checks passed\n",checks);
}
