/* Authored triangle traversal/cache fixture; no retail input or state.
 * First the documented coordinate edges ("edge" lines), then the 144 stock-core
 * cases. The contracts script compares both. */
#include "source_gpu_command_projection.h"
#include "gpu_sw_renderer.h"
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "source_gpu_polygon_wrap_geometries.h"
static uint16_t vram[1024*512],saved[1024*512];
int g_ws_bd_stretch_on,g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void){abort();}
static void draw(const int *x,const int *y,unsigned mode,unsigned feedback,unsigned shaded,
                 int work[2],uint8_t hash[2][32]) {
    memset(vram,0,sizeof(vram));
    unsigned ty=feedback?0:256;
    for(unsigned y=0;y<128;y++)for(unsigned x=0;x<256;x++)
        vram[(y+ty)*1024+x]=(uint16_t)(((x*37u+y*113u+1u)&32767u)|32768u);
    for(unsigned i=0;i<256;i++)vram[400*1024+i]=(uint16_t)(((i*97u+1u)&32767u)|32768u);
    uint32_t colors[3]={0x808080,0x808080,0x808080};if(shaded){colors[0]=0xe3942b;colors[1]=0x285eb0;colors[2]=0x76c341;}
    SourceGPUTexture texture={0};texture.page=(feedback?0:16)|(mode<<7);texture.clut=400<<6;
    texture.load_clut=1;texture.uv[0]=0x0301;texture.uv[1]=0x65f7;texture.uv[2]=0x7d0e;
    sw_renderer_init(vram);sw_source_texture_control(0,0);sw_source_texture_control(3,texture.page);
    sw_set_draw_area(0,0,1023,511);sw_set_mask_bits(0,0);sw_set_semi_transparency(0,0);
    for(unsigned pass=0;pass<2;pass++) {
        int extra=-1; if(!sw_draw_source_triangle(x,y,colors,shaded,shaded,0,0,&texture,&extra)){puts("walk failed");exit(1);}
        /* Set-up of the first triangle half: 24h textured, 34h gouraud textured. */
        work[pass]=SOURCE_GPU_T_POLYGON_SETUP(shaded?0x34u:0x24u,0)+source_poly_cost(x,y,0,0,1023,511,1,0,0,0)+extra;
        psx_sha256_compute((uint8_t*)vram,sizeof(vram),hash[pass]);
    }
}
static void sums(unsigned core,int truncate,int *x,int *y) {
    for(unsigned i=0;i<3;i++){
        x[i]=geometry[core].x[i]+geometry[core].offset[0];y[i]=geometry[core].y[i]+geometry[core].offset[1];
        if(truncate){x[i]=source_poly_signed_coordinate(x[i]);y[i]=source_poly_signed_coordinate(y[i]);}
    }
}
static void run(unsigned core,unsigned mode,unsigned feedback,unsigned shaded) {
    int x[3],y[3],work[2];uint8_t hash[2][32];sums(core,0,x,y);
    draw(x,y,mode,feedback,shaded,work,hash);
    printf("shade%u-geometry%u-m%u-f%u",shaded,core,mode,feedback);
    for(unsigned pass=0;pass<2;pass++){printf(" %d ",work[pass]);for(unsigned i=0;i<32;i++)printf("%02x",hash[pass][i]);}
    puts("");
}

static void edge(const char *name,int ok){printf("edge %s %s\n",name,ok?"PASS":"FAIL");}
static int changed(void){for(size_t i=0;i<1024*512;i++)if(vram[i])return 1;return 0;}
/* One flat white triangle on empty VRAM; the walk's work, -1 if it fails. */
static int flat(int x0,int y0,int x1,int y1,int x2,int y2) {
    int x[3]={x0,x1,x2},y[3]={y0,y1,y2},extra=-1;uint32_t colors[3]={0xffffff,0xffffff,0xffffff};
    memset(vram,0,sizeof(vram));sw_renderer_init(vram);
    sw_set_draw_area(0,0,1023,511);sw_set_mask_bits(0,0);sw_set_semi_transparency(0,0);
    if(!sw_draw_source_triangle(x,y,colors,0,0,0,0,NULL,&extra)||extra)return -1;
    return source_poly_cost(x,y,0,0,1023,511,0,0,0,0);
}
static int drawn(int work){return work>0&&changed();}
static int dropped(int work){return work==0&&!changed();}
/* One flat white line (GP0 40h) on empty VRAM, first vertex as its origin. */
static uint32_t vertex(int x,int y){return ((uint32_t)x&0x7FFu)|(((uint32_t)y&0x7FFu)<<16);}
static int line(int x0,int y0,int x1,int y1) {
    uint32_t words[3]={0x40FFFFFFu,vertex(x0,y0),vertex(x1,y1)};int extra=-1;
    SourceGPUBlock b={0};b.words=words;b.x=x0;b.y=y0;b.clip_right=1023;b.clip_bottom=511;
    memset(vram,0,sizeof(vram));sw_renderer_init(vram);sw_set_mask_bits(0,0);sw_set_semi_transparency(0,0);
    return sw_draw_source_block(&b,&extra)==1&&extra==0&&changed();
}
/* A flat triangle draws the same pixels and work from its raw sums and from
 * the sums truncated to signed 11 bits. */
