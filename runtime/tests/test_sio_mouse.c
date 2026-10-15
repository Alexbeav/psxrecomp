/* PS1 Mouse byte protocol on the production SIO device (PS1B-279).
 *
 * Expected bytes come from recomp-corpus references/ps1/PERIPHERAL-MOUSE-SPEC.md,
 * which is written from PSX-SPX "Controllers - Mouse" only: ID 5A12h, the 42h
 * reply 12h 5Ah FFh <buttons> <dX> <dY>, buttons active low in bits 2 (right)
 * and 3 (left) of the fifth byte, signed motion bytes, /ACK after every byte
 * but the last, and FFh padding on a multitap seat. The carry of motion past
 * one byte and the hi-z answer to other commands are runtime choices that the
 * spec marks open; they are pinned here so a change is deliberate. */
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
static unsigned cards_present;
int memcard_is_present(int slot) { return (cards_present >> slot) & 1u; }
int memcard_read_sector(int slot,int sector,uint8_t *buf) { (void)slot;(void)sector;memset(buf,0,128);return 0; }
int memcard_write_sector(int slot,int sector,const uint8_t *buf) { (void)slot;(void)sector;(void)buf;return 0; }
void memcard_flush(int slot) { (void)slot; }

static unsigned checks;
static void check(int okay,const char *message) {
    checks++;
    if(!okay){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
}
static void advance(unsigned n) { while(n--) { clock_now++;sio_advance(1); } }

/* Continue the selected DTR session: rx[i] replies to tx[i], ack[i] is IRQ7. */
static void stream(int port,const uint8_t *tx,unsigned count,uint8_t *rx,uint8_t *ack) {
    const uint32_t slot = port ? 0x2000u : 0u;
    for (unsigned i=0;i<count;++i) {
        sio_write(0x1F801040,tx[i]); advance(1088);
        rx[i] = (uint8_t)sio_read(0x1F801040);
        advance(256);
        ack[i] = (uint8_t)((i_stat >> 7) & 1u);
        sio_write(0x1F80104A,0x1013u|slot); i_stat &= ~0x80u;
    }
}

static void txn(int port,const uint8_t *tx,unsigned count,uint8_t *rx,uint8_t *ack) {
    sio_write(0x1F80104A,0);
    sio_write(0x1F80104A,0x1003u|(port ? 0x2000u : 0u));
    stream(port,tx,count,rx,ack);
    sio_write(0x1F80104A,0);
    advance(2000);
}

/* A 42h read of a 7-byte device; compares all 7 reply bytes and the /ACK
 * pattern (after bytes 1..6, not after byte 7). */
static void expect_read(int port,const uint8_t want[7],const char *what) {
    static const uint8_t tx[7] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00 };
    uint8_t rx[7], ack[7];
    txn(port,tx,7,rx,ack);
    int ok = 1;
    for (unsigned i=0;i<7;++i) {
        if (rx[i] != want[i]) ok = 0;
        if (ack[i] != (i < 6 ? 1 : 0)) ok = 0;
    }
    if (!ok) {
        fprintf(stderr,"FAIL: %s\n  got   ",what);
        for (unsigned i=0;i<7;++i) fprintf(stderr,"%02X%s ",rx[i],ack[i]?"+":"");
        fprintf(stderr,"\n  want  ");
        for (unsigned i=0;i<7;++i) fprintf(stderr,"%02X%s ",want[i],i<6?"+":"");
        fprintf(stderr,"\n  (+ = /ACK followed)\n");
        exit(1);
    }
    checks++;
}
#define EXPECT(port, what, ...) do { static const uint8_t w_[7] = { __VA_ARGS__ }; expect_read(port,w_,what); } while (0)

static void setup_bus(void) {
    sio_init();
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);
}

#if PSX_MAX_PLAYERS >= 5
static void expect_vector(const uint8_t *rx,const uint8_t *ack,const uint8_t *want,
                          unsigned count,int last_ack,const char *what) {
    for (unsigned i=0;i<count;++i) {
        const int expected_ack = last_ack >= 0 && i <= (unsigned)last_ack;
        if (rx[i] != want[i] || ack[i] != expected_ack) {
            fprintf(stderr,"%s byte %u: got %02X/%u, want %02X/%d\n",
                    what,i,rx[i],ack[i],want[i],expected_ack);
            check(0,what);
        }
    }
    check(1,what);
}

