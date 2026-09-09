/* Production SIO controller with an explicit synthetic guest clock.
 * No BIOS, retail data or disk-backed card. */
#include "../src/sio.c"
#include <stdio.h>
#include <assert.h>
uint32_t i_stat, i_mask, g_debug_current_func_addr, g_debug_last_store_pc;
static uint64_t clock_now;
static int test_unrelated_card_words;
uint64_t psx_get_cycle_count(void) { return clock_now; }
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t a) { (void)a;return 0; }
uint32_t psx_read_word(uint32_t a) {
    if(test_unrelated_card_words && (a==0x800A6C10u || a==0x800B4E30u)) return 1;
    return 0;
}
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

static void setup(int source, int irq) {
#ifdef _WIN32
    _putenv_s("PSX_INPUT_ROUTE_PAD_ACK_MODEL",source?"octoshock-2.2.2-digital":"");
#else
    setenv("PSX_INPUT_ROUTE_PAD_ACK_MODEL",source?"octoshock-2.2.2-digital":"",1);
#endif
    clock_now=0; i_stat=1u<<9;
    sio_init(); sio_connect_pad(0); sio_set_pad_config_capable(0,0);
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);
    sio_write(0x1F80104A,irq?0x1003:3);
}
static void jump(unsigned n) { clock_now+=n;sio_advance(n); }
static unsigned stat(void) { return sio_read(0x1F801044); }
int main(void) {
    /* Default negative control retains its historical externally visible timing. */
    setup(0,1); sio_write(0x1F801040,1);
    advance(1087); assert(!(stat()&2));advance(1);assert(stat()&2);
    advance(169);assert(!(stat()&0x80));advance(1);
    advance(1000);assert(sio_peek_stat()&0x80);
    assert(stat()&0x80);assert(stat()&0x80);assert(!(stat()&0x80));
    assert((i_stat & ((1u<<9)|0x80))==((1u<<9)|0x80));
    assert(sio_snapshot_bytes()>0);

    /* Source pulse is independent of reads and of IRQ enable. */
    for(int irq=0;irq<2;irq++) {
        setup(1,irq);sio_write(0x1F801040,1);
        advance(1088+63);assert(!(stat()&0x80));advance(1);
        assert(stat()&0x80);assert(!!(i_stat&0x80)==irq);
        for(int i=0;i<100;i++)assert(stat()&0x80);
        assert(clock_now==1152);advance(31);assert(stat()&0x80);
        advance(1);assert(!(stat()&0x80));assert(!g_sio_timing_active);
        assert(!!(stat()&0x200)==irq); /* pulse expiry does not clear IRQ latch */
        assert(i_stat&(1u<<9)); /* never consume unrelated SPU IRQ */
        assert(sio_snapshot_bytes()==0 && !sio_snapshot_read(NULL,0));
    }
    /* CTRL ACK clears only IRQ; source intentionally does not reassert it. */
    setup(1,1);sio_write(0x1F801040,1);jump(1152);
    sio_write(0x1F80104A,0x1013);assert(!(stat()&0x200));assert(stat()&0x80);
    jump(31);assert(stat()&0x80);jump(1);assert(!(stat()&0x80));
    /* A complete coarse interval crosses shift, ACK-on and ACK-off exactly. */
    setup(1,1);sio_write(0x1F801040,1);jump(1184);
    assert((stat()&0x282)==0x202 && !g_sio_timing_active);
    /* Cancellation before ACK, while ACK active, and RESET. */
    setup(1,1);sio_write(0x1F801040,1);jump(1100);sio_write(0x1F80104A,0);jump(1000);
    assert(!(stat()&0x280) && !(i_stat&0x80));
    setup(1,1);sio_write(0x1F801040,1);jump(1152);sio_write(0x1F80104A,0);
    assert(!(stat()&0x80));
    setup(1,1);sio_write(0x1F801040,1);jump(1152);sio_write(0x1F80104A,0x40);
    assert(!(stat()&0x280) && !g_sio_timing_active);
    /* Digital poll bytes and no ACK after the final response. */
    setup(1,1);
    const unsigned tx[]={1,0x42,0,0,0},rx[]={0xFF,0x41,0x5A,0xFF,0xFF};
    for(int i=0;i<5;i++) {
        sio_write(0x1F801040,tx[i]);jump(1088);
        assert(sio_read(0x1F801040)==rx[i]);jump(64);
        assert(!!(stat()&0x80)==(i<4));jump(32);
    }
    /* Absent selected slot never fabricates ACK. */
    setup(1,1);sio_write(0x1F80104A,0x3003);sio_write(0x1F801040,1);jump(2000);
    assert(!(stat()&0x280) && !(i_stat&0x80));
    puts("SIO source digital ACK: exact deadlines, read-independent pulse, IRQ clear/mask, default, packet, cancellation and cold-boot guard PASS");
    return 0;
}
