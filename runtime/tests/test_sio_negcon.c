/* neGcon byte protocol on the production SIO device (PS1B-304).
 *
 * Expected bytes come from recomp-corpus references/ps1/PERIPHERAL-NEGCON-SPEC.md,
 * which is written from PSX-SPX "Controllers - Racing Controllers" only: ID
 * 5A23h, the 42h reply 23h 5Ah <buttons lo> <buttons hi> <twist> <I> <II> <L>,
 * buttons active low (Start bit 3, D-pad 4-7, R 11, B 12, A 13, all others 1),
 * /ACK after every byte but the last, and a full 8-byte multitap seat. The
 * hi-z answer to other commands and the idle values (twist 80h, I/II/L 00h)
 * are runtime choices that the spec marks open; they are pinned here so a
 * change is deliberate. */
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

/* A 42h read of the 9-byte neGcon; compares all 9 reply bytes and the /ACK
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

/* neGcon halfword with the given active-low bits cleared. */
#define NB(bits) ((uint16_t)(0xFFFFu & ~(uint16_t)(bits)))
enum { START = 1u << 3, UP = 1u << 4, RIGHT = 1u << 5, DOWN = 1u << 6,
       LEFT = 1u << 7, R = 1u << 11, B = 1u << 12, A = 1u << 13 };

