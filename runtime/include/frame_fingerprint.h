#ifndef FRAME_FINGERPRINT_H
#define FRAME_FINGERPRINT_H

/* Per-frame guest-write fingerprint accumulator behind the debug server's
 * `frame_fingerprint` command (debug_server.c snapshots it once per frame).
 * Header-only so the hash semantics are unit-tested without the server
 * (runtime/tests/test_frame_fingerprint.c).
 *
 * Two kinds of column, because an A/B run needs both:
 *
 *   JUDGE columns say whether two runs did the same guest-visible things:
 *     wcount + wsum   main RAM: count and ORDER-INDEPENDENT sum of (phys,val)
 *     sp_*, mmio_*    scratchpad / device registers: CPU-only traffic, so the
 *                     ordered hash is program order (includes the store PC)
 *     quiet_count     writes FMV-quiet kept out of every column
 *
 *   LOCATOR columns help find WHERE two runs parted, but can differ while
 *   guest state is identical:
 *     wr_hash, pc_hash  ORDERED rolling hashes over main RAM. DMA/device
 *                       writes (MDEC-out, CD, SPU, GPU->RAM) land in host
 *                       device-service order, which depends on how often a
 *                       backend flushes cycles, so the same multiset of writes
 *                       can arrive interleaved differently with CPU stores.
 *                       A device write also carries the last CPU store PC.
 */

#include <stdint.h>

#define PSX_FP_FNV_SEED  1469598103934665603ULL
#define PSX_FP_FNV_PRIME 1099511628211ULL

#define PSX_FP_SCRATCH_LO 0x1F800000u
#define PSX_FP_SCRATCH_HI 0x1F8003FFu

typedef struct PsxFrameFingerprint {
    uint64_t wr_hash;     /* locator: ordered (phys,val), main RAM            */
    uint64_t pc_hash;     /* locator: ordered store PC, main RAM              */
    uint64_t wcount;      /* judge: main-RAM write count                      */
    uint64_t wsum;        /* judge: order-independent sum over main RAM       */
    uint64_t mmio_hash;   /* judge: ordered (addr,val,pc), device registers   */
    uint64_t mmio_count;
    uint64_t sp_hash;     /* judge: ordered (phys,val,pc), scratchpad         */
    uint64_t sp_count;
    uint64_t quiet_count; /* judge: fingerprint-eligible writes FMV-quiet skipped */
} PsxFrameFingerprint;

#define PSX_FRAME_FINGERPRINT_INIT                                  \
    { PSX_FP_FNV_SEED, PSX_FP_FNV_SEED, 0, 0, PSX_FP_FNV_SEED, 0,   \
      PSX_FP_FNV_SEED, 0, 0 }

/* One write's contribution to wsum: the splitmix64 finalizer of (phys,val).
 * It is a bijection on the 64-bit key, so distinct (phys,val) pairs never
 * share a term, and a value moved between two addresses changes the sum.
 * Summing (mod 2^64) makes the column blind to order only. */
static inline uint64_t psx_fp_write_term(uint32_t phys, uint32_t val)
{
    uint64_t z = (((uint64_t)phys << 32) | val) + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Main RAM (phys < ram_live_bytes) and scratchpad feed the fingerprint;
 * anything else a RAM write path reports is ignored. */
static inline int psx_fp_write_eligible(uint32_t phys, uint32_t ram_live_bytes)
{
    return phys < ram_live_bytes ||
           (phys >= PSX_FP_SCRATCH_LO && phys <= PSX_FP_SCRATCH_HI);
}

static inline void psx_fp_record_write(PsxFrameFingerprint *fp, uint32_t phys,
                                       uint32_t val, uint32_t pc,
                                       uint32_t ram_live_bytes)
{
    if (phys >= PSX_FP_SCRATCH_LO && phys <= PSX_FP_SCRATCH_HI) {
        uint64_t s = fp->sp_hash;
        s = (s ^ (uint64_t)phys) * PSX_FP_FNV_PRIME;
        s = (s ^ (uint64_t)val)  * PSX_FP_FNV_PRIME;
        s = (s ^ (uint64_t)pc)   * PSX_FP_FNV_PRIME;
        fp->sp_hash = s;
        fp->sp_count++;
        return;
    }
    if (phys >= ram_live_bytes) return;
    uint64_t h = fp->wr_hash;
    h = (h ^ (uint64_t)phys) * PSX_FP_FNV_PRIME;
    h = (h ^ (uint64_t)val)  * PSX_FP_FNV_PRIME;
    fp->wr_hash = h;
    fp->pc_hash = (fp->pc_hash ^ (uint64_t)pc) * PSX_FP_FNV_PRIME;
    fp->wcount++;
    fp->wsum += psx_fp_write_term(phys, val);
}

static inline void psx_fp_record_mmio(PsxFrameFingerprint *fp, uint32_t addr,
                                      uint32_t val, uint32_t pc)
{
    uint64_t h = fp->mmio_hash;
    h = (h ^ (uint64_t)addr) * PSX_FP_FNV_PRIME;
    h = (h ^ (uint64_t)val)  * PSX_FP_FNV_PRIME;
    h = (h ^ (uint64_t)pc)   * PSX_FP_FNV_PRIME;
    fp->mmio_hash = h;
    fp->mmio_count++;
}

/* A write FMV-quiet kept out of the fingerprint. Counting it makes the
 * suppression visible: two runs whose quiet_count differs dropped different
 * writes, so their cumulative columns stop being comparable from there on. */
static inline void psx_fp_note_quiet(PsxFrameFingerprint *fp)
{
    fp->quiet_count++;
}

#endif /* FRAME_FINGERPRINT_H */
