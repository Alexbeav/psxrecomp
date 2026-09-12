/* Real SIO timing across a checkpoint taken during an active ACK pulse. */
#define main old_ack_timing_main
#include "test_sio_ack_timing.c"
#undef main

int main(void) {
    setup(1, 1);
    pad_buttons[0] = 0x1234;
    pad_stick[0][0] = 37;
    sio_write(0x1F801040, 1);
    jump(1157); /* 27 clocks remain in the source ACK pulse. */
    uint32_t n = sio_snapshot_bytes();
    assert(n > 0);
    uint8_t *wire = malloc(n), *again = malloc(n);
    assert(wire && again);
    sio_snapshot_write(wire);
    jump(27);
    assert(!(sio_peek_stat() & 0x80));
    setup(1, 1);
    clock_now = 1157;
    assert(sio_snapshot_read(wire, n));
    assert(pad_buttons[0] == 0x1234 && pad_stick[0][0] == 37);
    sio_snapshot_write(again);
    assert(!memcmp(wire, again, n));
    jump(26); assert(sio_peek_stat() & 0x80);
    jump(1); assert(!(sio_peek_stat() & 0x80));
    free(wire); free(again);
    puts("Source digital SIO checkpoint preserves inputs and active ACK deadline");
    return 0;
}