int main(void) {
    setup_bus();
    sio_set_port_device(0,SIO_DEVICE_NEGCON);
    sio_set_pad_connected(0,1);
    check(sio_get_port_device(0) == SIO_DEVICE_NEGCON,"slot 0 reports the neGcon kind");
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"slot 1 stays a pad");

    /* Spec test vectors. */
    EXPECT(0,"idle neGcon",                 0xFF,0x23,0x5A,0xFF,0xFF,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(START),0x80,0,0,0);
    EXPECT(0,"Start",                       0xFF,0x23,0x5A,0xF7,0xFF,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(UP),0x80,0,0,0);
    EXPECT(0,"D-pad Up",                    0xFF,0x23,0x5A,0xEF,0xFF,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(A),0x80,0,0,0);
    EXPECT(0,"A",                           0xFF,0x23,0x5A,0xFF,0xDF,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(B),0x80,0,0,0);
    EXPECT(0,"B",                           0xFF,0x23,0x5A,0xFF,0xEF,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(R),0x80,0,0,0);
    EXPECT(0,"R",                           0xFF,0x23,0x5A,0xFF,0xF7,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(0),0x00,0xFF,0x00,0x00);
    EXPECT(0,"full left twist, I pressed",  0xFF,0x23,0x5A,0xFF,0xFF,0x00,0xFF,0x00,0x00);
    sio_set_negcon_state(0,NB(0),0xFF,0x00,0x80,0xFF);
    EXPECT(0,"full right, II half, L",      0xFF,0x23,0x5A,0xFF,0xFF,0xFF,0x00,0x80,0xFF);
    EXPECT(0,"state holds until changed",   0xFF,0x23,0x5A,0xFF,0xFF,0xFF,0x00,0x80,0xFF);

    /* Bits the neGcon does not have always read 1, whatever the host sends
     * (DualShock Select, L3, R3, L2, R2, L1, Cross, Square positions). */
    sio_set_negcon_state(0,0x0000,0x80,0,0,0);
    EXPECT(0,"unused bits forced to 1",     0xFF,0x23,0x5A,0x07,0xC7,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(LEFT|RIGHT|DOWN),0x80,0,0,0);
    EXPECT(0,"D-pad Left/Right/Down",       0xFF,0x23,0x5A,0x1F,0xFF,0x80,0x00,0x00,0x00);

    /* Any other command: hi-z and no /ACK, the transfer ends. */
    {
        static const uint8_t probe[3] = { 0x01,0x43,0x00 };
        uint8_t rx[3], ack[3];
        txn(0,probe,3,rx,ack);
        check(rx[0] == 0xFF && ack[0] == 1,"address byte is acknowledged");
        check(rx[1] == 0xFF && ack[1] == 0,"43h gets hi-z and no /ACK");
        check(rx[2] == 0xFF,"nothing answers after the transfer ended");
        static const uint8_t probe45[3] = { 0x01,0x45,0x00 };
        txn(0,probe45,3,rx,ack);
        check(rx[1] == 0xFF && ack[1] == 0,"45h gets hi-z and no /ACK");
    }
    /* Every command byte but 42h, including the DualShock config set (43h-47h,
     * 4Ch, 4Dh, 4Fh) and bytes no controller uses: hi-z and no /ACK. The
     * neGcon has no config mode to fall into, and no byte gets a zero reply. */
    {
        unsigned silent = 0;
        for (unsigned cmd = 0; cmd <= 0xFF; ++cmd) {
            if (cmd == 0x42) continue;
            const uint8_t probe[3] = { 0x01,(uint8_t)cmd,0x00 };
            uint8_t rx[3], ack[3];
            txn(0,probe,3,rx,ack);
            if (!(rx[0] == 0xFF && ack[0] == 1 && rx[1] == 0xFF && ack[1] == 0 && rx[2] == 0xFF)) {
                fprintf(stderr,"FAIL: command %02Xh: got %02X%s %02X%s %02X (+ = /ACK)\n",
                        cmd,rx[0],ack[0]?"+":"",rx[1],ack[1]?"+":"",rx[2]);
                exit(1);
            }
            silent++;
        }
        check(silent == 255,"all 255 other command bytes get hi-z and no /ACK");
    }
    EXPECT(0,"still a neGcon after every probe", 0xFF,0x23,0x5A,0x1F,0xFF,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(0,NB(A),0x40,0x10,0x20,0x00);
    EXPECT(0,"a read after a probe still works", 0xFF,0x23,0x5A,0xFF,0xDF,0x40,0x10,0x20,0x00);

    /* The reply is taken when 42h arrives: a host update during the read
     * shows on the next read, not partway through this one. */
    {
        static const uint8_t tx[9] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
        uint8_t rx[9];
        sio_set_negcon_state(0,NB(0),0x10,0x20,0x30,0x40);
        sio_write(0x1F80104A,0);
        sio_write(0x1F80104A,0x1003u);
        for (unsigned i=0;i<9;++i) {
            if (i == 4) sio_set_negcon_state(0,NB(A),0xE0,0xD0,0xC0,0xB0);
            sio_write(0x1F801040,tx[i]); advance(1088);
            rx[i] = (uint8_t)sio_read(0x1F801040);
            advance(256);
            sio_write(0x1F80104A,0x1013u); i_stat &= ~0x80u;
        }
        sio_write(0x1F80104A,0);
        advance(2000);
        check(rx[3] == 0xFF && rx[4] == 0xFF && rx[5] == 0x10 && rx[6] == 0x20 &&
              rx[7] == 0x30 && rx[8] == 0x40,"a read keeps the state it started with");
    }
    EXPECT(0,"the update shows on the next read", 0xFF,0x23,0x5A,0xFF,0xDF,0xE0,0xD0,0xC0,0xB0);

    /* Every twist value, and each of I, II and L at 0, 1, 127, 128, 254 and
     * 255 with the other axes elsewhere: the four bytes are independent and
     * never change the button bytes. */
    {
        static const uint8_t tx[9] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
        uint8_t rx[9], ack[9];
        for (unsigned v=0;v<=0xFF;++v) {
            sio_set_negcon_state(0,NB(START),(uint8_t)v,0x11,0x22,0x33);
            txn(0,tx,9,rx,ack);
            if (rx[3] != 0xF7 || rx[4] != 0xFF || rx[5] != v || rx[6] != 0x11 ||
                rx[7] != 0x22 || rx[8] != 0x33) {
                fprintf(stderr,"FAIL: twist %02Xh read back wrong\n",v);
                exit(1);
            }
        }
        checks++;
        static const uint8_t levels[6] = { 0x00,0x01,0x7F,0x80,0xFE,0xFF };
        for (unsigned axis=0;axis<3;++axis) {
            for (unsigned k=0;k<6;++k) {
                uint8_t p[3] = { 0x5A,0xA5,0x3C };
                p[axis] = levels[k];
                sio_set_negcon_state(0,NB(0),0x9C,p[0],p[1],p[2]);
                txn(0,tx,9,rx,ack);
                if (rx[3] != 0xFF || rx[4] != 0xFF || rx[5] != 0x9C || rx[6] != p[0] ||
                    rx[7] != p[1] || rx[8] != p[2]) {
                    fprintf(stderr,"FAIL: pressure axis %u at %02Xh read back wrong\n",
                            axis,levels[k]);
                    exit(1);
                }
            }
        }
        checks++;
    }

    /* Out-of-range slots are ignored, not written. */
    sio_set_negcon_state(0,NB(A),0x40,0x10,0x20,0x00);
    sio_set_port_device(-1,SIO_DEVICE_NEGCON);
    sio_set_port_device(PSX_MAX_PLAYERS,SIO_DEVICE_NEGCON);
    sio_set_negcon_state(-1,NB(B),0,0xFF,0xFF,0xFF);
    sio_set_negcon_state(PSX_MAX_PLAYERS,NB(B),0,0xFF,0xFF,0xFF);
    check(sio_get_port_device(-1) == SIO_DEVICE_PAD &&
          sio_get_port_device(PSX_MAX_PLAYERS) == SIO_DEVICE_PAD,"out-of-range slots read as pads");
    EXPECT(0,"slot 0 is untouched by out-of-range writes", 0xFF,0x23,0x5A,0xFF,0xDF,0x40,0x10,0x20,0x00);

    /* The host state setter is ignored for a slot that is not a neGcon. */
    sio_set_negcon_state(1,NB(A),0x00,0xFF,0xFF,0xFF);
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"slot 1 is still a pad");

    /* sio_init keeps the kind but returns the neGcon to idle. */
    setup_bus();
    check(sio_get_port_device(0) == SIO_DEVICE_NEGCON,"the kind survives sio_init");
    sio_set_pad_connected(0,1);
    EXPECT(0,"sio_init resets the inputs",  0xFF,0x23,0x5A,0xFF,0xFF,0x80,0x00,0x00,0x00);

    /* Port 1 digital pad, port 2 neGcon: the pad is untouched. */
    sio_set_port_device(0,SIO_DEVICE_PAD);
    sio_set_port_device(1,SIO_DEVICE_NEGCON);
    sio_set_pad_connected(0,1); sio_set_pad_connected(1,1);
    sio_set_pad_config_capable(0,0);
    sio_set_pad_state_slot(0,0xFFEF);
    sio_set_negcon_state(1,NB(START),0xC0,0x7F,0,0);
    sio_set_negcon_state(0,NB(A),0,0xFF,0xFF,0xFF);   /* ignored: slot 0 is a pad */
    {
        static const uint8_t tx[5] = { 0x01,0x42,0x00,0x00,0x00 };
        uint8_t rx[5], ack[5];
        txn(0,tx,5,rx,ack);
        check(rx[1] == 0x41 && rx[2] == 0x5A && rx[3] == 0xEF && rx[4] == 0xFF,
              "port 1 pad answers 41 5A EF FF");
        check(ack[3] == 1 && ack[4] == 0,"pad /ACK pattern is unchanged");
    }
    EXPECT(1,"port 2 neGcon",               0xFF,0x23,0x5A,0xF7,0xFF,0xC0,0x7F,0x00,0x00);

    /* The kind is not in the snapshot wire. */
    {
        const uint32_t with_negcon = sio_snapshot_bytes();
        sio_set_port_device(1,SIO_DEVICE_PAD);
        check(sio_snapshot_bytes() == with_negcon,"snapshot size does not depend on the kind");
    }

    /* A different plug starts idle: nothing carries over a kind change. */
    sio_set_port_device(1,SIO_DEVICE_NEGCON);
    sio_set_negcon_state(1,NB(A|B),0x00,0xFF,0xFF,0xFF);
    sio_set_port_device(1,SIO_DEVICE_MOUSE);
    sio_set_port_device(1,SIO_DEVICE_NEGCON);
    EXPECT(1,"switching kinds resets it",   0xFF,0x23,0x5A,0xFF,0xFF,0x80,0x00,0x00,0x00);
    sio_set_negcon_state(1,NB(A),0x11,0x22,0x33,0x44);
    sio_set_port_device(1,SIO_DEVICE_NEGCON);   /* same kind: a no-op */
    EXPECT(1,"re-asserting the kind keeps state", 0xFF,0x23,0x5A,0xFF,0xDF,0x11,0x22,0x33,0x44);

    /* An unknown kind falls back to a pad. */
    sio_set_port_device(1,99);
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"an unknown kind is a pad");
    sio_set_port_device(1,SIO_DEVICE_NEGCON);

    /* Disconnected neGcon: nothing answers. */
    sio_set_pad_connected(1,0);
    {
        static const uint8_t tx[3] = { 0x01,0x42,0x00 };
        uint8_t rx[3], ack[3];
        txn(1,tx,3,rx,ack);
        check(rx[1] == 0xFF && ack[0] == 0,"a disconnected neGcon port is silent");
    }
    sio_set_pad_connected(1,1);

    /* Netplay canonicalization turns every seat back into a pad. */
    sio_netplay_canonicalize_session_pads(2);
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"netplay seats are pads");

