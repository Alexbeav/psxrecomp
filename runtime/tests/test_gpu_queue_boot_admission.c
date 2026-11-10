/* Full-loader admission fixture, real GPU/DMA and boot-state owners. */
#define GPU_QUEUE_BOOT_ADMISSION
#include "test_dma_gpu_command_queue.c"
#include "../src/boot_state.c"
static uint8_t spad[1024],spuram[16],memctrl[72];
static unsigned mutations;
uint32_t memory_control_snapshot_bytes(void) { return sizeof memctrl; }
void memory_control_snapshot_write(uint8_t *p) { memcpy(p,memctrl,sizeof memctrl); }
int memory_control_snapshot_read(const uint8_t *p,uint32_t n) {
 if(!p || n!=sizeof memctrl)return 0;memcpy(memctrl,p,n);mutations++;return 1;
}
uint8_t *memory_get_scratchpad_ptr(void) { return spad; }
uint8_t *spu_get_ram_ptr(void) { return spuram; }
uint32_t spu_get_ram_bytes(void) { return sizeof spuram; }
#define SNAP_SEAM(n) uint32_t n##_snapshot_bytes(void){return 4;} \
 int n##_snapshot_validate(const uint8_t*p,uint32_t n){(void)p;return n==4;} \
 int n##_snapshot_read(const uint8_t*p,uint32_t n){(void)p;(void)n;mutations++;return 1;}