static void setup_tap(int port,const char *profile) {
#ifdef _WIN32
    _putenv_s("PSX_INPUT_ROUTE_PAD_ACK_MODEL",profile);
#else
    setenv("PSX_INPUT_ROUTE_PAD_ACK_MODEL",profile,1);
#endif
    cards_present = 0;
    setup_bus(); sio_set_multitap_port(port); sio_set_multitap(1);
    for (int s=0;s<PSX_MAX_PLAYERS;++s) {
        sio_set_port_device(s,SIO_DEVICE_PAD);
        sio_set_pad_connected(s,1); sio_set_pad_config_capable(s,0);
        sio_set_pad_state_slot(s,(uint16_t)(0xFFFFu ^ (1u << s)));
    }
}

static void method2_controls(void) {
    static const char *profiles[] = { "", "octoshock-2.2.2-digital", "nymashock-1.29.0-dualshock" };
    static const uint8_t silent[5] = { 0xFF,0xFF,0xFF,0xFF,0xFF };
    static const uint8_t card_id_tx[8] = { 0x81,0x53,0,0,0,0,0,0 };
    static const uint8_t card_id_rx[8] = { 0xFF,0x08,0x5A,0x5D,0x04,0,0,0x80 };
    for (unsigned profile=0;profile<3;++profile) {
        for (int port=0;port<2;++port) {
            /* Builds with eight seats have two taps: port 2 starts at seat 4.
             * Five through seven seats have one tap: port 2 starts at seat 1. */
            const int base = PSX_MAX_PLAYERS >= 8 ? 4*port : port;
            for (int addr=2;addr<=4;++addr) {
                const uint16_t buttons = (uint16_t)(0xFFFFu ^ (1u << (base+addr-1)));
                const uint8_t want[5] = { 0xFF,0x41,0x5A,(uint8_t)buttons,(uint8_t)(buttons>>8) };
                const uint8_t tx[5] = { (uint8_t)addr,0x42,0,0,0 };
                uint8_t rx[8],ack[8];
                setup_tap(port,profiles[profile]);
                txn(port,tx,5,rx,ack);
                expect_vector(rx,ack,want,5,3,"direct method-2 reply/ACK");
                printf("method2: players=%d profile=%s port=%d address=%02X reply=%02X%02X%02X%02X%02X ack=%d%d%d%d%d\n",
                       PSX_MAX_PLAYERS,profile ? profiles[profile] : "default",port+1,addr,
                       rx[0],rx[1],rx[2],rx[3],rx[4],ack[0],ack[1],ack[2],ack[3],ack[4]);

                /* Complete or abort a card command, then poll without a DTR edge.
                 * Default routing resumes; source profiles retain their mute. */
                for (int completed=0;completed<2;++completed) {
                    setup_tap(port,profiles[profile]); cards_present = 1u << port;
                    sio_write(0x1F80104A,0x1003u|(port ? 0x2000u : 0u));
                    if (completed) {
                        stream(port,card_id_tx,8,rx,ack);
                        expect_vector(rx,ack,card_id_rx,8,6,"completed card ID reply/ACK");
                    } else {
                        static const uint8_t abort_tx[2] = { 0x81,0xFF };
                        static const uint8_t abort_rx[2] = { 0xFF,0xFF };
                        stream(port,abort_tx,2,rx,ack);
                        expect_vector(rx,ack,abort_rx,2,0,"aborted card command reply/ACK");
                    }
                    stream(port,tx,5,rx,ack);
                    expect_vector(rx,ack,profile ? silent : want,5,profile ? -1 : 3,
                                  "card-idle method-2 reply/ACK with retained DTR mute");
                    txn(port,tx,5,rx,ack);
                    expect_vector(rx,ack,want,5,3,"fresh DTR recovers after card probe");
                }

                /* Exercise the router's saved-card entry directly. A real DTR
                 * drop clears volatile card state, so this constructed entry
                 * does not claim that cards continue across a DTR drop. */
                setup_tap(port,profiles[profile]); cards_present = 1u << port;
                sio_write(0x1F80104A,0x1003u|(port ? 0x2000u : 0u));
                stream(port,card_id_tx,3,rx,ack);
                expect_vector(rx,ack,card_id_rx,3,2,"card prefix reply/ACK");
                mc_save_slot(port);
                const McSlotState saved = mc_slots[port];
                active_device = DEV_NONE;
                pad_dtr_session_first[port] = 1; pad_dtr_session_mute[port] = 0;
                stream(port,tx,5,rx,ack);
                expect_vector(rx,ack,want,5,3,"method-2 poll from saved-card router entry");
                check(!memcmp(&saved,&mc_slots[port],sizeof saved),"pad poll preserves saved card state");
                static const uint8_t continuation[5] = { 0,0,0,0,0 };
                active_device = DEV_NONE;
                stream(port,continuation,5,rx,ack);
                expect_vector(rx,ack,card_id_rx+3,5,3,"saved card continuation reply/ACK");

                /* No card responds, but the source-profile pad still sees 81h as
                 * the first DTR byte and stays muted until the next session. */
                setup_tap(port,profiles[profile]);
                sio_write(0x1F80104A,0x1003u|(port ? 0x2000u : 0u));
                const uint8_t absent = 0x81;
                stream(port,&absent,1,rx,ack);
                expect_vector(rx,ack,silent,1,-1,"absent card probe reply/ACK");
                stream(port,tx,5,rx,ack);
                expect_vector(rx,ack,profile ? silent : want,5,profile ? -1 : 3,
                              "absent-card DTR mute reply/ACK");
                txn(port,tx,5,rx,ack);
                expect_vector(rx,ack,want,5,3,"fresh DTR recovers after absent card");

                setup_tap(port,profiles[profile]);
                sio_set_pad_connected(base+addr-1,0);
                txn(port,tx,5,rx,ack);
                expect_vector(rx,ack,silent,5,-1,"disconnected method-2 seat reply/ACK");
            }
            setup_tap(port,profiles[profile]);
            static const uint8_t invalid[5] = { 5,0x42,0,0,0 };
            uint8_t rx[5],ack[5];
            txn(port,invalid,5,rx,ack);
            expect_vector(rx,ack,silent,5,-1,"05h on an enabled tap is silent");
#if PSX_MAX_PLAYERS < 8
            for (int addr=2;addr<=5;++addr) {
                const uint8_t tx[5] = { (uint8_t)addr,0x42,0,0,0 };
                txn(1-port,tx,5,rx,ack);
                expect_vector(rx,ack,silent,5,-1,"standalone port rejects tap addresses");
            }
#endif
            sio_set_multitap(0);
        }
    }
    cards_present = 0;
#ifdef _WIN32
    _putenv_s("PSX_INPUT_ROUTE_PAD_ACK_MODEL","");
#else
    unsetenv("PSX_INPUT_ROUTE_PAD_ACK_MODEL");
#endif
}
#endif

