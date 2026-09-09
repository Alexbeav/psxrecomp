#include "psx_cyc.h"
#include <assert.h>
#include <stdio.h>
uint64_t psx_cycle_count,psx_next_service_cycle=1000000;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit; int g_psx_cyc_bb_defer;
uint32_t g_dma_cpu_read_wait;
uint32_t *g_psx_cyc_local_acc;
int psx_in_device_service,g_event_step_conservative,g_ls_replay_active,g_ls_mode;
volatile int g_ds_recording;
static uint8_t ram[0x200000]; uint8_t *g_psx_ram=ram;
int g_psx_load_delay=1,g_ram_read_watch_active;
static uint32_t observed,count;
void psx_devices_service_to_now(void) { assert(0); }
void psx_advance_cycles_slow(uint32_t n) { psx_cycle_count+=n; }
int psx_load_delay_enabled(void) { return 1; }
uint32_t psx_cyc_load_word_slow(CPUState *cpu,uint32_t a,uint32_t r,uint32_t m) {
 (void)cpu;(void)a;(void)r;(void)m;assert(0);return 0;
}
uint16_t psx_cyc_load_half_slow(CPUState *cpu,uint32_t a,uint32_t r,uint32_t m) {
 (void)cpu;(void)a;(void)r;(void)m;assert(0);return 0;
}
void debug_server_trace_ram_read_watch(uint32_t a,uint32_t v) {
 assert(a==0x1000);observed=v;count++;
}
int main(void) {
 CPUState off={0},on={0};off.ld_which_t=on.ld_which_t=32;
 uint32_t expected=0x12345678; memcpy(ram+0x1000,&expected,4);
 assert(psx_cyc_load_word(&off,0x80001000,2,0)==expected);assert(count==0);
 uint64_t cycles=psx_cycle_count;uint32_t deferred=g_psx_cyc_batch;
 psx_cycle_count=0;g_psx_cyc_batch=0;g_psx_cyc_batch_limit=0;
 g_ram_read_watch_active=1;
 assert(psx_cyc_load_word(&on,0x80201000,2,0)==expected);
 if(count!=1||observed!=expected){puts("FAIL: compiled RAM watch omitted");return 1;}
 assert(!memcmp(&off,&on,sizeof(off)));assert(cycles==psx_cycle_count&&deferred==g_psx_cyc_batch);
 assert(psx_cyc_load_half(&on,0x80001000,2,0)==0x5678);
 assert(count==2&&observed==0x5678);
 puts("compiled RAM word read watch: value, mirror, disabled watch and timing parity PASS");
}
