/* Actual production scheduler + DMA controller. Other devices expose a single
 * optional timer boundary. No game, BIOS, MMIO payload model or timing tune. */
#define PSX_TEST_REAL_CYCLE_SCHEDULER
#define PSX_TEST_SOURCE_CD_IMPLEMENTED
#define main cd_fixture_baseline_main
#include "test_dma_cd_source.c"
#undef main
#include "../src/psx_cycles.c"

static uint64_t timer_due, timer_fired_cycle, dma_irq_cycle;
static unsigned timer_events;
uint32_t i_mask;
uint32_t interrupts_cycles_to_vblank(void) { return 1000000; }
uint32_t timers_cycles_to_irq(uint32_t mask) {
    (void)mask;
    return timer_due && !timer_events ? (uint32_t)(timer_due-psx_cycle_count) : 1000000;
}
uint32_t cdrom_cycles_to_irq(uint32_t mask) { (void)mask; return 1000000; }
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return 1000000; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return 1000000; }
void psx_spu_sample_event_service(void) {}
void interrupts_service_scheduled_events(void) {}
void sio_advance(uint32_t cycles) { (void)cycles; }
void cdrom_advance(uint32_t cycles) { (void)cycles; }
void timers_advance(uint32_t cycles) {
    (void)cycles;
    if (timer_due && !timer_events && psx_cycle_count>=timer_due) {
        timer_events++;timer_fired_cycle=psx_cycle_count;
    }
    if (irqs && !dma_irq_cycle) dma_irq_cycle=psx_cycle_count;
}
void interrupts_advance_cycles(uint32_t cycles) { (void)cycles; }
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
int psx_netplay_active(void) { return 0; }
int psx_selfcheck_enabled(void) { return 0; }
int psx_get_in_exception(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}
uint64_t g_guest_store_count, g_mmio_access_count;
int g_ls_mode, g_precise_mode, g_psx_call_bail;

static int run_case(uint64_t start, uint32_t future, int chopped) {
    setup(512,start);
    s_devices_synced_cycle=start;s_next_watchdog=s_next_pc_sample=UINT64_MAX;
    psx_in_device_service=0;g_psx_cycle_fast_limit=0;g_psx_cyc_batch=0;
    timer_due=future?start+future:0;timer_events=0;timer_fired_cycle=dma_irq_cycle=0;
    channels[3].madr=0xB070;channels[3].bcr=0x00010200;
    channels[3].chcr=0;
    /* Same barrier order as the production memory MMIO wrapper. */
    psx_devices_mmio_sync();
    uint64_t cached=psx_next_service_cycle;
    dma_write(0x1f8010b8u,chopped?0x11000100u:0x11000000u);
    uint64_t returned=psx_cycle_count;
    psx_devices_mmio_sync();
    uint64_t expected=((start+4536u+127u)/128u)*128u;
    if(chopped) {
        if(returned!=start || writes!=8 || irqs) return 1;
        psx_advance_cycles((uint32_t)(expected-start));
        if(writes!=512 || irqs!=1) return 1;
    } else if(returned!=expected || writes!=512 || irqs!=1) {
        fprintf(stderr,"cached deadline: start=%llu cache=%llu returned=%llu expected=%llu words=%u IRQ=%u\n",
                (unsigned long long)start,(unsigned long long)cached,(unsigned long long)returned,
                (unsigned long long)expected,writes,irqs);
        return 1;
    }
    if(dma_irq_cycle!=expected) {
        fprintf(stderr,"DMA IRQ changed: %llu expected %llu\n",(unsigned long long)dma_irq_cycle,(unsigned long long)expected);return 1;
    }
    if(future && timer_due<=expected) {
        if(timer_events!=1 || timer_fired_cycle!=timer_due)return 1;
    } else if(timer_events)return 1;
    if(ram[0xB070/4]!=0xCA000000u || ram[(0xB070+2044)/4]!=0xCA0001FFu)return 1;
    return 0;
}
int main(void) {
    set_option("PSX_INPUT_ROUTE_FILE","authored-fixture");
    set_option("PSX_CD_DMA_MODEL","octoshock-2.2.2");
    /* Actual observed CHCR timestamp and quiet 16K scheduler cache. */
    if(run_case(407914091u,0,0))return 1;
    for(uint32_t phase=0;phase<128;phase++) {
        if(run_case(phase,0,0) || run_case(phase,2000,0) ||
           run_case(phase,9000,0) || run_case(phase,9000,1))return 1;
    }
    puts("PASS production CD DMA scheduler: observed trigger plus 512 cache/phase/order controls");
    return 0;
}

/* No source GPU consumer in this device-isolation control. */
void source_gpu_runtime_advance(void) {}
uint32_t source_gpu_runtime_cycles_to_event(void) {return UINT32_MAX;}
