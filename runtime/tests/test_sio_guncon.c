/* GunCon byte protocol on the production SIO device (PS1B-305).
 *
 * Expected bytes come from recomp-corpus references/ps1/PERIPHERAL-GUNCON-SPEC.md,
 * which is written from PSX-SPX "Lightguns - Namco (GunCon)" only: ID 5A63h,
 * the 42h reply 63h 5Ah <buttons lo> <buttons hi> <X lo> <X hi> <Y lo> <Y hi>,
 * buttons active low (A bit 3, trigger bit 13, B bit 14, all others 1),
 * "no light" = X 0001h / Y 000Ah, /ACK after every byte but the last, and a
 * full 8-byte multitap seat. The hi-z answer to other commands is a runtime
 * choice that the spec marks open; it is pinned here so a change is
 * deliberate. */
#include "../src/sio.c"
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

/* One transaction: rx[i] is the reply to tx[i]; ack[i] says IRQ7 followed. */
static void txn(int port,const uint8_t *tx,unsigned count,uint8_t *rx,uint8_t *ack) {
    const uint32_t slot = port ? 0x2000u : 0u;
    sio_write(0x1F80104A,0);
    sio_write(0x1F80104A,0x1003u|slot);
    for (unsigned i=0;i<count;++i) {
        sio_write(0x1F801040,tx[i]); advance(1088);
        rx[i] = (uint8_t)sio_read(0x1F801040);
        advance(256);
        ack[i] = (uint8_t)((i_stat >> 7) & 1u);
        sio_write(0x1F80104A,0x1013u|slot); i_stat &= ~0x80u;
    }
    sio_write(0x1F80104A,0);
    advance(2000);
}

/* A 42h read of the 9-byte GunCon; compares all 9 reply bytes and the /ACK
 * pattern (after bytes 1..8, not after byte 9). */
static void expect_read(int port,const uint8_t want[9],const char *what) {
    static const uint8_t tx[9] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
    uint8_t rx[9], ack[9];
    txn(port,tx,9,rx,ack);
    int ok = 1;
    for (unsigned i=0;i<9;++i) {
        if (rx[i] != want[i]) ok = 0;
        if (ack[i] != (i < 8 ? 1 : 0)) ok = 0;
    }
    if (!ok) {
        fprintf(stderr,"FAIL: %s\n  got   ",what);
        for (unsigned i=0;i<9;++i) fprintf(stderr,"%02X%s ",rx[i],ack[i]?"+":"");
        fprintf(stderr,"\n  want  ");
        for (unsigned i=0;i<9;++i) fprintf(stderr,"%02X%s ",want[i],i<8?"+":"");
        fprintf(stderr,"\n  (+ = /ACK followed)\n");
        exit(1);
    }
    checks++;
}
#define EXPECT(port, what, ...) do { static const uint8_t w_[9] = { __VA_ARGS__ }; expect_read(port,w_,what); } while (0)

static void setup_bus(void) {
    sio_init();
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);
}

/* GunCon halfword with the given active-low bits cleared. */
#define GB(bits) ((uint16_t)(0xFFFFu & ~(uint16_t)(bits)))
enum { BTN_A = 1u << 3, TRIGGER = 1u << 13, BTN_B = 1u << 14 };
#define NOL_X SIO_GUNCON_NO_LIGHT_X
#define NOL_Y SIO_GUNCON_NO_LIGHT_Y

