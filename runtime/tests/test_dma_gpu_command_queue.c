/* Authored production DMA + production GPU, no BIOS or disc. */
#define GPU_QUEUE_NO_MAIN
#define GPU_QUEUE_HOST_CLOCK 1
#define GPU_QUEUE_REAL_CLOCK
#include "test_gpu_command_queue.c"
#undef main
#include "../src/dma.c"
uint32_t i_stat;
int g_ls_replay_active;
uint64_t g_io_openbus_reads,g_io_openbus_writes;
static uint32_t spu_in,spu_out,cd_reads;
void psx_write_word(uint32_t a,uint32_t v) { test_ram[(a&0x1ffffc)/4]=v; }
int cdrom_dma_ready(void) { return 1; }
uint32_t cdrom_dma_sector_word_count(void) { return 512; }
uint32_t cdrom_dma_read(void) { return 0xCD000000u + cd_reads++; }
uint32_t cdrom_dma_read_padded(void) { return cdrom_dma_read(); }
void cdrom_debug_snapshot(CDROMDebugState *s) { memset(s, 0, sizeof *s); }
int cdrom_get_setloc_lba(void) { return 4; }

void overlay_capture_before_dma(uint32_t a, uint32_t n) { (void)a; (void)n; }
void overlay_capture_on_dma(uint32_t a, uint32_t n, const uint8_t *p) { (void)a; (void)n; (void)p; }
void dirty_ram_mark_executable_range(uint32_t a, uint32_t n) { (void)a; (void)n; }

int mdec_source_active(void) { return 0; }
void mdec_source_advance(uint32_t n) { (void)n; }
uint32_t mdec_source_dma_read(uint32_t *o) { *o = 0; return 0; }
int mdec_dma_write_ready(void) { return 0; }
int mdec_dma_read_ready(void) { return 0; }
uint32_t mdec_dma_write_words(const uint32_t *p, uint32_t n) { (void)p; (void)n; return 0; }
void mdec_dma_write_word(uint32_t v) { (void)v; }
uint32_t mdec_dma_read_word(void) { return 0; }
void mdec_debug_dma_in_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_start(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_in_end(uint32_t a, uint32_t n) { (void)a; (void)n; }
void mdec_debug_dma_out_end(uint32_t a, uint32_t n) { (void)a; (void)n; }

void spu_dma_write(uint32_t v) { spu_in = spu_in * 33u + v; }
uint32_t spu_dma_read(void) { return spu_out++; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }

uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }

void source_gpu_runtime_dma_write(void) {  }

#include "../src/psx_cycles.c"
uint32_t i_mask;
int g_ls_mode,g_precise_mode,g_psx_call_bail;
uint64_t g_guest_store_count,g_mmio_access_count;
uint32_t interrupts_cycles_to_vblank(void) { return 1000000; }
uint32_t timers_cycles_to_irq(uint32_t m) { (void)m; return 1000000; }
uint32_t cdrom_cycles_to_irq(uint32_t m) { (void)m; return 1000000; }
uint32_t sio_cycles_to_irq(uint32_t m) { (void)m; return 1000000; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return 1000000; }
void psx_spu_sample_event_service(void) {}
void interrupts_service_scheduled_events(void) {}
void sio_advance(uint32_t n) { (void)n; }
void cdrom_advance(uint32_t n) { (void)n; }
void timers_advance(uint32_t n) { (void)n; }
void interrupts_advance_cycles(uint32_t n) { (void)n; }
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
int psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}
void source_gpu_runtime_advance(void) {}

