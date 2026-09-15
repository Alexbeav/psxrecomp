/* DualShock 0x45 "Query Model and Mode": the exact reply bytes.
 *
 * The source builds this reply in two phases (frontio.c, case 0x4500 then
 * 0x4501):
 *
 *     0x4500:  transmit_buffer[0] = 0x01                  ; device type
 *     0x4501:  transmit_buffer[0] = 0x02
 *              transmit_buffer[1] = analog_mode ? 1 : 0   ; live mode
 *              transmit_buffer[2] = 0x02
 *              transmit_buffer[3] = 0x01
 *              transmit_buffer[4] = 0x00
 *
 * so after the 0xF3 0x5A config header the reply is 01 02 <mode> 02 01 00.
 * Corroborated against the pinned Nymashock build: replaying Mega Man X5
 * "Training, X" with the pad in digital mode, the source answers
 * F3 5A 01 02 00 02 01 00, and the game stores those bytes verbatim into its
 * SIO receive queue.
 *
 * This existed as a gap because nymashock_dualshock_transactions.jsonl pins 260
 * transactions -- cold-digital, config enter/exit, analog lock and a 256-case
 * axis sweep -- and none of them issues 0x45. Bio Hazard never asks the pad what
 * model it is; Mega Man X5 does, on return 1090.
 *
 * Source compatibility check, not a PS1 hardware timing claim.
 */
#include "../src/sio.c"
#include "input_dualshock_delivery.h"
#include <stdio.h>
#include <stdlib.h>

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

static unsigned checks;
static void check(int okay,const char *message) {
    checks++;
    if(!okay){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
}
static void advance(unsigned n) { while(n--) { clock_now++;sio_advance(1); } }

/* Run one transaction and collect the reply bytes. */
static void transaction(const unsigned char *tx,unsigned count,unsigned char *rx) {
    sio_write(0x1F80104A,0);
    sio_write(0x1F80104A,0x1003);
    for (unsigned i=0;i<count;++i) {
        sio_write(0x1F801040,tx[i]); advance(1088);
        rx[i] = (unsigned char)sio_read(0x1F801040);
        advance(256);
        sio_write(0x1F80104A,0x1013); i_stat &= ~0x80u;
    }
    sio_write(0x1F80104A,0);
}

static void query_model(int analog,const unsigned char *expected) {
    sio_set_pad_analog(0,analog,128,128,128,128);
    input_dualshock_deliver(0xFFEF,(uint8_t[4]){128,128,128,128});
    /* 0x43 with data 1 enters config mode; 0x45 is answered only in config. */
    const unsigned char enter[]={1,0x43,0,1,0,0,0,0,0};
    unsigned char scratch[9];
    transaction(enter,sizeof enter,scratch);
    const unsigned char query[]={1,0x45,0,0,0,0,0,0,0};
    unsigned char rx[9];
    transaction(query,sizeof query,rx);
    for (unsigned i=1;i<9;++i) {
        if (rx[i]!=expected[i-1]) {
            fprintf(stderr,"FAIL: 0x45 reply byte %u: got %02X expected %02X (analog=%d)\n",
                    i,rx[i],expected[i-1],analog);
            fprintf(stderr,"  got     ");
            for (unsigned k=1;k<9;++k) fprintf(stderr,"%02X ",rx[k]);
            fprintf(stderr,"\n  expected ");
            for (unsigned k=0;k<8;++k) fprintf(stderr,"%02X ",expected[k]);
            fprintf(stderr,"\n");
            exit(1);
        }
    }
    checks++;
    const unsigned char leave[]={1,0x43,0,0,0,0,0,0,0};
    transaction(leave,sizeof leave,scratch);
}

int main(void) {
    sio_init(); sio_set_pad_connected(0,1); sio_set_pad_config_capable(0,1);
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);

    /* Digital: the Mega Man X5 case. The source answers exactly these bytes. */
    static const unsigned char digital[8] = { 0xF3,0x5A,0x01,0x02,0x00,0x02,0x01,0x00 };
    query_model(0,digital);

    /* Analog: only the live-mode byte moves, and it is byte 4 of the reply --
     * not byte 3, which is the constant 0x02. Writing the mode into byte 3 was
     * the defect: it destroyed the constant and left a hard-coded analog-on. */
    static const unsigned char analog[8]  = { 0xF3,0x5A,0x01,0x02,0x01,0x02,0x01,0x00 };
    query_model(1,analog);

    /* The constant fields must not depend on the mode. */
    check(digital[2]==analog[2] && digital[3]==analog[3],"device type and the 0x02 constant are mode-independent");
    check(digital[5]==analog[5] && digital[6]==analog[6] && digital[7]==analog[7],"reply tail is mode-independent");
    check(digital[4]==0x00 && analog[4]==0x01,"only byte 4 carries the live analog mode");

    printf("source DualShock 0x45 query model and mode: %u checks passed\n",checks);
    return 0;
}