int main(void) {
    setup_bus();
    sio_set_port_device(0,SIO_DEVICE_GUNCON);
    sio_set_pad_connected(0,1);
    check(sio_get_port_device(0) == SIO_DEVICE_GUNCON,"slot 0 reports the GunCon kind");
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"slot 1 stays a pad");

    /* Spec test vectors. */
    EXPECT(0,"idle: no buttons, no light",      0xFF,0x63,0x5A,0xFF,0xFF,0x01,0x00,0x0A,0x00);
    sio_set_guncon_state(0,GB(0),0x00B0,0x0070);
    EXPECT(0,"aimed at B0h, 70h",               0xFF,0x63,0x5A,0xFF,0xFF,0xB0,0x00,0x70,0x00);
    sio_set_guncon_state(0,GB(TRIGGER),0x01CD,0x00F8);
    EXPECT(0,"trigger at 1CDh, F8h",            0xFF,0x63,0x5A,0xFF,0xDF,0xCD,0x01,0xF8,0x00);
    sio_set_guncon_state(0,GB(BTN_A),NOL_X,NOL_Y);
    EXPECT(0,"A, no light",                     0xFF,0x63,0x5A,0xF7,0xFF,0x01,0x00,0x0A,0x00);
    sio_set_guncon_state(0,GB(BTN_B),0x004D,0x0019);
    EXPECT(0,"B at 4Dh, 19h",                   0xFF,0x63,0x5A,0xFF,0xBF,0x4D,0x00,0x19,0x00);
    sio_set_guncon_state(0,GB(TRIGGER|BTN_B),0x0100,0x0127);
    EXPECT(0,"trigger and B at 100h, 127h",     0xFF,0x63,0x5A,0xFF,0x9F,0x00,0x01,0x27,0x01);
    EXPECT(0,"state holds until changed",       0xFF,0x63,0x5A,0xFF,0x9F,0x00,0x01,0x27,0x01);

    /* Bits the GunCon does not have always read 1, whatever the host sends. */
    sio_set_guncon_state(0,0x0000,NOL_X,NOL_Y);
    EXPECT(0,"unused bits forced to 1",         0xFF,0x63,0x5A,0xF7,0x9F,0x01,0x00,0x0A,0x00);

    /* Any other command: hi-z and no /ACK, the transfer ends. */
    {
        static const uint8_t probe[3] = { 0x01,0x43,0x00 };
        uint8_t rx[3], ack[3];
        txn(0,probe,3,rx,ack);
        check(rx[0] == 0xFF && ack[0] == 1,"address byte is acknowledged");
        check(rx[1] == 0xFF && ack[1] == 0,"43h gets hi-z and no /ACK");
        check(rx[2] == 0xFF,"nothing answers after the transfer ended");
    }
    sio_set_guncon_state(0,GB(0),0x0123,0x0045);
    EXPECT(0,"a read after a probe still works", 0xFF,0x63,0x5A,0xFF,0xFF,0x23,0x01,0x45,0x00);

    /* The host state setter is ignored for a slot that is not a GunCon, and
     * the neGcon setter is ignored for a GunCon. */
    sio_set_guncon_state(1,GB(TRIGGER),0x0100,0x0100);
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"slot 1 is still a pad");
    sio_set_negcon_state(0,0x0000,0x00,0xFF,0xFF,0xFF);
    EXPECT(0,"neGcon state does not reach a GunCon", 0xFF,0x63,0x5A,0xFF,0xFF,0x23,0x01,0x45,0x00);

    /* sio_init keeps the kind but returns the gun to idle (no light). */
    setup_bus();
    check(sio_get_port_device(0) == SIO_DEVICE_GUNCON,"the kind survives sio_init");
    sio_set_pad_connected(0,1);
    EXPECT(0,"sio_init resets to no light",     0xFF,0x63,0x5A,0xFF,0xFF,0x01,0x00,0x0A,0x00);

    /* Port 1 digital pad, port 2 GunCon: the pad is untouched. */
    sio_set_port_device(0,SIO_DEVICE_PAD);
    sio_set_port_device(1,SIO_DEVICE_GUNCON);
    sio_set_pad_connected(0,1); sio_set_pad_connected(1,1);
    sio_set_pad_config_capable(0,0);
    sio_set_pad_state_slot(0,0xFFEF);
    sio_set_guncon_state(1,GB(TRIGGER),0x00C0,0x0080);
    {
        static const uint8_t tx[5] = { 0x01,0x42,0x00,0x00,0x00 };
        uint8_t rx[5], ack[5];
        txn(0,tx,5,rx,ack);
        check(rx[1] == 0x41 && rx[2] == 0x5A && rx[3] == 0xEF && rx[4] == 0xFF,
              "port 1 pad answers 41 5A EF FF");
        check(ack[3] == 1 && ack[4] == 0,"pad /ACK pattern is unchanged");
    }
    EXPECT(1,"port 2 GunCon",                   0xFF,0x63,0x5A,0xFF,0xDF,0xC0,0x00,0x80,0x00);

    /* The kind is not in the snapshot wire. */
    {
        const uint32_t with_gun = sio_snapshot_bytes();
        sio_set_port_device(1,SIO_DEVICE_PAD);
        check(sio_snapshot_bytes() == with_gun,"snapshot size does not depend on the kind");
    }

    /* A different plug starts idle: nothing carries over a kind change. */
    sio_set_port_device(1,SIO_DEVICE_GUNCON);
    sio_set_guncon_state(1,GB(TRIGGER|BTN_A|BTN_B),0x0111,0x0022);
    sio_set_port_device(1,SIO_DEVICE_NEGCON);
    sio_set_port_device(1,SIO_DEVICE_GUNCON);
    EXPECT(1,"switching kinds resets it",       0xFF,0x63,0x5A,0xFF,0xFF,0x01,0x00,0x0A,0x00);

    /* The neGcon keeps its own reply next to a GunCon. */
    sio_set_port_device(0,SIO_DEVICE_NEGCON);
    {
        static const uint8_t tx[9] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
        uint8_t rx[9], ack[9];
        txn(0,tx,9,rx,ack);
        check(rx[1] == 0x23 && rx[5] == 0x80,"port 1 neGcon still answers 23h, twist 80h");
    }

    /* Disconnected GunCon: nothing answers. */
    sio_set_pad_connected(1,0);
    {
        static const uint8_t tx[3] = { 0x01,0x42,0x00 };
        uint8_t rx[3], ack[3];
        txn(1,tx,3,rx,ack);
        check(rx[1] == 0xFF && ack[0] == 0,"a disconnected GunCon port is silent");
    }
    sio_set_pad_connected(1,1);

    /* Netplay canonicalization turns every seat back into a pad. */
    sio_netplay_canonicalize_session_pads(2);
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"netplay seats are pads");