static int same_after_truncation(const int *x,const int *y) {
    int tx[3],ty[3];for(unsigned i=0;i<3;i++){tx[i]=source_poly_signed_coordinate(x[i]);ty[i]=source_poly_signed_coordinate(y[i]);}
    int raw=flat(x[0],y[0],x[1],y[1],x[2],y[2]);memcpy(saved,vram,sizeof(vram));
    int truncated=flat(tx[0],ty[0],tx[1],ty[1],tx[2],ty[2]);
    return raw>=0&&raw==truncated&&!memcmp(saved,vram,sizeof(vram));
}
static int same_period(const int *v){
    for(unsigned i=1;i<3;i++)if(source_poly_signed_coordinate(v[i])-v[i]!=source_poly_signed_coordinate(v[0])-v[0])return 0;
    return 1;
}
static void edges(void) {
    /* PSX-SPX "Vertex", Size Restriction: at most 1023 apart in x and 511 in
     * y; larger polygons and lines are not rendered. Offscreen parts count. */
    edge("triangle-x1023-drawn",drawn(flat(0,10,1023,10,0,110)));
    edge("triangle-x1024-dropped",dropped(flat(0,10,1024,10,0,110)));
    edge("triangle-y511-drawn",drawn(flat(10,0,110,0,10,511)));
    edge("triangle-y512-dropped",dropped(flat(10,0,110,0,10,512)));
    edge("triangle-x1023-offscreen-drawn",drawn(flat(-600,10,423,10,-600,110)));
    edge("triangle-x1024-offscreen-dropped",dropped(flat(-601,10,423,10,-601,110)));
    edge("line-x1023-drawn",line(0,5,1023,5));
    edge("line-x1024-dropped",!line(-1,5,1023,5));
    edge("line-y511-drawn",line(5,0,5,511));
    edge("line-y512-dropped",!line(5,-1,5,511));
    /* The line charge: set-up plus steps, set-up alone when not rendered. */
    edge("line-cost-x1023",source_gpu_t_line(0x40u,0,1023,0,1)==SOURCE_GPU_T_LINE_SETUP+SOURCE_GPU_T_LINE_STEP*1023);
    edge("line-cost-x1024",source_gpu_t_line(0x40u,0,1024,0,1)==SOURCE_GPU_T_LINE_SETUP);
    edge("line-cost-y511",source_gpu_t_line(0x40u,0,0,511,512)==SOURCE_GPU_T_LINE_SETUP+SOURCE_GPU_T_LINE_STEP*511);
    edge("line-cost-y512",source_gpu_t_line(0x40u,0,0,512,512)==SOURCE_GPU_T_LINE_SETUP);
    /* A signed 11-bit vertex plus a signed 11-bit offset (PSX-SPX "Vertex",
     * "GP0(E5h)") spans -2048..2046. No sum in that range is refused, and the
     * result equals the sum truncated to signed 11 bits (No$PSX GP0(E5h)). */
    { int x[3]={-2048,-2000,-2020},y[3]={10,10,60};edge("sum-x-2048-wraps",same_after_truncation(x,y)&&drawn(flat(0,10,48,10,28,60))); }
    { int x[3]={2046,2000,2020},y[3]={10,10,60};edge("sum-x2046-wraps",same_after_truncation(x,y)); }
    { int x[3]={10,60,10},y[3]={-2048,-2000,-2020};edge("sum-y-2048-wraps",same_after_truncation(x,y)&&drawn(flat(10,0,60,48,10,28))); }
    { int x[3]={10,60,10},y[3]={2046,2000,2020};edge("sum-y2046-wraps",same_after_truncation(x,y)); }
    /* Each stock geometry, in all twelve settings: the same images and work
     * from raw and from truncated sums. Where the three sums wrap alike this
     * must hold. Geometries 7 and 8 straddle in y (-1060 | -1000, -1020 and
     * 1010 | 1050, 1040): raw they lie outside the drawing area, truncated
     * they are too tall, so neither draws and the two still agree. */
    for(unsigned core=0;core<12;core++) {
        if(core==10)continue;
        int x[3],y[3],tx[3],ty[3];sums(core,0,x,y);sums(core,1,tx,ty);
        int alike=same_period(x)&&same_period(y),ok=alike||core==7||core==8;
        for(unsigned shaded=0;shaded<2;shaded++)for(unsigned mode=0;mode<3;mode++)for(unsigned feedback=0;feedback<2;feedback++) {
            int w[2],tw[2];uint8_t h[2][32],th[2][32];
            draw(x,y,mode,feedback,shaded,w,h);draw(tx,ty,mode,feedback,shaded,tw,th);
            ok&=!memcmp(w,tw,sizeof(w))&&!memcmp(h,th,sizeof(h));
        }
        char name[48];snprintf(name,sizeof(name),"%s-geometry%u-truncation-equal",alike?"wrap":"straddle",core);edge(name,ok);
    }
    /* Geometry 10 straddles +1023/+1024 in x (1000 | 1100, 1110). The stock
     * core keeps the raw sums: 110 apart, drawn (its cases below). Truncated
     * first (1000, -948, -938) they are 1948 apart and would not be drawn. */
    { int x[3],y[3],tx[3],ty[3];sums(10,0,x,y);sums(10,1,tx,ty);
      edge("straddle-geometry10-raw-drawn-truncated-dropped",
           !same_period(x)&&drawn(flat(x[0],y[0],x[1],y[1],x[2],y[2]))&&dropped(flat(tx[0],ty[0],tx[1],ty[1],tx[2],ty[2]))); }
}
int main(void){edges();for(unsigned shaded=0;shaded<2;shaded++)for(unsigned core=0;core<12;core++)for(unsigned mode=0;mode<3;mode++)for(unsigned feedback=0;feedback<2;feedback++)run(core,mode,feedback,shaded);return 0;}
