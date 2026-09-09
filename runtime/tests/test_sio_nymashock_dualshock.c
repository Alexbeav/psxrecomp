/* Authored SIO transactions on the production device with an explicit clock. */
#include "../src/sio.c"
#include "input_dualshock_delivery.h"
#include <stdio.h>
#include <assert.h>
uint32_t i_stat, i_mask, g_debug_current_func_addr, g_debug_last_store_pc;
static uint64_t clock_now;
uint64_t psx_get_cycle_count(void) { return clock_now; }
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t a) { (void)a;return 0; }
uint32_t psx_read_word(uint32_t a) { (void)a;return 0; }
void psx_write_word(uint32_t a,uint32_t v) { (void)a;(void)v; }
uint32_t memory_get_sr(void) { return 0; }
void psx_irq_raise(uint32_t bit,uint32_t detail) { (void)detail;i_stat|=1u<<bit; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a,uint32_t v,uint8_t w) { (void)a;(void)v;(void)w; }
uint16_t debug_server_update_poll(int slot,uint16_t buttons,int analog) { (void)slot;(void)analog;return buttons; }
void event_ring_record_aux(uint16_t a,uint8_t b,uint32_t c) { (void)a;(void)b;(void)c; }
void starvation_ring_record(uint8_t a,uint8_t b,uint8_t c,uint16_t d,uint16_t e,int f,int g,int h,int i,int j,uint8_t k,uint32_t l,uint8_t m,uint8_t n,uint8_t o,uint8_t p,int q) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;(void)m;(void)n;(void)o;(void)p;(void)q; }
void card_read_summary_record(uint8_t a,uint8_t b,uint16_t c,uint8_t d,uint8_t e,const uint8_t *f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void card_data_writes_arm(uint8_t a,uint16_t b,uint8_t c,uint8_t d) { (void)a;(void)b;(void)c;(void)d; }
int memcard_is_present(int slot) { (void)slot;return 0; }
int memcard_read_sector(int slot,int sector,uint8_t *buf) { (void)slot;(void)sector;memset(buf,0,128);return 0; }
int memcard_write_sector(int slot,int sector,const uint8_t *buf) { (void)slot;(void)sector;(void)buf;return 0; }
void memcard_flush(int slot) { (void)slot; }
static void advance(unsigned n) { while(n--) { clock_now++;sio_advance(1); } }
static void controls(unsigned value) {
    uint8_t raw[4] = {(uint8_t)(value+159), (uint8_t)(value+106), (uint8_t)(value+53), (uint8_t)value};
    input_dualshock_deliver(0xFFEF, raw);
}
static void transaction(const char* name, const unsigned char* tx, unsigned count) {
    sio_write(0x1F80104A,0);
    sio_write(0x1F80104A,0x1003);
    printf("{\"case\":\"%s\",\"bytes\":[",name);
    for (unsigned i=0; i<count; ++i) {
        sio_write(0x1F801040,tx[i]); advance(1088);
        unsigned rx = sio_read(0x1F801040);
        int delay = sio_pending_ack ? sio_ack_remaining : 0;
        printf("%s[%u,%u,%d]",i?",":"",tx[i],rx,delay);
        advance(256);
        sio_write(0x1F80104A,0x1013); i_stat &= ~0x80u;
    }
    printf("]}\n");
    sio_write(0x1F80104A,0);
}
int main(void) {
    sio_init(); sio_set_pad_connected(0,1); sio_set_pad_config_capable(0,1);
    sio_set_pad_analog(0,0,128,128,128,128);
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);
    const unsigned char cold[]={1,0x42,0,0,0}, enter[]={1,0x43,0,1,0,0,0,0,0};
    const unsigned char mode[]={1,0x44,0,1,3,0,0,0,0}, leave[]={1,0x43,0,0,0,0,0,0,0};
    const unsigned char poll[]={1,0x42,0,0,0,0,0,0,0};
    controls(0);
    transaction("cold-digital",cold,sizeof cold);
    transaction("enter-config",enter,sizeof enter);
    transaction("set-analog-locked",mode,sizeof mode);
    transaction("leave-config",leave,sizeof leave);
    for (unsigned v=0;v<256;++v) {
        char name[32]; snprintf(name,sizeof name,"axis-%03u",v);
        controls(v); transaction(name,poll,sizeof poll);
    }
    return 0;
}
