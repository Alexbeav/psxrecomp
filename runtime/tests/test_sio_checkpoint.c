/* Real SIO timing across a checkpoint taken during an active ACK pulse. */
#define main old_ack_timing_main
#include "test_sio_ack_timing.c"
#undef main

int main(void) {
    for (int profile = 1; profile <= 2; profile++) {
    setup(profile, 1);
    sio_source_card = profile == 2;
    analog_mode_locked[0] = profile == 2;
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
    setup(profile, 1);
    sio_source_card = profile == 2;
    clock_now = 1157;
    assert(sio_snapshot_read(wire, n));
    assert(pad_buttons[0] == 0x1234 && pad_stick[0][0] == 37);
    assert(analog_mode_locked[0] == (profile == 2));
    sio_snapshot_write(again);
    assert(!memcmp(wire, again, n));
    jump(26); assert(sio_peek_stat() & 0x80);
    jump(1); assert(!(sio_peek_stat() & 0x80));
    free(wire); free(again);
    }
    /* Card read/write transactions must continue from the saved byte, including
     * the source terminal state, checksum, data buffer and other slot's FSM. */
    for (int state = MC_IDLE; state <= MC_SOURCE_DONE; state++) {
        setup(2, 1); sio_source_card = 1;
        mc_state = (McState)state; mc_slot = 0; mc_sector = 19;
        mc_data_idx = 63; mc_checksum = 0x47;
        memset(mc_data, 0x37, sizeof mc_data);
        mc_slots[1].state = MC_SOURCE_DONE; mc_slots[1].flag = 8;
        uint32_t n = sio_snapshot_bytes(); uint8_t *wire = malloc(n);
        assert(wire); sio_snapshot_write(wire);
        mc_process_byte(0); uint8_t expected_rx = sio_rx_data;
        McState expected_state = mc_state;
        setup(2, 1); sio_source_card = 1;
        assert(sio_snapshot_read(wire, n));
        assert(mc_data_idx == 63 && mc_data[4] == 0x37 && mc_slots[1].state == MC_SOURCE_DONE);
        mc_process_byte(0);
        assert(sio_rx_data == expected_rx && mc_state == expected_state);
        free(wire);
    }
    puts("Source digital SIO checkpoint preserves inputs and active ACK deadline");
    return 0;
}
