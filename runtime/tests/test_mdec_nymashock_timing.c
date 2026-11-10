/* Existing source block deadlines and model-bound continuation, through the MDEC API. */
#include <assert.h>
#include <stdlib.h>
#include "mdec.c"
uint64_t s_frame_count,psx_cycle_count;
int debug_server_fmv_quiet(void){return 0;}
static void model(const char *name) {
#ifdef _WIN32
 _putenv_s("PSX_MDEC_SOURCE_MODEL",name);
#else
 setenv("PSX_MDEC_SOURCE_MODEL",name,1);
#endif
 mdec_init();
}
int main(void) {
 const char *models[]={"octoshock-2.3","nymashock-1.29.0","nymashock-1.32.1","octoshock-2.4.2","octoshock-2.6.2"};
 for(unsigned i=0;i<sizeof(models)/sizeof(models[0]);i++) {
  unsigned cycles=i?512:474;
  model(models[i]);
  mdec_write(0x1f801820,0x38000002);mdec_source_advance(1);
  mdec_dma_write_word(0xfe000000);
  assert(source_mdec.phase==SMDEC_BLOCK_WAIT);
  mdec_source_advance(cycles-1);assert(source_mdec.phase==SMDEC_BLOCK_WAIT);
  unsigned n=mdec_snapshot_bytes();uint8_t *wire=malloc(n);mdec_snapshot_write(wire);
  model(models[i]);
  assert(mdec_snapshot_read(wire,n));
  mdec_source_advance(1);assert(source_mdec.phase==SMDEC_INPUT);
  if(cycles==512) {
   /* The final bytes are signed chroma history, not reserved flags. The
    * error word is immediately before those two arrays in the real codec. */
   unsigned error_at=n-sizeof(source_cr)-sizeof(source_cb)-sizeof(int32_t);
   uint8_t *before=malloc(n),*after=malloc(n);assert(before&&after);
   mdec_snapshot_write(before);
   wire[error_at]=1;assert(!mdec_snapshot_read(wire,n));wire[error_at]=0;
   mdec_snapshot_write(after);assert(!memcmp(before,after,n));
   free(before);free(after);
   wire[n-1]=1;assert(mdec_snapshot_read(wire,n));wire[n-1]=0;
   model("octoshock-2.3");assert(!mdec_snapshot_read(wire,n));
  }
  free(wire);
 }
 return 0;
}
