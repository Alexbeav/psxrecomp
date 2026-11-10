/* Configured source DSM state refuses malformed or mismatched context before
 * changing public channels. Accepted source spec transport supplies devices. */
#define main accepted_source_spec_main
#include "test_dma_source_spec2.c"
#undef main
int main(void){
 set_option("PSX_INPUT_ROUTE_FILE","authored-fixture");
 set_option("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-quad");
 set_option("PSX_CD_DMA_MODEL","octoshock-2.2.2");
 set_option("PSX_INPUT_ROUTE_DMA_MODEL","octoshock-2.2.2-otc");
 fresh(0x0fedcba9);gpu_ready=0;kick(2,0x1000,0x40020,0x11000201);
 uint32_t n=dma_snapshot_bytes();assert(n==DMA_SNAP_WIRE_BYTES+SDB_TAIL_BYTES);
 uint8_t *saved=malloc(n),*bad=malloc(n),*after=malloc(n);assert(saved&&bad&&after);dma_snapshot_write(saved);
 const unsigned offsets[]={0,4,8,12,13,14,15,16,20,24,40,44,48};unsigned checks=0;
 for(unsigned i=0;i<sizeof offsets/sizeof *offsets;++i){
  memcpy(bad,saved,n);unsigned p=DMA_SNAP_WIRE_BYTES+offsets[i];
  if(offsets[i]>=16 && offsets[i]!=44){memset(bad+p,0xff,4);bad[p+3]=0x7f;}else bad[p]=0xff;
  assert(!dma_snapshot_validate(bad,n));assert(!dma_snapshot_read(bad,n));dma_snapshot_write(after);assert(!memcmp(saved,after,n));checks+=3;
 }
 assert(!dma_snapshot_read(saved,n-1));assert(dma_snapshot_read(saved,n));checks+=2;
 /* Advance all shared service machines, then rewind their complete context. */
 psx_cycle_count=128;dma_advance(128);psx_cycle_count=0;assert(dma_snapshot_read(saved,n));dma_snapshot_write(after);assert(!memcmp(saved,after,n));checks+=2;
 free(saved);free(bad);free(after);printf("source DMA context: %u malformed/config/rewind checks PASS\n",checks);return 0;
}