#ifndef GPU_QUEUE_BOOT_ADMISSION
int main(int argc,char **argv) {
    reset_gpu_state_for_test(); dma_init(); psx_cycles_resync_after_restore(NULL);
    dma_write(0x1f8010f0,0x0fedcba9);
    dma_write(0x1f8010f4,(1u<<23)|(1u<<18));
    if(argc>1 && !strncmp(argv[1],"stop-read",9)) {
        gpu_write_gp0(0xc0000000); gpu_write_gp0(0); gpu_write_gp0(0x00010002);
        assert(vram_read_active); gp1_dma_direction(0x04000002);
        psx_write_word(0x1000,0x14ffffff);
        for(unsigned i=0;i<20;i++) psx_write_word(0x1004+4*i,0xe1000001);
        dma_write(0x1f8010a0,0x1000); dma_write(0x1f8010a8,0x01000401);
        psx_advance_cycles(25); psx_devices_service_to_now();
        assert(gpu_queue.count==16 && gpu_linked_list.payload_index==16);
        uint64_t stopped=psx_cycle_count;
        dma_write(0x1f8010a8,0);
        assert(psx_cycle_count==stopped && !(channels[2].chcr&(1u<<24)));
        assert(gpu_linked_list.active && gpu_linked_list.payload_index==16);
        assert(vram_read_active && gpu_queue.count==16 && !(dicr&(1u<<26)));
        uint32_t gn=gpu_snapshot_bytes(),dn=dma_snapshot_bytes();
        uint8_t *gw=malloc(gn),*dw=malloc(dn);
        gpu_snapshot_write(gw); dma_snapshot_write(dw);
        (void)gpu_read_gpuread(); assert(!vram_read_active);
        psx_advance_cycles(100); psx_devices_service_to_now();
        assert(gpu_linked_list.payload_index==16 && !(dicr&(1u<<26)));
        /* Stopping an already-paused list cannot clock or discard its tail. */
        uint64_t repeat_cycle=psx_cycle_count;
        printf("before repeated stop: busy=%u active=%u count=%u index=%u\n",
            (channels[2].chcr>>24)&1,gpu_linked_list.active,gpu_queue.count,gpu_linked_list.payload_index);
        fflush(stdout);
        dma_write(0x1f8010a8,0);
        assert(psx_cycle_count==repeat_cycle && gpu_linked_list.active);
        assert(gpu_linked_list.payload_index==16 && !(dicr&(1u<<26)));
        psx_cycle_count=stopped;
        assert(gpu_snapshot_read(gw,gn) && dma_snapshot_read(dw,dn));
        psx_cycles_resync_after_restore(NULL);
        (void)gpu_read_gpuread();
        psx_write_word(0x1004+18*4,0xe2000123);
        int new_list=!strcmp(argv[1],"stop-read-new");
        int block=!strcmp(argv[1],"stop-read-block");
        if(new_list || block) {
            psx_write_word(0x2000,new_list?0x01ffffff:0xe2000321);
            psx_write_word(0x2004,0xe2000321);
            dma_write(0x1f8010a0,0x2000); dma_write(0x1f8010a4,1);
        }
        dma_write(0x1f8010a8,block?0x11000001:0x01000401);
        psx_advance_cycles(100); psx_devices_service_to_now();
        assert(!gpu_linked_list.active && texture_window_value==((new_list||block)?0x321:0x123));
        assert(dicr&(1u<<26)); free(gw); free(dw);
        puts("PASS readback-blocked stop, paused snapshot and live resume"); return 0;
    }
    if(argc>1 && !strcmp(argv[1],"scheduler")) {
        fill(); gpu_write_gp0(0xe1000123);
        assert(texpage_x==0);
        psx_advance_cycles(25000); psx_devices_service_to_now();
        assert(texpage_x==3);
        puts("PASS real device scheduler retires queued GPU work without MMIO"); return 0;
    }
    if(argc>1 && !strcmp(argv[1],"upload")) {
        fill();
        gpu_write_gp0(0xa0000000); gpu_write_gp0(0x00400020); gpu_write_gp0(0x00010040);
        for(unsigned i=0;i<32;i++) psx_write_word(0x3000+4*i,0x12345678);
        dma_write(0x1f8010a0,0x3000); dma_write(0x1f8010a4,0x00010020);
        dma_write(0x1f8010a8,0x01000201);
        assert(channels[2].chcr&(1u<<24));
        assert(vram[64*1024+32]==0);
        psx_advance_cycles(25); psx_devices_service_to_now();
        uint32_t gn=gpu_snapshot_bytes(),dn=dma_snapshot_bytes();
        uint8_t *gw=malloc(gn),*dw=malloc(dn);
        gpu_snapshot_write(gw); dma_snapshot_write(dw);
        uint64_t saved_cycle=psx_cycle_count;
        psx_advance_cycles(25000); psx_devices_service_to_now();
        assert(!(channels[2].chcr&(1u<<24)));
        assert(vram[64*1024+32]==0x5678 && vram[64*1024+95]==0x1234);
        psx_cycle_count=saved_cycle;
        assert(gpu_snapshot_read(gw,gn) && dma_snapshot_read(dw,dn));
        psx_cycles_resync_after_restore(NULL);
        psx_write_word(0x307c,0xabcd1111);
        psx_advance_cycles(25000); psx_devices_service_to_now();
        assert(vram[64*1024+94]==0x1111 && vram[64*1024+95]==0xabcd);
        assert(!(channels[2].chcr&(1u<<24)));
        free(gw); free(dw);
        puts("PASS asynchronous upload, pending snapshot, and live tail mutation"); return 0;
    }
    /* A long fill blocks consumption. More than 16 words requires a pause. */
    fill();
    psx_write_word(0x1000,0x14002000);
    for(unsigned i=0;i<20;i++) psx_write_word(0x1004+4*i,0xe1000001);
    psx_write_word(0x2000,0x01ffffff); psx_write_word(0x2004,0xe1000002);
    dma_write(0x1f8010a0,0x1000); dma_write(0x1f8010a8,0x01000401);
    psx_advance_cycles(25); psx_devices_service_to_now();
    assert(gpu_linked_list.active && gpu_linked_list.phase==DMA_GPU_LL_PHASE_PAYLOAD);
    assert(gpu_linked_list.payload_index<20);
#ifndef TEST_BASE
    unsigned before_release=gpu_linked_list.payload_index;
    psx_advance_cycles(gpu_queue_cycles_to_event()); psx_devices_service_to_now();
    assert(gpu_linked_list.payload_index==before_release+1);
#endif
    psx_write_word(0x1004+18*4,0xe2000123); /* active-node word still unread */
    /* A real stop must finish this packet, even beyond its nominal clocks. */
    dma_write(0x1f8010a8,0);
    assert(channels[2].madr==0x2000 && !gpu_linked_list.active);
    assert(!(dicr & (1u<<26)));
    psx_write_word(0x2004,0xe1000007); /* later packet remains live */
    dma_write(0x1f8010a8,0x01000401);
    psx_advance_cycles(25000); psx_devices_service_to_now();
    assert(!gpu_linked_list.active && texpage_x==7 && texture_window_value==0x123);
    assert(dicr & (1u<<26));
    puts("PASS production GPU backpressure, packet stop, live later packet, completion");
    return 0;
}

#endif
