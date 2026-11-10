/* Exercise the actual production data-load charge, without a BIOS or game. */
#include "cpu_state.h"
#include "psx_cycles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dma.h"
#include <assert.h>
uint64_t psx_cycle_count,psx_next_service_cycle;
int psx_in_device_service,g_event_step_conservative;
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit;
int g_ls_replay_active;
static uint32_t penalty,queries,complete_on_charge;
uint32_t dma_cpu_read_penalty(void) {queries++;return penalty;}
void psx_devices_service_to_now(void) {if(complete_on_charge)penalty=0;}
void psx_advance_cycles_slow(uint32_t n) {psx_cycle_count+=n;if(complete_on_charge)penalty=0;}
#include PSX_TEST_MEMORY_READ_INCLUDE
int main(void) {
    CPUState cpu;
    for(uint32_t p=0;p<=200;p+=5) {
        penalty=p;memset(&cpu,0,sizeof(cpu));cpu.read_fudge=0x20;
        psx_cycle_count=queries=0;
        psx_cyc_readmem(&cpu,0x10000,4,2,3);
        assert(psx_cycle_count==7+p && cpu.ld_absorb==5+p && queries==1);
        memset(&cpu,0,sizeof(cpu));psx_cycle_count=queries=0;
        psx_cyc_readmem(&cpu,0x1f801814,4,2,3);
        assert(psx_cycle_count==3+p && cpu.ld_absorb==3+p && queries==1);
        memset(&cpu,0,sizeof(cpu));cpu.read_fudge=0x20;
        psx_cycle_count=queries=0;
        psx_cyc_readmem(&cpu,0x1f800004,4,2,3);
        assert(psx_cycle_count==0 && cpu.ld_absorb==0 && queries==0);
    }
    /* Completion while this load dispatches events must not erase the penalty
     * sampled when the load started. The next load observes the cleared state. */
    memset(&cpu,0,sizeof(cpu));cpu.read_fudge=0x20;
    penalty=15;complete_on_charge=1;psx_cycle_count=queries=0;psx_next_service_cycle=11;
    psx_cyc_readmem(&cpu,0x10000,4,2,3);
    assert(psx_cycle_count==22 && cpu.ld_absorb==20 && penalty==0 && queries==1);
    puts("PASS production load wait: RAM, MMIO, absorbed cycles, prior-load rule and scratchpad exclusion");return 0;
}
