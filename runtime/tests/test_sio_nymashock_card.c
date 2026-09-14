/* Authored SIO transactions on the production device with an explicit clock. */
#include "../src/sio.c"
#include "../src/memcard.c"
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

static void advance(unsigned n) { while(n--) { clock_now++;sio_advance(1); } }
static uint64_t nv_hash(void) {
    uint8_t bytes[131072];
    assert(memcard_debug_read_buffer(0,0,sizeof bytes,bytes)==sizeof bytes);
    uint64_t hash=UINT64_C(14695981039346656037);
    for (unsigned i=0;i<sizeof bytes;++i) { hash^=bytes[i];hash*=UINT64_C(1099511628211); }
    return hash;
}
static void card_transaction(const char *name,const uint8_t *tx,unsigned count) {
    sio_write(0x1F80104A,0); sio_write(0x1F80104A,0x1003);
    printf("{\"case\":\"%s\",\"bytes\":[",name);
    for (unsigned i=0;i<count;++i) {
        sio_write(0x1F801040,tx[i]); advance(1088);
        unsigned rx=sio_read(0x1F801040);
        int delay=sio_pending_ack ? sio_ack_remaining : 0;
        printf("%s[%u,%u,%d]",i?",":"",tx[i],rx,delay);
        advance(320); sio_write(0x1F80104A,0x1013); i_stat&=~0x80u;
    }
    sio_write(0x1F80104A,0);
    printf("],\"nv_fnv1a64\":\"%016llx\"}\n",(unsigned long long)nv_hash());
}
static void card_power(void) {
    sio_init(); sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);
}
#include "nymashock_card_cases.h"
static void start_card(unsigned ctrl) {
    card_power(); i_stat=1u<<9;
    sio_write(0x1F80104A,ctrl); sio_write(0x1F801040,0x81);
}
static void check_source_clock(void) {
    const unsigned ack=0x80, spu=1u<<9;
    if (!sio_source_card) return;
    assert(sio_snapshot_bytes()==0);
    start_card(0x1003);
    advance(1087); assert(!sio_pending_ack && !(i_stat&ack));
    advance(1); assert(sio_pending_ack && sio_ack_remaining==256);
    assert(sio_irq_pending_delay==256 && sio_read(0x1F801040)==255);
    advance(255); assert(!(sio_read(0x1F801044)&ack) && !(i_stat&ack));
    advance(1); assert(sio_read(0x1F801044)&ack); assert(i_stat==(spu|ack));
    for (unsigned i=0;i<100;++i) assert(sio_read(0x1F801044)&ack);
    advance(31); assert(sio_read(0x1F801044)&ack);
    advance(1); assert(!(sio_read(0x1F801044)&ack)); assert(i_stat==(spu|ack));
    assert(!sio_pending_ack && !g_sio_timing_active);
    /* FrontIO Update consumes the supplied interval across both deadlines. */
    start_card(0x1003); clock_now+=1375; sio_advance(1375);
    assert(sio_ack_pulse_remaining==1 && (i_stat&ack));
    advance(1); assert(!sio_ack_pulse_remaining && !sio_pending_ack);
    /* Already-latched INTC IRQ is not a reason to defer or replay DSR. */
    start_card(0x1003); i_stat|=ack; advance(1376);
    assert(i_stat==(spu|ack) && !sio_pending_ack && !g_sio_timing_active);
    /* IRQ mask does not suppress the physical DSR pulse. */
    start_card(3); advance(1344);
    assert(i_stat==spu && (sio_read(0x1F801044)&ack));
    advance(32); assert(!(sio_read(0x1F801044)&ack));
    /* DTR falling cancels the pending pulse; it must not flush an IRQ. */
    start_card(0x1003); advance(1088); sio_write(0x1F80104A,0x1001);
    advance(1024); assert(i_stat==spu && !sio_pending_ack && !sio_ack_pulse_remaining);
    start_card(0x1003); advance(1344); sio_write(0x1F80104A,0x1001);
    assert(!(sio_read(0x1F801044)&ack) && !sio_ack_pulse_remaining);
}
int main(int argc,char **argv) {
    if (argc!=3) return 2;
    MemcardSlotConfig slots[2]={{argv[1],1},{NULL,0}};
    memcard_init_slots(NULL,slots);
    assert(memcard_is_present(0) && !memcard_is_present(1));
    card_power(); card_cases();
    uint8_t final_card[131072];
    assert(memcard_export_raw(0,final_card)==0);
    FILE *f=fopen(argv[2],"wb");
    assert(f && fwrite(final_card,1,sizeof final_card,f)==sizeof final_card);
    assert(fclose(f)==0);
    check_source_clock();
    return 0;
}
