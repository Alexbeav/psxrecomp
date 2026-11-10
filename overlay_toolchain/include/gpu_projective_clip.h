#ifndef PSX_GPU_PROJECTIVE_CLIP_H
#define PSX_GPU_PROJECTIVE_CLIP_H

/* Clip before dividing by signed camera depth. Attributes follow the same
 * edge parameter, so clipped triangles retain their original texture plane. */
typedef struct PSXProjectedVertex {
    double x, y, z, u, v, r, g, b;
} PSXProjectedVertex;

static inline PSXProjectedVertex psx_projective_lerp(
    PSXProjectedVertex a, PSXProjectedVertex b, double t) {
    PSXProjectedVertex p;
#define LERP(f) p.f = a.f + (b.f-a.f)*t
    LERP(x); LERP(y); LERP(z); LERP(u); LERP(v);
    LERP(r); LERP(g); LERP(b);
#undef LERP
    return p;
}
static inline double psx_projective_distance(PSXProjectedVertex p, int plane,
    double left, double right, double top, double bottom) {
    switch (plane) {
    case 0: return p.z - 1.0;
    case 1: return p.x - left*p.z;
    case 2: return right*p.z - p.x;
    case 3: return p.y - top*p.z;
    default: return bottom*p.z - p.y;
    }
}
static inline int psx_projective_clip_triangle(const PSXProjectedVertex in[3],
    PSXProjectedVertex out[12], double left, double right, double top, double bottom) {
    PSXProjectedVertex a[12], b[12];
    int n=3;
    for (int i=0;i<3;++i) a[i]=in[i];
    for (int plane=0;plane<5 && n;++plane) {
        int m=0;
        PSXProjectedVertex prev=a[n-1];
        double dp=psx_projective_distance(prev,plane,left,right,top,bottom);
        for (int i=0;i<n;++i) {
            PSXProjectedVertex cur=a[i];
            double dc=psx_projective_distance(cur,plane,left,right,top,bottom);
            if ((dp>=0)!=(dc>=0))
                b[m++]=psx_projective_lerp(prev,cur,dp/(dp-dc));
            if (dc>=0) b[m++]=cur;
            prev=cur; dp=dc;
        }
        n=m;
        for (int i=0;i<n;++i) a[i]=b[i];
    }
    for (int i=0;i<n;++i) out[i]=a[i];
    return n;
}
#endif
