#ifdef NDEBUG
#undef NDEBUG
#endif
/* Reuse unrelated-device stubs, but run the production DMA and cycle clock. */
#define PSX_TEST_GPU_COMMAND_QUEUE_STUBS_H
#define main adjacent_completion_fixture_main
#include "test_dma_completion_deadline.c"
#undef main
static uint64_t ready_at;
int gpu_command_queue_dma_ready(void) {return psx_cycle_count>=ready_at;}
int gpu_command_queue_accept_word(void) {return psx_cycle_count>=ready_at;}
void gpu_command_queue_advance(void) {}
uint32_t gpu_command_queue_cycles_to_event(void) {
    return psx_cycle_count>=ready_at ? UINT32_MAX : (uint32_t)(ready_at-psx_cycle_count);
}
void gpu_ws_restore_linked_list_rank(uint32_t rank) {(void)rank;}
void gpu_ws_validate_linked_list_header(uint32_t a,uint32_t h) {(void)a;(void)h;}
void gpu_ws_validate_linked_list_node(uint32_t a,uint32_t n) {(void)a;(void)n;}
void source_gpu_runtime_copy(SourceGPUServiceClock *c,SourceGPUCommandProjection *p) {(void)c;(void)p;abort();}
static void reset_bus(void) {
    set_option("PSX_GPU_DMA_MODEL","");set_option("PSX_CD_DMA_MODEL","");set_option("PSX_DMA_MODEL","");
    dma_init();memset(ram,0,sizeof(ram));gpu_words=gpu_headers=irqs=i_stat=0;
    psx_cycle_count=s_devices_synced_cycle=ready_at=0;
    psx_next_service_cycle=0;psx_in_device_service=0;
    g_psx_cycle_fast_limit=g_psx_cyc_batch=g_psx_cyc_batch_limit=0;g_psx_cyc_local_acc=NULL;
    s_next_watchdog=s_next_pc_sample=UINT64_MAX;
    timer_due=3;timer_events=0;timer_fired_cycle=0;
    i_mask=8;dpcr|=8u<<8;dicr=(1u<<23)|(1u<<18);
    channels[2].madr=0x1000;
}
int main(void) {
    reset_bus();ram[0x1000/4]=0x03002000;ram[0x2000/4]=0x01ffffff;
    dma_write(0x1f8010a8,0x01000401);
    psx_advance_cycles(2);psx_devices_service_to_now();
    assert(gpu_words==1 && gpu_linked_list.phase==DMA_GPU_LL_PHASE_PAYLOAD);
    /* FIFO space disappears mid-packet. Stop still finishes this packet and
     * exposes the next header, never half a GPU primitive or a whole-list drain. */
    ready_at=10;
    dma_write(0x1f8010a8,0);
    assert(gpu_words==3 && channels[2].madr==0x2000 && !gpu_linked_list.active && !irqs);
    assert(timer_events==1 && timer_fired_cycle==3);
    ram[0x2000/4]=0x02ffffff; /* later header remains live after the stop */
    dma_write(0x1f8010a8,0x01000401);
    psx_advance_cycles(3);psx_devices_service_to_now();
    assert(gpu_words==5 && irqs==1 && !gpu_linked_list.active);
    psx_advance_cycles(20);psx_devices_service_to_now();assert(irqs==1);
    reset_bus();ram[0x1000/4]=0x1000;
    dma_write(0x1f8010a8,0x01000401);
    psx_advance_cycles(4);psx_devices_service_to_now();
    assert(gpu_linked_list.active && gpu_linked_list.nodes_processed==4);
    dma_write(0x1f8010a8,0);assert(!gpu_linked_list.active && !irqs);
    puts("PASS GPU backpressure: stop/resume, live later header, timer, one IRQ, cyclic-list cancellation");
    return 0;
}
