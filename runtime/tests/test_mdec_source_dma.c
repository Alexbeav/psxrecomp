/* Authored production-controller fixture. No BIOS, disc or retail code. */
#define _POSIX_C_SOURCE 200809L
#include "dma.c"
#include "mdec.c"
#include <assert.h>
uint64_t s_frame_count,psx_cycle_count,psx_next_service_cycle;
int psx_in_device_service,g_event_step_conservative,g_ls_replay_active;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
uint32_t i_stat,g_debug_current_func_addr,g_debug_last_store_pc;
static uint32_t ram[0x80000],writes,irqs,read_words,available;
static uint32_t upload_left, uploaded[65536], upload_count;
static void set_option(const char *key,const char *value) {
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
}
void psx_devices_service_to_now(void) {
#ifdef PSX_TEST_SOURCE_GPU_IMPLEMENTED
    advance_source_gpu();
#ifdef PSX_TEST_SOURCE_LL_IMPLEMENTED
    advance_source_gpu_ll();
#endif
#endif
}
void psx_advance_cycles_slow(uint32_t n) {psx_cycle_count+=n;psx_devices_service_to_now();}
void psx_write_word(uint32_t addr,uint32_t value) {ram[(addr&0x1ffffc)/4]=value;writes++;}
void psx_irq_raise(uint32_t bit,uint32_t detail) {(void)detail;i_stat|=1u<<bit;irqs++;}
void event_ring_record_aux(uint16_t kind,uint8_t src,uint32_t value) {(void)kind;(void)src;(void)value;}
int cdrom_dma_ready(void) {return read_words<available;}
uint32_t cdrom_dma_sector_word_count(void) {return available;}
uint32_t cdrom_dma_read(void) {assert(cdrom_dma_ready());return 0xCA000000u+read_words++;}
uint32_t cdrom_dma_read_padded(void) {return cdrom_dma_ready()?cdrom_dma_read():0;}
void cdrom_debug_snapshot(CDROMDebugState *s) {memset(s,0,sizeof(*s));s->last_sector_lba=4;s->sector_size=2048;s->sector_available=1;}
int cdrom_get_setloc_lba(void) {return 4;}
uint8_t *memory_get_ram_ptr(void) {return (uint8_t*)ram;}
void overlay_capture_before_dma(uint32_t a,uint32_t n) {(void)a;(void)n;}
void overlay_capture_on_dma(uint32_t a,uint32_t n,const uint8_t *p) {(void)a;(void)n;(void)p;}
void dirty_ram_mark_executable_range(uint32_t a,uint32_t n) {(void)a;(void)n;}
/* Unrelated device entry points must never execute in this fixture. */
uint64_t g_io_openbus_reads,g_io_openbus_writes;
uint32_t psx_read_word(uint32_t a) {return ram[(a&0x1ffffc)/4];}
void psx_fatal_halt(const char *s) {fprintf(stderr,"%s\n",s);abort();}
void gpu_set_gp0_linked_list_node(uint32_t a,uint32_t b) {(void)a;(void)b;}
uint32_t gpu_read_gpuread(void) {abort();}
void gpu_set_gp0_source(uint32_t a) {(void)a;}
static int ready_state=1;
int gpu_dma_source_ll_ready(void) {return ready_state;}
void gpu_write_gp0(uint32_t v) {uploaded[upload_count++]=v;}
uint32_t gpu_dma_vram_upload_words(void) {return upload_left;}
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) {(void)a;}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) {return a;}
void spu_dma_write(uint32_t v) {(void)v;abort();}
uint32_t spu_dma_read(void) {abort();}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;abort();}


int debug_server_fmv_quiet(void){return 0;}
int source_gpu_runtime_active(void){return 1;}
int source_gpu_runtime_ready(void){return 1;}
uint32_t source_gpu_runtime_cycles_to_event(void){return 128-(uint32_t)(psx_cycle_count%128);}
void source_gpu_runtime_dma_write(void){dma_source_gpu_service_at(psx_cycle_count);}
void source_gpu_runtime_copy(SourceGPUServiceClock *c,SourceGPUCommandProjection *s){(void)c;(void)s;abort();}
int main(int argc,char **argv){
 if(argc!=3)return 2;FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wbx");if(!in || !out)return 2;
 set_option("PSX_MDEC_SOURCE_MODEL","octoshock-2.3");
 set_option("PSX_INPUT_ROUTE_FILE","authored-fixture");
 set_option("PSX_GPU_DMA_MODEL","octoshock-2.2.2-bounded-quad");
 uint32_t op,time,addr,value;
 while(fread(&op,4,1,in)==1){
  if(fread(&time,4,1,in)!=1 || fread(&addr,4,1,in)!=1 || fread(&value,4,1,in)!=1)abort();
  uint32_t result=0,aux=0;psx_cycle_count=time;
  switch(op){
   case 0:dma_init();mdec_init();memset(ram,0,sizeof(ram));i_stat=irqs=0;break;
   case 1:dma_source_gpu_service_at(time);break;
   case 2:dma_write(addr,value);break;
   case 3:result=dma_read(addr);break;
   case 4:psx_write_word(addr,value);break;
   case 5:result=psx_read_word(addr);break;
   case 6:i_stat&=value;break;
   case 7:result=i_stat;break;
   case 8:mdec_write(0x1f801820,value);break;
   case 9:mdec_write(0x1f801824,value);break;
   case 10:result=mdec_read(0x1f801820);break;
   case 11:mdec_snapshot_bytes();break;
   case 12:dma_snapshot_write(NULL);break;
   default:abort();
  }
  SourceMDEC *s=&source_mdec;
  uint32_t row[]={result,aux,mdec_read(0x1f801824),(uint32_t)s->credit,s->command,s->control,s->remaining,s->in_count,s->out_count,
   s->coefficient,s->block,s->pixel_count,s->pixel_at,s->row,s->word_in_row,s->row_words,s->busy,s->quant_index,s->matrix_index,
   dma_read(0x1f801080),dma_read(0x1f801084),dma_read(0x1f801088),dma_read(0x1f801090),dma_read(0x1f801094),dma_read(0x1f801098),dma_get_dicr(),i_stat&8u};
  if(fwrite(row,sizeof(row),1,out)!=1)abort();
 }
 if(ferror(in) || fclose(in) || fclose(out))return 4;return 0;
}
