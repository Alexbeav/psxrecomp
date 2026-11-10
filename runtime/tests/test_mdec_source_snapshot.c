/* Authored optional-wire validation and modern activity compatibility. */
#define _POSIX_C_SOURCE 200809L
#include "mdec.c"
uint64_t s_frame_count,psx_cycle_count;
int debug_server_fmv_quiet(void){return 0;}
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void option(const char *s){
#ifdef _WIN32
 _putenv_s("PSX_MDEC_SOURCE_MODEL",s);
#else
 setenv("PSX_MDEC_SOURCE_MODEL",s,1);
#endif
}
static void wire(void){
 uint32_t n=mdec_snapshot_bytes();uint8_t *a=malloc(n),*b=malloc(n),*bad=malloc(n);
 CHECK(a&&b&&bad);mdec_snapshot_write(a);CHECK(mdec_snapshot_validate(a,n));
 CHECK(!mdec_snapshot_validate(a,n-1));CHECK(!mdec_snapshot_read(a,n-1));
 /* Magic, model cost, FIFO indices/counts, phase, credit, encoded count. */
 uint32_t offsets[]={0,4,8+448,8+452,8+472,8+484,8+508};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i){
  memcpy(bad,a,n);uint32_t off=n-SOURCE_MDEC_TAIL_BYTES+offsets[i];
  memset(bad+off,0xff,4);bad[off+3]=0x7f;CHECK(!mdec_snapshot_validate(bad,n));CHECK(!mdec_snapshot_read(bad,n));
  mdec_snapshot_write(b);CHECK(!memcmp(a,b,n));
 }
 mdec_source_advance(128);CHECK(mdec_snapshot_read(a,n));mdec_snapshot_write(b);CHECK(!memcmp(a,b,n));
 free(a);free(b);free(bad);
}
int main(void){
 const char *models[]={"octoshock-2.3","nymashock-1.29.0","nymashock-1.32.1"};
 for(unsigned i=0;i<3;++i){option(models[i]);psx_cycle_count=1000;mdec_init();CHECK(mdec_source_active());
 CHECK(source_mdec.block_cycles==(i?512u:474u));wire();
 mdec_write(4,0x60000000u);mdec_write(0,0x28000001u);mdec_write(0,0xfe000001u);
 mdec_source_advance(128);wire();mdec_source_advance(128);wire();
 for(unsigned j=0;j<10;++j)mdec_source_advance(128);
 uint16_t block[]={1,0xfe00};uint32_t pixels[48];
 source_decode_block(NULL,0x38000000u,2,block,2,pixels);
 CHECK(mdec_get_decode_count()>0);CHECK(mdec_last_color_decode_cycle==1000);wire();
 }
 option("");mdec_init();CHECK(!mdec_source_active());CHECK(mdec_snapshot_bytes()==mdec_snap_fixed_bytes());
 printf("MDEC optional snapshot/activity: %u checks PASS\n",checks);return 0;
}
