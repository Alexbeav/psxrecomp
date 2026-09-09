/* Source-owned observer fixture; driven by test_input_route_observer.py. */
#include "input_route_observer.h"
#include "debug_server.h"
#include <string.h>
CPUState *debug_cpu_ptr = NULL;
uint32_t i_stat = 5, i_mask = 13;
uint64_t psx_cycle_count = 123456;
void gpu_observer_video_state(uint32_t *out) { const uint32_t v[5]={100,16,256,1,0}; memcpy(out,v,sizeof v); }
void interrupts_observer_field_state(uint32_t *out) { const uint32_t v[3]={7,563969,1}; memcpy(out,v,sizeof v); }
/* Snapshot accessor copies all three channels without servicing devices. */
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3], uint16_t target[3], int32_t irq_line[3], uint32_t frac[3]) {
    for (unsigned i=0;i<3;i++) { counter[i]=(uint16_t)(0xFFFF-i); mode[i]=0x148+i;
        target[i]=(uint16_t)i; irq_line[i]=(int32_t)i-1; frac[i]=2145-i; }
}
#include "psx_memory.h"
#include "gpu.h"
#include "sio.h"
#include "cdrom.h"
#include "dma.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static uint8_t ram[PSX_MAIN_RAM_BYTES];
static const char *card_case = "";
int memcard_is_present(int slot) {
    if (!strstr(card_case,"card_")) return 0;
    return slot == 0 ? strcmp(card_case,"card_missing") != 0 : !strcmp(card_case,"card_extra");
}
int memcard_debug_read_buffer(int slot,uint32_t offset,uint32_t length,uint8_t *out) {
    if (slot || (!strcmp(card_case,"card_short") && offset==4096)) return 0;
    for (unsigned i=0;i<length;++i) out[i]=(uint8_t)(offset+i);
    if (!strcmp(card_case,"card_changed") && !offset) out[0]^=1;
    return (int)length;
}
uint8_t *g_psx_ram = ram;
int event_ring_dump_stream(FILE *f) { fputs("[]\n", f); return 0; }
uint64_t cdrom_debug_get_command_history(const CDROMCommandHistoryEntry **out) { *out = NULL; return 0; }
uint64_t cdrom_debug_get_trace(const CDROMTraceEntry **out) { *out = NULL; return 0; }
uint64_t cdrom_timing_total(void) { return 0; }
int cdrom_timing_record(uint64_t seq, CdTimingPub *out) { (void)seq; (void)out; return 0; }
uint64_t cdrom_debug_get_sector_history(const CDROMSectorHistoryEntry **out) { *out = NULL; return 0; }
int cdrom_get_bursts(void *out, int max) { (void)out; (void)max; return 0; }
uint64_t dma_debug_get_cdrom_history(const DMACDROMHistoryEntry **out) { *out = NULL; return 0; }
uint32_t sio_get_trace(const SioTraceEntry **out, int *index) {
    *out = NULL; *index = 0; return 0;
}
void gl_renderer_sync_cpu(void) {}
void debug_server_dump_watched_writes(FILE *f, const uint32_t *a, uint32_t count) {
    (void)a; (void)count; fputs("{\"kind\":\"coverage\",\"total_writes\":0,\"retained_writes\":0}\n", f);
}
void vk_renderer_sync_cpu(void) {}
void gpu_get_display_info(GpuDisplayInfo *out) {
    memset(out, 0, sizeof(*out)); out->disabled = 1;
}
void gpu_display_pixel_rgb(const GpuDisplayInfo *di, uint32_t x, uint32_t y,
                           uint8_t *r, uint8_t *g, uint8_t *b) {
    (void)di; (void)x; (void)y; *r = *g = *b = 0;
}
int main(int argc, char **argv) {
    if (argc != 2) return 9;
    card_case = argv[1];
    if (!strncmp(argv[1], "ds_", 3)) {
        if (!input_route_observer_dualshock_init(2)) return 4;
        const uint8_t source[4] = {1,0,128,255}, source2[4] = {129,130,131,132};
        uint8_t sticks[4] = {0,1,254,128};
        const uint8_t sticks2[4] = {129,128,131,130}, neutral[4] = {128,128,128,128};
        input_route_observer_boundary(0, 1);
        input_route_observer_dualshock_input(0xFFEF, source);
        if (!strcmp(argv[1], "ds_missing")) input_route_observer_boundary(1, 2);
        if (!strcmp(argv[1], "ds_wrong_axis")) sticks[0] = 1;
        if (!strcmp(argv[1], "ds_unconverted")) sticks[2] = 255;
        if (!strcmp(argv[1], "ds_digital")) input_route_observer_applied(0xFFEF, 1, 0);
        input_route_observer_dualshock_applied(0xFFEF, sticks,
            strcmp(argv[1], "ds_disconnected") != 0, strcmp(argv[1], "ds_plainpad") != 0, 0);
        input_route_observer_boundary(1, 2);
        input_route_observer_dualshock_input(0xFFFF, source2);
        input_route_observer_dualshock_applied(0xFFFF, sticks2, 1, 1, 1);
        input_route_observer_boundary(2, 3);
        input_route_observer_dualshock_input(0xFFFF, !strcmp(argv[1], "ds_tail_axis") ? source : neutral);
        input_route_observer_dualshock_applied(0xFFFF, neutral, 1, 1, 1);
        input_route_observer_boundary(3, 4);
        return 8;
    }
    CPUState cpu = {0};
    if (!strcmp(argv[1], "cpu")) {
        cpu.pc=0x80012340; cpu.gpr[31]=0x80054320;
        cpu.cop0[12]=0x40000401; cpu.cop0[13]=0x420; cpu.cop0[14]=0x80012000;
        debug_cpu_ptr=&cpu;
    }
    if (!input_route_observer_init(2)) return 4;
    g_psx_ram[0] = 0x34; g_psx_ram[1] = 0x12;
    g_psx_ram[PSX_MAIN_RAM_BYTES - 2] = 0xCD; g_psx_ram[PSX_MAIN_RAM_BYTES - 1] = 0xAB;
    input_route_observer_boundary(0, 1);
    input_route_observer_input(0xFFF7);
    if (!strcmp(argv[1], "missing")) input_route_observer_boundary(1, 2);
    input_route_observer_applied(!strcmp(argv[1], "wrong") ? 0xFFFF : 0xFFF7,
        strcmp(argv[1], "disconnected") != 0, !strcmp(argv[1], "analog"));
    input_route_observer_boundary(1, 2);
    input_route_observer_input(0xFFFF);
    input_route_observer_applied(0xFFFF, 1, 0);
    input_route_observer_boundary(2, 3);
    input_route_observer_input(!strcmp(argv[1], "tail_pressed") ? 0xFFF7 : 0xFFFF);
    input_route_observer_applied(0xFFFF, 1, 0);
    input_route_observer_boundary(3, 4);
    return 8; /* observer must exit at the declared boundary */
}
