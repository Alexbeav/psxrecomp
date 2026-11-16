/* Exercise both actual boot-state SPU sections and the wrapped CD consumer.
 * Voice/clock and CD input have separate owners in the composed wire format. */
#include "../src/spu.c"
#include <assert.h>
#include <stdlib.h>
uint64_t psx_get_cycle_count(void) { return 0; }

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
    const uint32_t cn = spu_cd_snapshot_bytes();
    uint8_t *cdwire = malloc(cn), *cdagain = malloc(cn);
    assert(cdwire && cdagain);
    spu_snapshot_write(wire);
    spu_cd_snapshot_write(cdwire);
    spu_cd_audio_reset();
    assert(spu_snapshot_read(wire, n));
    assert(spu_cd_snapshot_read(cdwire, cn));
    spu_snapshot_write(again);
    spu_cd_snapshot_write(cdagain);
    assert(memcmp(wire, again, n) == 0);
    assert(memcmp(cdwire, cdagain, cn) == 0);
    for (uint32_t i = 0; i < 9; ++i) {
        int16_t left, right;
        assert(cd_audio_pop(&left, &right));
        assert(left == (int16_t)(1234 + i));
        assert(right == (int16_t)(-2345 - i));
    }
    { int16_t l, r; assert(!cd_audio_pop(&l, &r)); }
    /* The queue is normalized on load. A malformed frame count must reject
     * without touching a previously restored queue, including its next sample. */
    assert(spu_cd_snapshot_read(cdwire, cn));
    memset(cdwire, 0xff, 4);
    assert(!spu_cd_snapshot_read(cdwire, cn));
    spu_cd_snapshot_write(cdwire);
    assert(memcmp(cdwire, cdagain, cn) == 0);
    { int16_t l, r; assert(cd_audio_pop(&l, &r)); assert(l == 1234 && r == -2345); }
    assert(!spu_cd_snapshot_read(NULL, cn));
    assert(!spu_cd_snapshot_read(cdagain, cn - 1u));
    assert(!spu_snapshot_read(wire, n - 1u));
    assert(!spu_snapshot_read(NULL, n));
    free(wire); free(again); free(cdwire); free(cdagain);
    return 0;
}