#if PSX_MAX_PLAYERS >= 5
    /* Multitap on port 1, GunCon on seat D: its seat in a bulk read is exactly
     * 63 5A b1 b2 Xlo Xhi Ylo Yhi, with no padding (PSX-SPX multitap method 1). */
    setup_bus();
    sio_set_multitap_port(0); sio_set_multitap(1);
    for (int s=0;s<4;++s) {
        sio_set_port_device(s,s == 3 ? SIO_DEVICE_GUNCON : SIO_DEVICE_PAD);
        sio_set_pad_connected(s,1);
        sio_set_pad_config_capable(s,0);
    }
    sio_set_guncon_state(3,GB(TRIGGER),0x0155,0x0066);
    {
        static const uint8_t req[5] = { 0x01,0x42,0x01,0x00,0x00 };
        uint8_t tx[35] = { 0x01,0x42,0x00 };
        uint8_t rx[35], ack[35];
        txn(0,req,5,rx,ack);
        txn(0,tx,35,rx,ack);
        check(rx[1] == 0x80 && rx[2] == 0x5A,"bulk read has the multitap ID");
        static const uint8_t seat_c[8] = { 0x41,0x5A,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
        static const uint8_t seat_d[8] = { 0x63,0x5A,0xFF,0xDF,0x55,0x01,0x66,0x00 };
        check(!memcmp(&rx[19],seat_c,8),"seat C is a digital pad");
        check(!memcmp(&rx[27],seat_d,8),"seat D is the GunCon, 8 bytes, no padding");
    }
    sio_set_multitap(0);
#endif

    printf("sio_guncon: %u checks passed (players=%d)\n",checks,PSX_MAX_PLAYERS);
    return 0;
}