int main(void) {
    setup_bus();
    sio_set_port_device(0,SIO_DEVICE_MOUSE);
    sio_set_pad_connected(0,1);
    check(sio_get_port_device(0) == SIO_DEVICE_MOUSE,"slot 0 reports the mouse kind");
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"slot 1 stays a pad");

    /* Spec test vectors. */
    EXPECT(0,"idle mouse",                     0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);
    sio_set_mouse_buttons(0,1,0); sio_mouse_add_motion(0,5,-3);
    EXPECT(0,"left, dX +5, dY -3",             0xFF,0x12,0x5A,0xFF,0xF4,0x05,0xFD);
    sio_set_mouse_buttons(0,0,1); sio_mouse_add_motion(0,-1,1);
    EXPECT(0,"right, dX -1, dY +1",            0xFF,0x12,0x5A,0xFF,0xF8,0xFF,0x01);
    sio_set_mouse_buttons(0,1,1); sio_mouse_add_motion(0,-128,127);
    EXPECT(0,"both, dX -128, dY +127",         0xFF,0x12,0x5A,0xFF,0xF0,0x80,0x7F);
    sio_set_mouse_buttons(0,0,0);
    EXPECT(0,"motion is consumed by the read", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

    /* Carry past one byte (host policy): +300 reads 7F 7F 2E 00. */
    sio_mouse_add_motion(0,300,-300);
    EXPECT(0,"carry read 1", 0xFF,0x12,0x5A,0xFF,0xFC,0x7F,0x80);
    EXPECT(0,"carry read 2", 0xFF,0x12,0x5A,0xFF,0xFC,0x7F,0x80);
    EXPECT(0,"carry read 3", 0xFF,0x12,0x5A,0xFF,0xFC,0x2E,0xD4);
    EXPECT(0,"carry read 4", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

    /* The pending motion is clamped to SIO_MOUSE_ACCUM_MAX. */
    sio_mouse_add_motion(0,5000,0);
    sio_mouse_add_motion(0,2147483647,0);
    {
        int total = 0;
        for (int r=0;r<16;++r) {
            static const uint8_t tx[7] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00 };
            uint8_t rx[7], ack[7];
            txn(0,tx,7,rx,ack);
            total += (int8_t)rx[5];
        }
        check(total == SIO_MOUSE_ACCUM_MAX,"pending motion is clamped to SIO_MOUSE_ACCUM_MAX");
    }
    sio_mouse_add_motion(0,40,40);
    sio_mouse_clear_motion(0);
    EXPECT(0,"clear drops pending motion", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

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
    sio_mouse_add_motion(0,9,0);
    EXPECT(0,"a read after a probe still works", 0xFF,0x12,0x5A,0xFF,0xFC,0x09,0x00);

    /* Two-port use: digital pad on port 1, mouse on port 2 (Quake II style). */
    setup_bus();
    check(sio_get_port_device(0) == SIO_DEVICE_MOUSE,"the kind survives sio_init");
    sio_set_port_device(0,SIO_DEVICE_PAD);
    sio_set_port_device(1,SIO_DEVICE_MOUSE);
    sio_set_pad_connected(0,1); sio_set_pad_connected(1,1);
    sio_set_pad_config_capable(0,0);
    sio_set_pad_state_slot(0,0xFFEF);
    sio_mouse_add_motion(1,-2,4); sio_set_mouse_buttons(1,1,0);
    sio_mouse_add_motion(0,50,50);   /* ignored: slot 0 is a pad */
    sio_set_mouse_buttons(0,1,1);    /* ignored */
    {
        static const uint8_t tx[5] = { 0x01,0x42,0x00,0x00,0x00 };
        uint8_t rx[5], ack[5];
        txn(0,tx,5,rx,ack);
        check(rx[1] == 0x41 && rx[2] == 0x5A && rx[3] == 0xEF && rx[4] == 0xFF,
              "port 1 pad answers 41 5A EF FF");
        check(ack[3] == 1 && ack[4] == 0,"pad /ACK pattern is unchanged");
    }
    EXPECT(1,"port 2 mouse", 0xFF,0x12,0x5A,0xFF,0xF4,0xFE,0x04);

    /* The kind is not in the snapshot wire. */
    {
        const uint32_t with_mouse = sio_snapshot_bytes();
        sio_set_port_device(1,SIO_DEVICE_PAD);
        check(sio_snapshot_bytes() == with_mouse,"snapshot size does not depend on the kind");
    }

    /* Back to a pad: the port answers as a pad again, with no mouse residue. */
    sio_set_port_device(1,SIO_DEVICE_MOUSE);
    sio_mouse_add_motion(1,3,3);
    sio_set_port_device(1,SIO_DEVICE_PAD);
    sio_set_port_device(1,SIO_DEVICE_MOUSE);
    EXPECT(1,"switching kinds clears motion", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

    /* Disconnected mouse: nothing answers. */
    sio_set_pad_connected(1,0);
    sio_mouse_add_motion(1,7,7);
    {
        static const uint8_t tx[3] = { 0x01,0x42,0x00 };
        uint8_t rx[3], ack[3];
        txn(1,tx,3,rx,ack);
        check(rx[1] == 0xFF && ack[0] == 0,"a disconnected mouse port is silent");
    }
    sio_set_pad_connected(1,1);
    EXPECT(1,"motion waits for the next read", 0xFF,0x12,0x5A,0xFF,0xFC,0x07,0x07);

    /* Netplay canonicalization turns every seat back into a pad. */
    sio_netplay_canonicalize_session_pads(2);
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"netplay seats are pads");

#if PSX_MAX_PLAYERS >= 5
    method2_controls();

    /* Multitap on port 1, mouse on seat B: its 8-byte block in a bulk read is
     * 12 5A FF <buttons> dX dY FF FF (PSX-SPX multitap method 1). */
    setup_bus();
    sio_set_multitap_port(0); sio_set_multitap(1);
    for (int s=0;s<4;++s) {
        sio_set_port_device(s,s == 1 ? SIO_DEVICE_MOUSE : SIO_DEVICE_PAD);
        sio_set_pad_connected(s,1);
        sio_set_pad_config_capable(s,0);
    }
    sio_mouse_add_motion(1,-6,2); sio_set_mouse_buttons(1,0,1);
    {
        static const uint8_t req[5] = { 0x01,0x42,0x01,0x00,0x00 };
        uint8_t tx[35] = { 0x01,0x42,0x00 };
        uint8_t rx[35], ack[35];
        txn(0,req,5,rx,ack);
        txn(0,tx,35,rx,ack);
        check(rx[1] == 0x80 && rx[2] == 0x5A,"bulk read has the multitap ID");
        static const uint8_t seat_a[8] = { 0x41,0x5A,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
        static const uint8_t seat_b[8] = { 0x12,0x5A,0xFF,0xF8,0xFA,0x02,0xFF,0xFF };
        check(!memcmp(&rx[3],seat_a,8),"seat A is a digital pad");
        check(!memcmp(&rx[11],seat_b,8),"seat B is the mouse, padded with FF FF");
    }
    sio_set_multitap(0);
#endif

    printf("sio_mouse: %u checks passed (players=%d)\n",checks,PSX_MAX_PLAYERS);
    return 0;
}
