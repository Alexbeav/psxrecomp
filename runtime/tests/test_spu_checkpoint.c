/* Exercise the actual CD queue consumer across an SPU snapshot restore. */
#include "../src/spu.c"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    uint32_t n = spu_snapshot_bytes();
    uint8_t *wire = malloc(n), *again = malloc(n);
    assert(wire && again);
    cd_read_pos = SPU_CD_RING_FRAMES - 3u;
    cd_frame_count = 9;
    cd_write_pos = 6;
    cd_push_frames = 1234567890123ull;
    cd_overflow_frames = 53;
    cd_underflow_frames = 27;
    for (uint32_t i = 0; i < 9; ++i) {
        uint32_t at = ((cd_read_pos + i) % SPU_CD_RING_FRAMES) * 2u;
        cd_ring[at] = (int16_t)(1234 + i);
        cd_ring[at + 1] = (int16_t)(-2345 - i);
    }
    spu_snapshot_write(wire);
    spu_cd_audio_reset();
    assert(spu_snapshot_read(wire, n));
    spu_snapshot_write(again);
    assert(memcmp(wire, again, n) == 0);
    for (uint32_t i = 0; i < 9; ++i) {
        int16_t left, right;
        assert(cd_audio_pop(&left, &right));
        assert(left == (int16_t)(1234 + i));
        assert(right == (int16_t)(-2345 - i));
    }
    { int16_t l, r; assert(!cd_audio_pop(&l, &r)); }
    memset(wire + n - 36, 0xff, 4);
    assert(!spu_snapshot_read(wire, n));
    assert(!spu_snapshot_read(NULL, n));
    free(wire); free(again);
    return 0;
}
