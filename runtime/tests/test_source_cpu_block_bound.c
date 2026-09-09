#include "psx_icache.h"
#include "source_cpu_block_bound.h"
#include <assert.h>
#include <stdio.h>
uint64_t psx_cycle_count,psx_next_service_cycle=1000000;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
int g_psx_cyc_bb_defer,psx_in_device_service,g_event_step_conservative;
int g_ls_replay_active;
uint32_t *g_psx_cyc_local_acc;
uint64_t psx_get_cycle_count(void){return psx_cycle_count+g_psx_cyc_batch+(g_psx_cyc_local_acc?*g_psx_cyc_local_acc:0);}
static uint8_t ram[0x200000];uint8_t *g_psx_ram=ram;
void psx_devices_service_to_now(void){assert(0);}
void psx_advance_cycles_slow(uint32_t n){psx_cycle_count+=n;}
uint32_t psx_read_word(uint32_t addr){assert(addr==0xfffe0130);return 0x800;}
uint32_t psx_gte_cmd_latency(uint32_t cmd){(void)cmd;assert(0);return 0;}
int main(void) {
    /* Independently execute a five-instruction ALU/branch-slot timing path.
     * Entry at offset8 crosses a cold cache line. Seven clocks to a future
     * IRQ is beyond the old count5 but lies inside the real block17. */
    const uint32_t code[]={0x00a63823,0x000212c3,0x3046001f,0x14c00002,0};
    memcpy(ram+0x508,code,sizeof(code));
    for(unsigned warm=0;warm<2;warm++) {
        CPUState cpu={0};cpu.ld_which_t=32;cpu.read_fudge=32;
        psx_cycle_count=0;psx_icache_reset();g_psx_icache_active=1;
        if(warm)for(unsigned i=0;i<8;i++)g_psx_icache_tv[(0x500>>2)+i]=0x80000500+4*i;
        uint64_t before=psx_get_cycle_count();
        uint32_t bound=source_cpu_block_bound(&cpu,0x80000508,5,7);
        assert(psx_get_cycle_count()==before);
        for(unsigned i=0;i<5;i++){psx_icache_fetch_interp(&cpu,0x80000508+4*i);psx_cyc_step(&cpu,0);}
        psx_cyc_batch_flush();
        assert(psx_get_cycle_count()==(warm?5u:17u));
        assert(bound>=psx_get_cycle_count());
        assert(warm || (5u<7u && 7u<=bound && 7u<psx_get_cycle_count()));
        printf("PASS %s: actual=%llu upper=%u; selection adds no cycles\n",warm?"warm":"cold",(unsigned long long)psx_get_cycle_count(),bound);
    }
}
