#include "gpu_uv.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); ++failures; } } while(0)

/* Museum doorway packets captured from MediEvil II. Integer-rounded screen
 * derivatives classify only one half as a mirrored sprite. Shared world UVs
 * must stay identical on both halves, throughout camera settling. */
static void doorway(void) {
    const int x[4]={216,315,215,315}, y[4]={98,97,159,160};
    const int u[4]={95,49,95,49}, v[4]={151,151,197,197};
    const int triangles[2][3]={{0,1,2},{2,1,3}};
    for (int perturb=-1;perturb<=1;++perturb) {
        int saved_u[2][3],saved_v[2][3];
        for (int t=0;t<2;++t) {
            int xs[3],ys[3],us[3],vs[3],lim[4];
            for (int i=0;i<3;++i) {
                int j=triangles[t][i];
                xs[i]=x[j];ys[i]=y[j]+(j==0?perturb:0);
                us[i]=u[j];vs[i]=v[j];
            }
            psx_uv_tri_center_sample(xs,ys,us,vs,1,lim);
            for (int i=0;i<3;++i) {
                CHECK(us[i]==u[triangles[t][i]],"world U unchanged as camera rounds");
                CHECK(vs[i]==v[triangles[t][i]],"world V unchanged as camera rounds");
            }
            CHECK(lim[0]==49 && lim[2]==95,"world U atlas bounds include both ends");
            CHECK(lim[1]==151 && lim[3]==197,"world V atlas bounds include both ends");
            memcpy(saved_u[t],us,sizeof us);memcpy(saved_v[t],vs,sizeof vs);
        }
        CHECK(saved_u[0][1]==saved_u[1][1] && saved_u[0][2]==saved_u[1][0],
              "shared diagonal has identical U on both sides");
        CHECK(saved_v[0][1]==saved_v[1][1] && saved_v[0][2]==saved_v[1][0],
              "shared diagonal has identical V on both sides");
    }
}

static void sprites(void) {
    const int x[3]={0,8,0},y[3]={0,0,8};
    int u[3]={8,0,8},v[3]={8,8,0},lim[4];
    psx_uv_tri_center_sample(x,y,u,v,0,lim);
    CHECK(u[0]==9 && u[1]==1 && u[2]==9,"mirrored affine U keeps one-texel correction");
    CHECK(v[0]==9 && v[1]==9 && v[2]==1,"mirrored affine V keeps one-texel correction");
    CHECK(lim[0]==1 && lim[2]==8 && lim[1]==1 && lim[3]==8,
          "mirrored sprites exclude never-sampled texels");
    int forward_u[3]={0,8,0},forward_v[3]={0,0,8};
    psx_uv_tri_center_sample(x,y,forward_u,forward_v,0,lim);
    CHECK(forward_u[0]==0 && forward_u[1]==8 && forward_v[2]==8,"forward sprites retain authored UVs");
    CHECK(lim[0]==0 && lim[2]==7 && lim[1]==0 && lim[3]==7,"forward sprites keep exclusive endpoints");
    int rect_u0=8,rect_u1=0,rect_v0=8,rect_v1=0;
    psx_uv_rect_mirror_offset(&rect_u0,&rect_v0,&rect_u1,&rect_v1);
    CHECK(rect_u0==9 && rect_u1==1 && rect_v0==9 && rect_v1==1,"rect sprite correction unchanged");
}

static void wrapping(void) {
    const int u[3]={250,260,250},v[3]={10,10,20};int lim[4];
    psx_uv_tri_world_limits(u,v,lim);
    CHECK(lim[0]==0 && lim[2]==255,"world page wrapping remains unclamped");
    CHECK(lim[1]==10 && lim[3]==20,"other axis retains authored bounds");
}

int main(void) {
    doorway();sprites();wrapping();
    if(failures)return 1;
    puts("ALL PASS");return 0;
}
