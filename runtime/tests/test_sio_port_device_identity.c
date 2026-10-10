/* Byte identity of the pad and memory-card bus when no port is a mouse.
 *
 * PS1B-279 adds the PS1 Mouse as a selectable port device. The guarantee is
 * that a build with the mouse present answers every non-mouse transaction
 * exactly as the build without it: the TAS/oracle routes never select a mouse,
 * so their SIO traffic must not move by one byte.
 *
 * The script below drives a digital pad, a DualShock through its config
 * handshake and rumble map, a memory-card probe, and (at PSX_MAX_PLAYERS 5)
 * multitap bulk reads, under the default pad model and both source ACK
 * profiles. Every exchanged byte (tx, rx, and whether IRQ7 followed) and the
 * final SIO snapshot wire are hashed. The golden hashes were recorded by
 * building this file against pin F (a3e5fd892) sio.c, before the mouse
 * existed. With --print the test prints the hash instead of checking it.
 *
 * When the mouse API is present, each profile runs a second time after a port
 * was switched to the mouse, fed motion and buttons, and switched back. That
 * run must reproduce the same hashes. */
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
int memcard_is_present(int slot) { return slot == 0; }
int memcard_read_sector(int slot,int sector,uint8_t *buf) { (void)slot;for(int i=0;i<128;i++)buf[i]=(uint8_t)(sector+i);return 0; }
int memcard_write_sector(int slot,int sector,const uint8_t *buf) { (void)slot;(void)sector;(void)buf;return 0; }
void memcard_flush(int slot) { (void)slot; }

static uint64_t hash;
static unsigned bytes;
static void mix(uint8_t v) { hash ^= v; hash *= 0x100000001B3ull; }
static void advance(unsigned n) { while(n--) { clock_now++;sio_advance(1); } }

/* One transaction on a port: every byte's tx, rx and IRQ7 go into the hash. */
static void txn(int port,const uint8_t *tx,unsigned count) {
    const uint32_t slot = port ? 0x2000u : 0u;
    sio_write(0x1F80104A,0);
    sio_write(0x1F80104A,0x1003u|slot);
    for (unsigned i=0;i<count;++i) {
        sio_write(0x1F801040,tx[i]); advance(1088);
        const uint8_t rx = (uint8_t)sio_read(0x1F801040);
        advance(256);
        const uint8_t irq = (uint8_t)((i_stat >> 7) & 1u);
        mix(tx[i]); mix(rx); mix(irq); bytes++;
        sio_write(0x1F80104A,0x1013u|slot); i_stat &= ~0x80u;
    }
    sio_write(0x1F80104A,0);
    advance(2000);
}
#define TXN(port, ...) do { static const uint8_t t_[] = { __VA_ARGS__ }; txn(port,t_,sizeof t_); } while (0)

