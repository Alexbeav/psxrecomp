/* frame_fingerprint.h: the judge columns (wc/ws, sp, mmio, qc) must agree for
 * two runs that performed the same guest writes, however a backend interleaved
 * device writes with CPU stores, and must still tell different writes apart.
 * The ordered wr column is the locator that is allowed to differ.
 *
 * The streams below model the R4 (SLUS-00797) case that motivated ws: a native
 * overlay shard flushes cycles at every store barrier, so MDEC-out DMA words
 * land between its CPU stores, while batched interpreted/static code services
 * the same DMA after the block. */
#include <assert.h>
#include <stdint.h>

#include "frame_fingerprint.h"

#define LIVE_RAM 0x00200000u

typedef struct { uint32_t phys, val, pc; } Write;

static void replay(PsxFrameFingerprint *fp, const Write *w, int n)
{
    for (int i = 0; i < n; i++)
        psx_fp_record_write(fp, w[i].phys, w[i].val, w[i].pc, LIVE_RAM);
}

int main(void)
{
    /* CPU stores A, B; DMA words C, D (DMA writes carry whatever the last CPU
     * store PC was when the device was serviced). */
    const Write shard[] = {
        { 0x00010000u, 1u, 0x80065800u },
        { 0x000F0000u, 3u, 0x80065800u },   /* DMA, serviced at the barrier */
        { 0x00010004u, 2u, 0x80065804u },
        { 0x000F0004u, 4u, 0x80065804u },   /* DMA */
    };
    const Write batched[] = {
        { 0x00010000u, 1u, 0x80065800u },
        { 0x00010004u, 2u, 0x80065804u },
        { 0x000F0000u, 3u, 0x80065804u },   /* DMA, serviced after the block */
        { 0x000F0004u, 4u, 0x80065804u },
    };
    PsxFrameFingerprint a = PSX_FRAME_FINGERPRINT_INIT;
    PsxFrameFingerprint b = PSX_FRAME_FINGERPRINT_INIT;
    replay(&a, shard, 4);
    replay(&b, batched, 4);
    assert(a.wcount == 4 && b.wcount == 4);
    assert(a.wsum == b.wsum);               /* judge: same guest writes */
    assert(a.wr_hash != b.wr_hash);         /* locator: service order differs */
    assert(a.pc_hash != b.pc_hash);

    /* ws still separates different writes: a changed value, and the same two
     * values swapped between two addresses. */
    {
        const Write changed[] = {
            { 0x00010000u, 1u, 0 }, { 0x00010004u, 5u, 0 },
            { 0x000F0000u, 3u, 0 }, { 0x000F0004u, 4u, 0 },
        };
        const Write swapped[] = {
            { 0x00010000u, 2u, 0 }, { 0x00010004u, 1u, 0 },
            { 0x000F0000u, 3u, 0 }, { 0x000F0004u, 4u, 0 },
        };
        PsxFrameFingerprint c = PSX_FRAME_FINGERPRINT_INIT;
        PsxFrameFingerprint d = PSX_FRAME_FINGERPRINT_INIT;
        replay(&c, changed, 4);
        replay(&d, swapped, 4);
        assert(c.wsum != b.wsum);
        assert(d.wsum != b.wsum);
    }

    /* A write that straddles a frame snapshot: one backend services it before
     * VBlank, the other after. The cumulative ws/wc differ for that one frame
     * and agree again at the next snapshot. */
    {
        PsxFrameFingerprint early = PSX_FRAME_FINGERPRINT_INIT;
        PsxFrameFingerprint late  = PSX_FRAME_FINGERPRINT_INIT;
        replay(&early, batched, 3);             /* frame N: DMA word C in   */
        replay(&late,  batched, 2);             /* frame N: DMA word C late */
        PsxFrameFingerprint snap_early = early, snap_late = late;
        assert(snap_early.wsum != snap_late.wsum);
        assert(snap_early.wcount == snap_late.wcount + 1);
        replay(&early, batched + 3, 1);         /* frame N+1 */
        replay(&late,  batched + 2, 2);
        assert(early.wsum == late.wsum && early.wcount == late.wcount);
    }

    /* FMV-quiet: a suppressed write changes nothing but qc. If the straddled
     * write above falls into a quiet frame for one run only, ws never agrees
     * again, and qc is where that shows. */
    {
        PsxFrameFingerprint recorded = PSX_FRAME_FINGERPRINT_INIT;
        PsxFrameFingerprint quiet    = PSX_FRAME_FINGERPRINT_INIT;
        replay(&recorded, batched, 4);
        replay(&quiet, batched, 2);
        replay(&quiet, batched + 3, 1);
        assert(psx_fp_write_eligible(batched[2].phys, LIVE_RAM));
        psx_fp_note_quiet(&quiet);
        assert(quiet.quiet_count == 1 && recorded.quiet_count == 0);
        assert(quiet.wsum != recorded.wsum);
        assert(quiet.wcount + quiet.quiet_count == recorded.wcount);
        /* Only writes the fingerprint would have recorded are eligible. */
        assert(psx_fp_write_eligible(0x1F800010u, LIVE_RAM));
        assert(!psx_fp_write_eligible(LIVE_RAM, LIVE_RAM));
        assert(!psx_fp_write_eligible(0x1F801810u, LIVE_RAM));
    }

    /* Scratchpad and MMIO keep their own ordered columns (program order, so the
     * store PC is part of the signature); main RAM beyond the live size is
     * ignored. */
    {
        PsxFrameFingerprint fp = PSX_FRAME_FINGERPRINT_INIT;
        const PsxFrameFingerprint seed = PSX_FRAME_FINGERPRINT_INIT;
        psx_fp_record_write(&fp, LIVE_RAM, 7u, 0x80010000u, LIVE_RAM);
        assert(fp.wcount == 0 && fp.wsum == 0 && fp.wr_hash == seed.wr_hash);
        psx_fp_record_write(&fp, 0x1F800010u, 7u, 0x80010000u, LIVE_RAM);
        assert(fp.sp_count == 1 && fp.wcount == 0 && fp.wsum == 0);
        uint64_t sp = fp.sp_hash;
        PsxFrameFingerprint other_pc = PSX_FRAME_FINGERPRINT_INIT;
        psx_fp_record_write(&other_pc, 0x1F800010u, 7u, 0x80010004u, LIVE_RAM);
        assert(other_pc.sp_hash != sp);
        psx_fp_record_mmio(&fp, 0x1F801070u, 1u, 0x80010008u);
        assert(fp.mmio_count == 1 && fp.mmio_hash != seed.mmio_hash);
    }
    return 0;
}
