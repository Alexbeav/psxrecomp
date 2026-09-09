#ifndef PSX_SOURCE_GPU_POLYGON_PROJECTION_H
#define PSX_SOURCE_GPU_POLYGON_PROJECTION_H
#include <stdint.h>

/* Independent source-profile triangle traversal and work estimator.
 * Pixel values are intentionally absent. Directed edge traversal matters for
 * entering-clip work and fixed-point rounding, even when coverage looks equal. */
static inline int64_t source_poly_edge(int x) {
    return (int64_t)x*INT64_C(4294967296)+INT64_C(4294965248);
}
static inline int64_t source_poly_slope(int dx,int dy) {
    return ((int64_t)dx*INT64_C(4294967296)+(dx<0?-(dy-1):dx>0?dy-1:0))/dy;
}
/* Callback: raw Y, physical X, width and raw interpolation X. */
typedef void (*SourcePolySpan)(void *,int,int,int,int);
static inline int source_poly_walk(const int *input_x,const int *input_y,
                                  int clip_left,int clip_top,int clip_right,int clip_bottom,
                                  int doubled,int masked_or_blended,int interlace,unsigned skip_field,
                                  SourcePolySpan visit,void *context) {
    struct {int x,y,original;} v[3];
    int core= input_x[1]<=input_x[0] ? (input_x[2]<=input_x[1]?2:1) :
              input_x[2]<input_x[0]?2:0;
    for(int i=0;i<3;i++) {
        v[i].x=input_x[i];v[i].y=input_y[i];v[i].original=i;
        /* Original 11-bit vertex plus 11-bit drawing offset. */
        if(v[i].x < -2048 || v[i].x > 2046 || v[i].y < -2048 || v[i].y > 2046)return -1;
    }
    const unsigned pairs[3][2]={{1,2},{0,1},{1,2}};
    for(unsigned i=0;i<3;i++) {
        unsigned a=pairs[i][0],b=pairs[i][1];
        if(v[a].y>v[b].y) {
            int x=v[a].x,y=v[a].y,o=v[a].original;
            v[a]=v[b];v[b].x=x;v[b].y=y;v[b].original=o;
        }
    }
    for(int i=0;i<3;i++)if(v[i].original==core){core=i;break;}
    if(v[2].y==v[0].y || v[2].y-v[0].y>=512)return 0;
    for(int i=0;i<3;i++)for(int j=i+1;j<3;j++)
        if(v[i].x-v[j].x>=1024 || v[j].x-v[i].x>=1024)return 0;
    int area=(v[1].x-v[0].x)*(v[2].y-v[0].y)-(v[2].x-v[0].x)*(v[1].y-v[0].y);
    if(!area)return 0;
    int64_t long_step=source_poly_slope(v[2].x-v[0].x,v[2].y-v[0].y);
    int right= v[1].y==v[0].y ? v[1].x>v[0].x :
        source_poly_slope(v[1].x-v[0].x,v[1].y-v[0].y)>long_step;
    int cost=0;
    /* Source traversal starts at the leftmost core vertex. For a middle or
     * bottom core, visit the lower half before the upper half. Texture-cache
     * residency and texture reads of the draw destination observe this order. */
    for(int part=0;part<2;part++) {
        int half=core?1-part:part;
        int lo=half,hi=half+1;
        if(v[hi].y==v[lo].y)continue;
        int down=half==0?core==0:core!=2;
        int anchor=down?lo:hi;
        int64_t short_step=source_poly_slope(v[hi].x-v[lo].x,v[hi].y-v[lo].y);
        int begin=down?v[lo].y:v[hi].y-1,end=down?v[hi].y:v[lo].y-1,dy=down?1:-1;
        for(int y=begin;y!=end;y+=dy) {
            /* Clip signed11-bit Y; retain raw Y for interpolation. */
            int clip_y=((y&2047)^1024)-1024;
            if(down?clip_y>clip_bottom:clip_y<clip_top)break;
            if(down?clip_y<clip_top:clip_y>clip_bottom){cost+=2;continue;}
            if(interlace && ((unsigned)y&1u)==skip_field)continue;
            int lx=(int)((source_poly_edge(v[0].x)+long_step*(y-v[0].y))>>32);
            int sx=(int)((source_poly_edge(v[anchor].x)+short_step*(y-v[anchor].y))>>32);
            int left=right?lx:sx,bound=right?sx:lx;
            int width=bound-left;
            /* Source signs the left endpoint before applying its clip. */
            /* Keep the interpolation position separate from the wrapped
             * store address through the left clip adjustment. */
            int logical_left=left;
            left=((left&2047)^1024)-1024;
            if(left<clip_left){int delta=clip_left-left;logical_left+=delta;width-=delta;left=clip_left;}
            if(left+width>clip_right+1)width=clip_right+1-left;
            if(width>0) {
                cost+=doubled?2*width:masked_or_blended?width+(width+1)/2:width;
                if(visit)visit(context,y,left,width,logical_left);
            }
        }
    }
    return cost;
}
static inline int source_poly_cost(const int *x,const int *y,int l,int t,int r,int b,
                                  int doubled,int masked,int interlace,unsigned skip) {
    return source_poly_walk(x,y,l,t,r,b,doubled,masked,interlace,skip,0,0);
}
#endif
