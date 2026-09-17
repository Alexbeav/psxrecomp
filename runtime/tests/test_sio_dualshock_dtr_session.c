/* Source DualShock DTR session: a session whose first byte is not 0x01 silences the pad until
 * its port's DTR rises again. Authored bytes only; the source reference is Mednafen's
 * InputDevice_DualShock::SetDTR/Clock (command_phase restarts on the rising edge only). */
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
static int failures;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); failures++; } }
static void advance(unsigned n) { while(n--) { clock_now++;sio_advance(1); } }
/* One byte as the BIOS pad driver sends it; returns whether the device requested an ACK. */
static int byte_acked(unsigned value) {
    sio_write(0x1F801040, value); advance(1088);
    (void)sio_read(0x1F801040);
    int acked = sio_pending_ack != 0;
    advance(256);
    sio_write(0x1F80104A, sio_ctrl | 0x10); i_stat &= ~0x80u;
    return acked;
}
static void boot(const char *profile) {
#ifdef _WIN32
    _putenv_s("PSX_INPUT_ROUTE_PAD_ACK_MODEL", profile);
#else
    setenv("PSX_INPUT_ROUTE_PAD_ACK_MODEL", profile, 1);
#endif
    sio_init(); sio_set_pad_connected(0,1); sio_set_pad_config_capable(0,1);
    sio_set_pad_analog(0,0,128,128,128,128);
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);
    const uint8_t neutral[4] = {128,128,128,128};
    input_dualshock_deliver(0xFFFF, neutral);
}
int main(void) {
    boot("nymashock-1.29.0-dualshock");
    /* Crash 7798S return 1153: a card probe leaves port 1's DTR asserted, then the BIOS pad
     * read rewrites control without dropping DTR and sends 0x01. */
    sio_write(0x1F80104A, 0); sio_write(0x1F80104A, 0x1003);
    check(!byte_acked(0x81), "no card: the 0x81 probe is not acknowledged");
    sio_write(0x1F80104A, 0x2); sio_write(0x1F80104A, 0x1003);
    check(!byte_acked(0x01), "0x01 in a session that began with 0x81 gets no pad ACK");
    check(pad_dtr_session_mute[0] == 1 && pad_dtr_session_mute[1] == 0, "mute is per port");
    /* State encoding carries the mute. */
    uint32_t n = sio_snapshot_bytes(); uint8_t *wire = malloc(n);
    sio_snapshot_write(wire); pad_dtr_session_mute[0] = 0;
    check(sio_snapshot_read(wire, n) && pad_dtr_session_mute[0] == 1, "DTR session survives state encoding");
    wire[n - 2] = 2;
    check(!sio_snapshot_read(wire, n), "invalid session byte is refused");
    free(wire);
    /* Moving DTR to the other port drops port 1's DTR; selecting port 1 again restarts it. */
    sio_write(0x1F80104A, 0x3003);
    check(pad_dtr_session_mute[0] == 0 && pad_dtr_session_first[1] == 1, "port switch is a DTR edge on both ports");
    sio_write(0x1F80104A, 0x1003);
    check(byte_acked(0x01), "a fresh DTR session answers 0x01");
    check(byte_acked(0x42), "and continues the poll");
    sio_write(0x1F80104A, 0);
    /* Negative control: the default profile keeps its device routing. */
    boot("");
    sio_write(0x1F80104A, 0); sio_write(0x1F80104A, 0x1003);
    (void)byte_acked(0x81);
    sio_write(0x1F80104A, 0x2); sio_write(0x1F80104A, 0x1003);
    check(byte_acked(0x01), "default profile still routes 0x01 to the pad");
    if (failures) return 1;
    puts("DualShock DTR session: source mute after a non-pad first byte, per port, encoded; default unchanged");
    return 0;
}