#if PSX_MAX_PLAYERS >= 5
    /* Multitap on port 1, neGcon on seat C: its seat in a bulk read is exactly
     * 23 5A b1 b2 twist I II L, with no padding (PSX-SPX multitap method 1). */
    setup_bus();
    sio_set_multitap_port(0); sio_set_multitap(1);
    for (int s=0;s<4;++s) {
        sio_set_port_device(s,s == 2 ? SIO_DEVICE_NEGCON : SIO_DEVICE_PAD);
        sio_set_pad_connected(s,1);
        sio_set_pad_config_capable(s,0);
    }
    sio_set_negcon_state(2,NB(B|UP),0x30,0xE0,0x05,0xFF);
    {
        static const uint8_t req[5] = { 0x01,0x42,0x01,0x00,0x00 };
        uint8_t tx[35] = { 0x01,0x42,0x00 };
        uint8_t rx[35], ack[35];
        txn(0,req,5,rx,ack);
        txn(0,tx,35,rx,ack);
        check(rx[1] == 0x80 && rx[2] == 0x5A,"bulk read has the multitap ID");
        static const uint8_t seat_b[8] = { 0x41,0x5A,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
        static const uint8_t seat_c[8] = { 0x23,0x5A,0xEF,0xEF,0x30,0xE0,0x05,0xFF };
        check(!memcmp(&rx[11],seat_b,8),"seat B is a digital pad");
        check(!memcmp(&rx[19],seat_c,8),"seat C is the neGcon, 8 bytes, no padding");
    }
    sio_set_multitap(0);
#endif

    printf("sio_negcon: %u checks passed (players=%d)\n",checks,PSX_MAX_PLAYERS);
    return 0;
}
