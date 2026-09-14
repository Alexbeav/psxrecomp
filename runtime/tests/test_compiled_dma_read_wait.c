#include "psx_cyc.h"
#include <assert.h>
#include <stdio.h>
uint64_t psx_cycle_count,psx_next_service_cycle=1000000;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit,g_dma_cpu_read_wait; int g_psx_cyc_bb_defer;
uint32_t *g_psx_cyc_local_acc;
int psx_in_device_service,g_event_step_conservative,g_ls_replay_active,g_ls_mode;
volatile int g_ds_recording;
static uint8_t ram[0x200000];uint8_t *g_psx_ram=ram;
int g_psx_load_delay=1,g_ram_read_watch_active;
static unsigned slow_words,slow_halves;
void psx_devices_service_to_now(void) {assert(0);}
void psx_advance_cycles_slow(uint32_t n) {psx_cycle_count+=n;}
int psx_load_delay_enabled(void) {return 1;}
void debug_server_trace_ram_read_watch(uint32_t a,uint32_t v) {(void)a;(void)v;assert(0);}
uint32_t psx_cyc_load_word_slow(CPUState *cpu,uint32_t a,uint32_t r,uint32_t m) {
 (void)cpu;(void)r;(void)m;assert((a&0x1fffff)==0x1000);slow_words++;return 0x12345678;
}
uint16_t psx_cyc_load_half_slow(CPUState *cpu,uint32_t a,uint32_t r,uint32_t m) {
 (void)cpu;(void)r;(void)m;assert((a&0x1fffff)==0x1000);slow_halves++;return 0x5678;
}
int main(void) {
 CPUState cpu={0};cpu.ld_which_t=32;
 uint32_t v=0x12345678;memcpy(ram+0x1000,&v,4);
 assert(psx_cyc_load_word(&cpu,0x80001000,2,0)==v && slow_words==0);
 const uint32_t aliases[]={0x00001000,0x80001000,0xa0001000,0x80201000};
 g_dma_cpu_read_wait=15;
 for(unsigned i=0;i<4;i++) {
  assert(psx_cyc_load_word(&cpu,aliases[i],2,0)==v);
  assert(psx_cyc_load_half(&cpu,aliases[i],2,0)==0x5678);
 }
 if(slow_words!=4 || slow_halves!=4) {puts("FAIL compiled RAM path bypasses active DMA load wait");return 1;}
 g_dma_cpu_read_wait=0;assert(psx_cyc_load_word(&cpu,0x80001000,2,0)==v && slow_words==4);
 puts("PASS compiled word/half active-DMA routing, aliases, and unchanged inactive fast path");return 0;
}