static void script(void) {
    /* Port 1: plain digital pad (SCPH-1080), not config-capable. */
    sio_set_pad_connected(0,1); sio_set_pad_config_capable(0,0);
    sio_set_pad_analog(0,0,0x80,0x80,0x80,0x80);
    /* Port 2: DualShock, analog, config-capable. */
    sio_set_pad_connected(1,1); sio_set_pad_config_capable(1,1);
    sio_set_pad_analog(1,1,0x10,0xE0,0x7F,0x81);
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);

    for (int f=0;f<4;++f) {
        sio_set_pad_state_slot(0,(uint16_t)(0xFFFFu ^ (1u << (4+f))));
        sio_set_pad_state_slot(1,(uint16_t)(0xFFFFu ^ (1u << (12+f))));
        TXN(0, 0x01,0x42,0x00,0x00,0x00);
        TXN(1, 0x01,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
    }
    /* Digital pad ignores config probes. */
    TXN(0, 0x01,0x43,0x00,0x01,0x00);
    TXN(0, 0x01,0x45,0x00,0x00,0x00);
    /* DualShock config handshake, queries, rumble map, lock, exit. */
    TXN(1, 0x01,0x43,0x00,0x01,0x00,0x00,0x00,0x00,0x00);
    TXN(1, 0x01,0x45,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
    TXN(1, 0x01,0x46,0x00,0x01,0x00,0x00,0x00,0x00,0x00);
    TXN(1, 0x01,0x47,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
    TXN(1, 0x01,0x4C,0x00,0x01,0x00,0x00,0x00,0x00,0x00);
    TXN(1, 0x01,0x4D,0x00,0x00,0x01,0xFF,0xFF,0xFF,0xFF);
    TXN(1, 0x01,0x44,0x00,0x01,0x03,0x00,0x00,0x00,0x00);
    TXN(1, 0x01,0x43,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
    TXN(1, 0x01,0x42,0x00,0x01,0xC0,0x00,0x00,0x00,0x00);
    sio_request_pad_type(1,0);
    TXN(1, 0x01,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
    /* Memory card on port 1: get ID, then read sector 3. */
    TXN(0, 0x81,0x53,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
    {
        uint8_t rd[140] = { 0x81,0x52,0x00,0x00,0x00,0x03 };
        txn(0,rd,sizeof rd);
    }
    /* Unknown address on port 2 and a disconnected port. */
    TXN(1, 0x02,0x42,0x00);
    sio_set_pad_connected(1,0);
    TXN(1, 0x01,0x42,0x00,0x00,0x00);
    sio_set_pad_connected(1,1);
    TXN(1, 0x01,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
#if PSX_MAX_PLAYERS >= 5
    /* Multitap on port 1: seats A-D, REQ latch, bulk and garbage replies. */
    sio_set_multitap_port(0); sio_set_multitap(1);
    for (int s=0;s<4;++s) {
        sio_set_pad_connected(s,1);
        sio_set_pad_config_capable(s,0);
        sio_set_pad_state_slot(s,(uint16_t)(0xFFFFu ^ (1u << (s+4))));
    }
    for (int r=0;r<3;++r) {
        TXN(0, 0x01,0x42,0x01,0x00,0x00);
        TXN(0, 0x01,0x42,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
               0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00);
    }
    TXN(0, 0x03,0x42,0x00,0x00,0x00);
    sio_set_multitap(0);
#endif
    /* Final snapshot wire. */
    {
        const uint32_t n = sio_snapshot_bytes();
        uint8_t *p = (uint8_t *)calloc(n ? n : 1, 1);
        if (n) sio_snapshot_write(p);
        mix((uint8_t)n); mix((uint8_t)(n >> 8));
        for (uint32_t i=0;i<n;++i) mix(p[i]);
        free(p);
    }
}

static void set_profile(const char *pad, const char *card) {
    static char pad_env[96], card_env[96];
    snprintf(pad_env,sizeof pad_env,"PSX_INPUT_ROUTE_PAD_ACK_MODEL=%s",pad);
    snprintf(card_env,sizeof card_env,"PSX_INPUT_ROUTE_CARD_MODEL=%s",card);
    putenv(pad_env);
    putenv(card_env);
}

typedef struct { const char *name, *pad, *card; uint64_t golden2, golden5; } Profile;

/* golden2 retains pin F a3e5fd892 runtime/src/sio.c (no mouse; sha256
 * dac9311e304c9ac0a2e613affd72559f27c5000579b41b93cd4dece6b77a874c).
 * golden5 includes the method-2 03h poll in script(), which now reaches seat C.
 * Measured against the PS1B-288 candidate, with stage 9016036bf5456708ce3dcfbe511af44c4d6c73f9
 * passing the old pin F golden as a separate control. */
static const Profile profiles[] = {
    { "default",                   "",                           "",                 0xE78B3101B495BF34ull, 0xB7FE6DB2EE9CB500ull },
    { "octoshock-2.2.2-digital",   "octoshock-2.2.2-digital",    "",                 0xD5AFDEB1F613FBA8ull, 0xA0100CD88FB64904ull },
    { "nymashock-1.29.0-dualshock","nymashock-1.29.0-dualshock", "nymashock-1.29.0", 0x11B28ADCB1A3E907ull, 0x902F7CDFD376FE8Bull },
    { "nymashock-1.32.1-dualshock","nymashock-1.32.1-dualshock", "nymashock-1.32.1", 0x11B28ADCB1A3E907ull, 0x902F7CDFD376FE8Bull },
};

static uint64_t run(const Profile *p, int touch_mouse, int touch_negcon,
                    int touch_guncon) {
    set_profile(p->pad,p->card);
    hash = 0xCBF29CE484222325ull; bytes = 0; clock_now = 0; i_stat = 0;
    sio_init();
#ifdef SIO_DEVICE_MOUSE
    if (touch_mouse) {
        /* A mouse was plugged in, moved and clicked, then replaced by the pad. */
        for (int s=0;s<PSX_MAX_PLAYERS;++s) {
            sio_set_port_device(s,SIO_DEVICE_MOUSE);
            sio_mouse_add_motion(s,300,-77);
            sio_set_mouse_buttons(s,1,1);
            sio_set_port_device(s,SIO_DEVICE_PAD);
        }
    }
#else
    (void)touch_mouse;
#endif
#ifdef SIO_DEVICE_NEGCON
    if (touch_negcon) {
        /* A neGcon was plugged in, twisted and pressed, then replaced by the pad. */
        for (int s=0;s<PSX_MAX_PLAYERS;++s) {
            sio_set_port_device(s,SIO_DEVICE_NEGCON);
            sio_set_negcon_state(s,0x0000,0x13,0xFF,0x7F,0xFF);
            sio_set_port_device(s,SIO_DEVICE_PAD);
        }
    }
#else
    (void)touch_negcon;
#endif
#ifdef SIO_DEVICE_GUNCON
    if (touch_guncon) {
        /* A GunCon was plugged in, aimed and fired, then replaced by the pad. */
        for (int s=0;s<PSX_MAX_PLAYERS;++s) {
            sio_set_port_device(s,SIO_DEVICE_GUNCON);
            sio_set_guncon_state(s,0x0000,0x0155,0x0077);
            sio_set_port_device(s,SIO_DEVICE_PAD);
        }
    }
#else
    (void)touch_guncon;
#endif
    script();
    return hash;
}

/* usage: <profile index> [--touch-mouse] [--touch-negcon] [--touch-guncon] [--print]
 * One profile per process: sio_init deliberately keeps the trace and IRQ
 * sequence counters, and the snapshot carries sio_irq_seq, so a second run in
 * the same process would not start from power-on. */
int main(int argc, char **argv) {
    const unsigned nprof = (unsigned)(sizeof profiles/sizeof profiles[0]);
    int touch = 0, touch_negcon = 0, touch_guncon = 0, print = 0;
    if (argc < 2) { fprintf(stderr,"usage: %s <0..%u> [--touch-mouse] [--touch-negcon] [--touch-guncon] [--print]\n",argv[0],nprof-1); return 2; }
    const unsigned idx = (unsigned)atoi(argv[1]);
    for (int a=2;a<argc;++a) {
        if (!strcmp(argv[a],"--touch-mouse")) touch = 1;
        else if (!strcmp(argv[a],"--touch-negcon")) touch_negcon = 1;
        else if (!strcmp(argv[a],"--touch-guncon")) touch_guncon = 1;
        else if (!strcmp(argv[a],"--print")) print = 1;
    }
    if (idx >= nprof) { fprintf(stderr,"profile index out of range\n"); return 2; }
#ifndef SIO_DEVICE_MOUSE
    if (touch) { fprintf(stderr,"--touch-mouse needs the mouse API\n"); return 2; }
#endif
#ifndef SIO_DEVICE_NEGCON
    if (touch_negcon) { fprintf(stderr,"--touch-negcon needs the neGcon API\n"); return 2; }
#endif
#ifndef SIO_DEVICE_GUNCON
    if (touch_guncon) { fprintf(stderr,"--touch-guncon needs the GunCon API\n"); return 2; }
#endif
    const Profile *p = &profiles[idx];
    const uint64_t golden = PSX_MAX_PLAYERS >= 5 ? p->golden5 : p->golden2;
    const uint64_t h = run(p,touch,touch_negcon,touch_guncon);
    const char *trip = touch ? " after a mouse round-trip"
                     : touch_negcon ? " after a neGcon round-trip"
                     : touch_guncon ? " after a GunCon round-trip" : "";
    if (print) {
        printf("players=%d profile=%s bytes=%u hash=0x%016llXull\n",
               PSX_MAX_PLAYERS,p->name,bytes,(unsigned long long)h);
        return 0;
    }
    if (h != golden) {
        fprintf(stderr,"FAIL: players=%d profile=%s%s hash %016llX, golden %016llX\n",
                PSX_MAX_PLAYERS,p->name,trip,
                (unsigned long long)h,(unsigned long long)golden);
        return 1;
    }
    printf("sio_port_device_identity: players=%d profile=%s%s: %u bytes match golden\n",
           PSX_MAX_PLAYERS,p->name,trip,bytes);
    return 0;
}
