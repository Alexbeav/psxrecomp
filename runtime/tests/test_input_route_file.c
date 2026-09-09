#include "input_route_file.h"
#include <stdlib.h>

static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}
static void le32(unsigned char *p, uint32_t n) {
    for (unsigned i=0; i<4; ++i) p[i]=(unsigned char)(n>>(8*i));
}
static FILE *fixture(unsigned n, int fault) {
    unsigned char h[24] = "PSXRTI1", r[8] = {0};
    FILE *f=tmpfile(); check(f != NULL, "tmpfile");
    le32(h+8,1); le32(h+12,8); le32(h+16,n);
    if (fault==1) h[7]=1;
    if (fault==2) h[8]=2;
    if (fault==3) h[12]=9;
    if (fault==4) h[20]=1;
    fwrite(h,1,fault==5 ? 23 : 24,f);
    if (n <= INPUT_ROUTE_MAX_FRAMES) for (unsigned i=0;i<n;++i) {
        le32(r,i+1); r[4]=(unsigned char)(fault==10 ? (i&1) : (i>=2 ? 1 : 0));
        if (fault==6 && i==1) le32(r,i);
        if (fault==7) r[6]=1;
        fwrite(r,1,fault==8 && i==n-1 ? 7 : 8,f);
    }
    if (fault==9) fputc(0,f);
    rewind(f); return f;
}
int main(void) {
    InputRouteStep steps[INPUT_ROUTE_MAX_STEPS]; uint32_t count,n;
    FILE *f=fixture(5,0);
    check(input_route_read(f,steps,&count,&n)==NULL,"valid route"); fclose(f);
    check(n==5 && count==2 && steps[0].frames==2 && steps[0].buttons==0 &&
          steps[1].frames==3 && steps[1].buttons==1,"exact RLE and first/last input");
    for (int fault=1;fault<=9;++fault) {
        f=fixture(5,fault);
        check(input_route_read(f,steps,&count,&n)!=NULL,"bad input rejected"); fclose(f);
        check(!count && !n,"failure cannot publish a partial route");
    }
    f=fixture(0,0); check(input_route_read(f,steps,&count,&n)!=NULL,"empty rejected"); fclose(f);
    f=fixture(INPUT_ROUTE_MAX_FRAMES+1,0);
    check(input_route_read(f,steps,&count,&n)!=NULL,"frame bound"); fclose(f);
    f=fixture(INPUT_ROUTE_MAX_STEPS,10);
    check(input_route_read(f,steps,&count,&n)==NULL && count==INPUT_ROUTE_MAX_STEPS,"capacity boundary"); fclose(f);
    f=fixture(INPUT_ROUTE_MAX_STEPS+1,10);
    check(input_route_read(f,steps,&count,&n)!=NULL && !count && !n,"step overflow rejected"); fclose(f);
    f=fixture(INPUT_ROUTE_MAX_FRAMES,0);
    check(input_route_read(f,steps,&count,&n)==NULL && n==INPUT_ROUTE_MAX_FRAMES,"frame capacity boundary"); fclose(f);
    puts("input_route_file: 15 parser cases passed"); return 0;
}
