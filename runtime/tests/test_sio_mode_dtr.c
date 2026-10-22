/* Physical MODE under PSX_INPUT_ROUTE_MODE_MODEL=octoshock-2.4.2 (PS1G-127, Chrono Cross 4538M, record 4442).
 * The pad only latches the button when an input arrives; the press is evaluated when the selected
 * port's DTR falls. It toggles analog once per press, a press while the mode is locked is consumed,
 * and a held button does not toggle again [ORACLE FIXTURE: corpus PSX-TAS-001].
 * The model below is authored from that rule; it is checked against the runtime over scripted cases and
 * a seeded random walk of inputs, locks and control writes, then against snapshot/restore and the default. */
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
typedef struct { int dtr, button, previous, locked, analog; } ModelPad;
static ModelPad model[2];
static int failures, comparisons;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); failures++; } }
static void put(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
static void model_dtr(int port, int dtr) {
    const int was = model[port].dtr;
    model[port].dtr = dtr;
    if (was && !dtr) {
        if (model[port].button && !model[port].previous && !model[port].locked) model[port].analog ^= 1;
        model[port].previous = model[port].button;
    }
}
static void compare(void) {
    for (int p = 0; p < 2; ++p) {
        check(pad_analog[p] == model[p].analog, "analog mode differs from the model");
        check(pad_mode_previous[p] == model[p].previous, "edge latch differs from the model");
    }
    ++comparisons;
}
static void input(int port, int pressed) { sio_set_pad_mode_button(port, pressed); model[port].button = pressed != 0; compare(); }
static void ctrl(unsigned value) {
    sio_write(0x1F80104A, value);
    const unsigned next = (value & 0x40) ? 0 : value;
    for (int p = 0; p < 2; ++p) model_dtr(p, (next & 2) && (((next & 0x2000) != 0) == p));
    compare();
}
static void lock_mode(int port, int locked) { analog_mode_locked[port] = (uint8_t)locked; model[port].locked = locked; compare(); }
int main(void) {
    put("PSX_INPUT_ROUTE_MODE_MODEL", "octoshock-2.4.2");
    put("PSX_INPUT_ROUTE_PAD_ACK_MODEL", "nymashock-1.29.0-dualshock");
    sio_init();
    memset(model, 0, sizeof model);
    input(0, 1); check(!pad_analog[0], "an input alone must not toggle");
    ctrl(2); ctrl(0); check(pad_analog[0], "the first DTR fall toggles");
    ctrl(2); ctrl(0); check(pad_analog[0], "a held button must not toggle twice");
    input(0, 0); ctrl(2); ctrl(0);
    input(0, 1); lock_mode(0, 1); ctrl(2); ctrl(0); lock_mode(0, 0); ctrl(2); ctrl(0);
    check(pad_analog[0], "a press while locked is consumed, not queued");
    input(1, 1); ctrl(0x2002); ctrl(2); check(pad_analog[1], "moving DTR to the other port is a fall on the first");
    ctrl(0x42); ctrl(0); check(1, "a control reset drops every DTR");
    unsigned seed = 19;
    for (unsigned i = 0; i < 2000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const int p = (seed >> 8) & 1;
        switch (seed % 4) {
        case 0: input(p, (seed >> 9) & 1); break;
        case 1: lock_mode(p, (seed >> 10) & 1); break;
        case 2: ctrl(((seed >> 12) & 1) ? 2 : 0x2002); break;
        default: ctrl(0); break;
        }
    }
    const unsigned n = sio_snapshot_bytes();
    uint8_t *wire = malloc(n);
    uint8_t button[2], previous[2];
    sio_snapshot_write(wire);
    memcpy(button, pad_mode_button, 2); memcpy(previous, pad_mode_previous, 2);
    memset(pad_mode_button, 0, 2); memset(pad_mode_previous, 0, 2);
    check(sio_snapshot_read(wire, n), "snapshot read");
    check(!memcmp(button, pad_mode_button, 2) && !memcmp(previous, pad_mode_previous, 2), "the latch survives a snapshot");
    free(wire);
    /* Default: with no MODE model the bridge ignores the button. */
    put("PSX_INPUT_ROUTE_MODE_MODEL", "");
    sio_init();
    sio_set_pad_mode_button(0, 1); sio_write(0x1F80104A, 2); sio_write(0x1F80104A, 0);
    check(!pad_analog[0] && !pad_mode_button[0], "without the model the default bridge is unchanged");
    printf("%d model comparisons, %d failures\n", comparisons, failures);
    return failures ? 1 : 0;
}