SNAP_SEAM(spu)
SNAP_SEAM(cdrom)
SNAP_SEAM(sio)
int sio_snapshot_shape_ok(uint32_t n){return n==4;}
int mdec_snapshot_prepare(const uint8_t*p,uint32_t n){(void)p;return n==4;}
int mdec_snapshot_read(const uint8_t*p,uint32_t n){(void)p;(void)n;mutations++;return 1;}
int dirty_ram_checkpoint_validate(const uint8_t*p,uint32_t n){(void)p;return n==DIRTY_RAM_CHECKPOINT_BYTES;}
int dirty_ram_checkpoint_read(const uint8_t*p,uint32_t n){(void)p;(void)n;mutations++;return 1;}
uint32_t dirty_ram_get_bitmap_word_count(void){return 1;}
void dirty_ram_set_bitmap_words(const uint32_t*p,uint32_t n){(void)p;(void)n;mutations++;}
int psx_scheduler_snapshot_validate(const uint8_t*p,uint32_t n,const uint8_t*r){(void)p;(void)r;return n==PSX_SCHEDULER_SNAPSHOT_BYTES;}
int psx_scheduler_snapshot_read(const uint8_t*p,uint32_t n,CPUState*c){(void)p;(void)n;(void)c;mutations++;return 1;}
uint32_t psx_mod_memory_snapshot_bytes(void){return 0;}
uint32_t psx_mod_memory_layout_cookie(void){return 0;}
int psx_mod_memory_snapshot_validate(const uint8_t*p,uint32_t n){(void)p;(void)n;abort();}
int psx_mod_memory_snapshot_read(const uint8_t*p,uint32_t n){(void)p;(void)n;abort();}
int interrupts_raster_comparison_active(void){return 0;}
int timers_source_active(void){return 0;}
void source_gpu_runtime_rederive_returns(void){mutations++;}
void interrupts_note_state_load(int n){(void)n;mutations++;}
void overlay_watch_invalidate_after_ram_restore(void){mutations++;}
void fntrace_restore_game_started(int n){(void)n;mutations++;}
void interrupts_set_cycles_since_vblank(uint32_t n){(void)n;mutations++;}
int gpu_vram_dirty_tracking(void){return 0;}
void gpu_vram_dirty_clear(void){mutations++;}
void gte_precision_timeline_invalidate(void){mutations++;}
void psx_kernel_bless_note_range(uint32_t a,uint32_t n){(void)a;(void)n;mutations++;}
int interrupts_timing_wire_read(const uint8_t*p,uint32_t n){(void)p;(void)n;mutations++;return 1;}
#define INACTIVE_READER(f) int f(const uint8_t*p,uint32_t n){(void)p;(void)n;abort();}
INACTIVE_READER(interrupts_raster_wire_read)
INACTIVE_READER(source_gpu_raster_wire_read)
INACTIVE_READER(timers_source_wire_read)
INACTIVE_READER(source_gpu_service_wire_read)
void timers_set_snapshot(const uint16_t a[3],const uint32_t b[3],const uint16_t c[3],const int32_t d[3],const uint32_t e[3]){(void)a;(void)b;(void)c;(void)d;(void)e;mutations++;}
#include "../src/psx_icache.c"
int main(void) {
 reset_gpu_state_for_test(); dma_init(); psx_icache_attach_ram((uint8_t*)test_ram,sizeof(test_ram)); psx_icache_reset();
 gpu_queue.credit=SOURCE_GPU_T_CREDIT_LIMIT; /* valid inclusive bound */
 BsOut o={0}; o.no_zlib=1;
 BootStateHeader h={0}; h.magic=BOOT_STATE_MAGIC;h.version=BOOT_STATE_VERSION_WITHOUT_HOST;
 h.codegen_hash=PSX_OVERLAY_CODEGEN_HASH;h.abi_tag=PSX_OVERLAY_ABI_TAG;h.codegen_ver=PSX_OVERLAY_CODEGEN_VER;
 h.reserved=boot_state_layout_cookie();
 const unsigned tags[]={BS_SEC_CPU,BS_SEC_CPU_EXEC,BS_SEC_RAM,BS_SEC_SCHED,BS_SEC_GAME_START,BS_SEC_MEMCTRL,BS_SEC_SPAD,BS_SEC_IRQ,BS_SEC_TIMER,BS_SEC_CLOCK,BS_SEC_GPU,BS_SEC_VRAM,BS_SEC_SPU,BS_SEC_SPURAM,BS_SEC_CDROM,BS_SEC_DMA,BS_SEC_SIO,BS_SEC_MDEC,BS_SEC_ICACHE,BS_SEC_DIRTY,BS_SEC_IRQ_TIMING,BS_SEC_SPUCD};
 h.section_count=sizeof(tags)/sizeof(tags[0]); assert(write_header_le(&o,&h));
 CPUState saved={0},live={0};saved.pc=0x1234;live.pc=0x5678;
 size_t gpu_at=0,icache_at=0;
 for(unsigned i=0;i<h.section_count;i++) {
  unsigned tag=tags[i],n=4;
  switch(tag){
  case BS_SEC_CPU:n=CPU_REGS_WIRE_BYTES;break;case BS_SEC_CPU_EXEC:n=DIRTY_RAM_CHECKPOINT_BYTES;break;
  case BS_SEC_RAM:n=sizeof(test_ram);break;case BS_SEC_SCHED:n=PSX_SCHEDULER_SNAPSHOT_BYTES;break;
  case BS_SEC_MEMCTRL:n=sizeof memctrl;break;case BS_SEC_SPAD:n=SPAD_SIZE;break;case BS_SEC_IRQ:case BS_SEC_CLOCK:n=8;break;
  case BS_SEC_TIMER:n=48;break;case BS_SEC_GPU:n=gpu_snapshot_bytes();break;
  case BS_SEC_VRAM:n=VRAM_SIZE;break;case BS_SEC_SPURAM:n=sizeof spuram;break;
  case BS_SEC_DMA:n=dma_snapshot_bytes();break;case BS_SEC_ICACHE:n=PSX_ICACHE_SNAPSHOT_WIRE_BYTES;break;case BS_SEC_IRQ_TIMING:n=64;break;
  }
  uint8_t *p=calloc(1,n);
  if(tag==BS_SEC_CPU) assert(cpu_state_wire_write(p,&saved));
  if(tag==BS_SEC_GPU){gpu_snapshot_write(p);gpu_at=o.len+16;}
  if(tag==BS_SEC_DMA)dma_snapshot_write(p);
  if(tag==BS_SEC_ICACHE){psx_icache_snapshot_write(p);icache_at=o.len+16;}
  assert(write_section_raw(&o,tag,0,p,n));free(p);
 }
 /* A valid full stream first proves this fixture reaches and applies commit. */
 assert(boot_state_load_buffer(o.data,o.len,0,0,&live));assert(live.pc==saved.pc);
 uint8_t *valid_stream=malloc(o.len);memcpy(valid_stream,o.data,o.len);
 CPUState before={0};before.pc=0x5678;live=before;
 test_ram[0]=0xaabbccdd;psx_cycle_count=123;mutations=0;
 uint8_t *ram_before=malloc(sizeof(test_ram));memcpy(ram_before,test_ram,sizeof(test_ram));
 uint32_t gn=gpu_snapshot_bytes();uint8_t *gpu_before=malloc(gn),*gpu_after=malloc(gn);
 uint32_t queue_at=gn-gpu_source_tail_bytes()+200u;
 gpu_snapshot_write(gpu_before);
 for(unsigned bad=0;bad<3;bad++) {
 o.data[4]=BOOT_STATE_VERSION_WITHOUT_HOST;
 memset(o.data+gpu_at+queue_at,0,8);
 if(bad==1){o.data[gpu_at+queue_at+4]=1;o.data[gpu_at+queue_at+5]=1;}
 if(bad==2)o.data[4]=14;
 if(bad==0)o.data[gpu_at+queue_at]=17;
 int ok=boot_state_load_buffer(o.data,o.len,0,0,&live);
 printf("admission=%d pc=%x ram=%x cycle=%llu mutations=%u\n",ok,live.pc,test_ram[0],(unsigned long long)psx_cycle_count,mutations);fflush(stdout);
 gpu_snapshot_write(gpu_after);
 assert(!ok && !memcmp(&live,&before,sizeof live) && !memcmp(test_ram,ram_before,sizeof(test_ram)));
 assert(!memcmp(gpu_before,gpu_after,gn) && psx_cycle_count==123 && !mutations);
 }
 memcpy(o.data,valid_stream,o.len);o.data[icache_at+8192]=2;
 uint8_t cache_before[PSX_ICACHE_SNAPSHOT_WIRE_BYTES],cache_after[PSX_ICACHE_SNAPSHOT_WIRE_BYTES];
 psx_icache_snapshot_write(cache_before);
 assert(!boot_state_load_buffer(o.data,o.len,0,0,&live));
 psx_icache_snapshot_write(cache_after);gpu_snapshot_write(gpu_after);
 assert(!memcmp(cache_before,cache_after,sizeof cache_before));
 assert(!memcmp(&live,&before,sizeof live) && !memcmp(test_ram,ram_before,sizeof(test_ram)));
 assert(!memcmp(gpu_before,gpu_after,gn) && psx_cycle_count==123 && !mutations);
 free(valid_stream);free(ram_before);free(gpu_before);free(gpu_after);
 free(o.data);puts("PASS full GPU admission rejects before any mutation");return 0;
}

uint64_t g_psx_device_gen;
uint32_t memory_get_ram_bytes(void) {return sizeof test_ram;}
int psx_ram_8mb_active(void) {return 0;}
int mdec_snapshot_validate(const uint8_t *p,uint32_t n) {return p&&n==4;}
int spu_cd_snapshot_validate(const uint8_t *p,uint32_t n) {return p&&n==4;}
int spu_cd_snapshot_read(const uint8_t *p,uint32_t n) {if(!spu_cd_snapshot_validate(p,n))return 0;mutations++;return 1;}
int mod_runtime_snapshot_package_active(const char *id) {(void)id;return 0;}

/* Report-only accounting is outside this fixture's hardware seams. */
void dirty_ram_pc_table_game_load(void) {}
int fntrace_is_game_started(void) {return 0;}
