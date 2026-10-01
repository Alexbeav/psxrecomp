/* test_pgxp.cpp — PGXP value-propagation engine unit tests (docs/ENHANCEMENTS.md
 * G1.2/G1.3). White-box over runtime/src/pgxp.cpp with the gte.cpp fallback
 * cache stubbed, exercising exactly the properties the engine's safety rests
 * on: provenance roundtrips, validate-on-read, half-word semantics, the
 * repack arithmetic, the suppression bracket, and the GPU-side safeguards. */

#include "pgxp.h"
#include "pgxp_hooks.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- gte.cpp fallback-cache stub ----------------------------------------- */

static uint32_t g_fb_packed = 0;
static int32_t  g_fb_x16 = 0, g_fb_y16 = 0;
static int      g_fb_valid = 0;

extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16) {
    if (!g_fb_valid || packed != g_fb_packed) return 0;
    if (x16) *x16 = g_fb_x16;
    if (y16) *y16 = g_fb_y16;
    return 1;
}

/* ---- MIPS encodings ------------------------------------------------------ */

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh,
                      uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | funct;
}
static uint32_t enc_cop2(uint32_t sub, uint32_t rt, uint32_t rd) {
    return (0x12u << 26) | (sub << 21) | (rt << 16) | (rd << 11);
}

#define LW(rs, rt)   enc_i(0x23, rs, rt, 0)
#define SW(rs, rt)   enc_i(0x2B, rs, rt, 0)
#define LH(rs, rt)   enc_i(0x21, rs, rt, 0)
#define LHU(rs, rt)  enc_i(0x25, rs, rt, 0)
#define SH(rs, rt)   enc_i(0x29, rs, rt, 0)
#define SB(rs, rt)   enc_i(0x28, rs, rt, 0)
#define LWC2(rt)     enc_i(0x32, 1, rt, 0)
#define SWC2(rt)     enc_i(0x3A, 1, rt, 0)
#define MFC2(rt, rd) enc_cop2(0x00, rt, rd)
#define MTC2(rt, rd) enc_cop2(0x04, rt, rd)
#define ADDIU(rs, rt, imm) enc_i(0x09, rs, rt, (uint16_t)(imm))
#define LUI(rt, imm) enc_i(0x0F, 0, rt, (uint16_t)(imm))
#define SLL(rt, rd, sh) enc_r(0, rt, rd, sh, 0x00)
#define SRA(rt, rd, sh) enc_r(0, rt, rd, sh, 0x03)
#define OR(rs, rt, rd)  enc_r(rs, rt, rd, 0, 0x25)
#define ADDU(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x21)

/* One projected vertex: x = 160.5, y = 80.25 -> packed integer word. */
static const uint32_t PACKED  = (80u << 16) | 160u;
static const int32_t  X16     = (160 << 16) | 0x8000;   /* 160.5  */
static const int32_t  Y16     = (80 << 16)  | 0x4000;   /* 80.25  */
static const uint16_t SZ3     = 100;

static const uint32_t ADDR_A  = 0x80100000u;   /* packet slot A (KSEG0)  */
static const uint32_t ADDR_B  = 0x00100040u;   /* packet slot B (KUSEG)  */

static void produce_at(uint32_t addr) {
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, SWC2(14), PACKED, addr);
}

static int lookup(uint32_t addr, uint32_t word, int32_t ix, int32_t iy,
                  int32_t *x, int32_t *y, uint16_t *z) {
    int32_t lx, ly; uint16_t lz;
    int r = pgxp_get_precise_vertex(addr, word, ix, iy, &lx, &ly, &lz);
    if (x) *x = lx;
    if (y) *y = ly;
    if (z) *z = lz;
    return r;
}

int main(void) {
    pgxp_set_enabled(1);
    pgxp_set_tolerance(-1.0f);
    pgxp_set_cpu_mode(0);

    /* --- SWC2 produce -> GPU consume (the perspective-texturing spine) --- */
    produce_at(ADDR_A);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        /* mirrors resolve to the same shadow word */
        CHECK(lookup(0xA0100000u, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
    }

    /* --- DMA/untracked overwrite: value validation rejects the shadow --- */
    {
        int32_t x, y; uint16_t z;
        uint32_t other = (81u << 16) | 161u;
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_NATIVE);
        CHECK(x == (161 << 16) && y == (81 << 16) && z == 0);
    }

    /* --- LW/SW roundtrip: packet copied by the CPU keeps provenance --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
    }

    /* --- stale GPR: register changed between load and store --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, 0xDEADBEEFu);   /* r8 mutated */
    CHECK(lookup(ADDR_B, 0xDEADBEEFu, 0, 0, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- MOVE idiom (memory mode, no cpu_mode needed) --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDU(8, 0, 10), PACKED, PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- MFC2 -> SW (register transfer path) --- */
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, MFC2(9, 14), PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- LH/SH: halves travel independently, depth does not survive --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B + 2u, PACKED >> 16);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A, PACKED & 0xFFFFu);     /* X   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B, PACKED & 0xFFFFu);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
        CHECK(z == 0);                       /* SH killed the vertex depth   */
    }

    /* --- SB destroys the touched half only --- */
    produce_at(ADDR_B);
    psx_pgxp_store(nullptr, SB(1, 8), ADDR_B, PACKED & 0xFFu);  /* same byte */
    {
        int32_t x, y; uint16_t z;
        /* low half invalidated -> not a full XY hit anymore */
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) != PGXP_SRC_DATAFLOW);
    }

    /* --- cpu-mode repack: lhu / sll 16 / or (the classic vertex build) --- */
    pgxp_set_cpu_mode(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_load(nullptr, LHU(1, 10), ADDR_A, PACKED & 0xFFFFu);    /* X   */
    psx_pgxp_alu(nullptr, OR(9, 10, 11), PACKED,
                 (PACKED >> 16) << 16, PACKED & 0xFFFFu);
    psx_pgxp_store(nullptr, SW(1, 11), ADDR_B, PACKED);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
    }

    /* --- cpu-mode addiu: fraction rides an integer offset (incl. -N) --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, 4), PACKED + 4u, PACKED, 4u);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED + 4u);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED + 4u, 164, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 + (4 << 16) && y == Y16);
    }
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, (uint16_t)-4), PACKED - 4u, PACKED,
                 (uint32_t)(int32_t)-4);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED - 4u);
    {
        int32_t x;
        CHECK(lookup(ADDR_B, PACKED - 4u, 156, 80, &x, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 - (4 << 16));
    }
    pgxp_set_cpu_mode(0);

    /* --- cpu-mode OFF: the same repack must degrade to native, cleanly --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, (PACKED >> 16) << 16);
    CHECK(lookup(ADDR_B, (PACKED >> 16) << 16, 0, 80, nullptr, nullptr,
                 nullptr) == PGXP_SRC_NATIVE);

    /* --- truncation agreement: integer part must match the native parse --- */
    produce_at(ADDR_A);
    CHECK(lookup(ADDR_A, PACKED, 161, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- tolerance clamp --- */
    produce_at(ADDR_A);
    pgxp_set_tolerance(0.25f);                 /* fraction is 0.5 -> reject  */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_set_tolerance(0.75f);                 /* 0.5 <= 0.75 -> accept      */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_set_tolerance(-1.0f);

    /* --- fallback tier: no address -> position cache, never a depth --- */
    g_fb_valid = 1; g_fb_packed = PACKED; g_fb_x16 = X16; g_fb_y16 = Y16;
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_FALLBACK);
        CHECK(x == X16 && y == Y16 && z == 0);
    }
    g_fb_valid = 0;

    /* --- dataflow only (G1.11): position fallback off skips the cache --- */
    g_fb_valid = 1; g_fb_packed = PACKED; g_fb_x16 = X16; g_fb_y16 = Y16;
    CHECK(pgxp_position_fallback() == 1);              /* default: unchanged */
    pgxp_set_position_fallback(0);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);
        CHECK(x == (160 << 16) && y == (80 << 16) && z == 0);
        /* a validated dataflow shadow is unaffected */
        produce_at(ADDR_A);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        /* a stale shadow no longer falls through to the cache */
        uint32_t other = (81u << 16) | 161u;
        g_fb_packed = other;
        g_fb_x16 = (161 << 16) | 0x8000;
        g_fb_y16 = (81 << 16) | 0x4000;
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_NATIVE);
        pgxp_set_position_fallback(1);
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_FALLBACK);
    }
    g_fb_valid = 0;

    /* --- preserve projection (G1.11): the agreement window ---------------- */
    {
        CHECK(pgxp_preserve_projection() == 0);        /* default: unchanged */
        auto seed = [](int32_t x16, int32_t y16, uint32_t packed) {
            pgxp_gte_push_sxy(x16, y16, SZ3, packed);
            psx_pgxp_cop2(nullptr, SWC2(14), packed, ADDR_A);
        };
        const int32_t half = 1 << 15;
        /* exact projection half a pixel BELOW the guest integer: the IR path
         * can never produce that, so off rejects it and on accepts it */
        seed((160 << 16) - half, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        pgxp_set_preserve_projection(1);
        {
            int32_t x, y;
            CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, nullptr) ==
                  PGXP_SRC_DATAFLOW);
            CHECK(x == (160 << 16) - half && y == Y16);
        }
        /* window edges: (-BELOW, +ABOVE) px exclusive */
        const int32_t hi = PGXP_PPP_AGREE_ABOVE, lo = PGXP_PPP_AGREE_BELOW;
        seed(((160 + hi) << 16) - 1, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        seed((160 + hi) << 16, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        seed(((160 - lo) << 16) + 1, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        seed((160 - lo) << 16, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        seed(X16, (80 + hi) << 16, PACKED);            /* Y axis too      */
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* a half beyond the GTE range, which the GPU's 11-bit parse wraps
         * (a CPU-modified word), needs exact agreement: 0x0402 parses as
         * -1022, a shadow at 1026.5 lies 2048 px away and one at -1021.5
         * truncates to -1022 */
        const uint32_t wrap = (80u << 16) | 0x0402u;
        seed((1026 << 16) + half, Y16, wrap);
        CHECK(lookup(ADDR_A, wrap, -1022, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        seed(-(1022 << 16) + half, Y16, wrap);
        CHECK(lookup(ADDR_A, wrap, -1022, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        seed(-(1022 << 16) - half, Y16, wrap);   /* in the window, not exact */
        CHECK(lookup(ADDR_A, wrap, -1022, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* an 11-bit wrapped coordinate is far outside any window */
        seed(X16 + (2048 << 16), Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* at the GTE saturation limit the guest integer is a clamp: the
         * window does not apply, agreement must be exact */
        const uint32_t sat = (80u << 16) | 0x3FFu;   /* SX saturated at 1023 */
        seed((1023 << 16) + half, Y16, sat);
        CHECK(lookup(ADDR_A, sat, 1023, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);                    /* exact agreement ok */
        seed((1024 << 16) + half, Y16, sat);
        CHECK(lookup(ADDR_A, sat, 1023, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        const uint32_t sat_lo = (0xFC00u << 16) | 160u;  /* SY at -1024 */
        seed(X16, -(1024 << 16) - half, sat_lo);
        CHECK(lookup(ADDR_A, sat_lo, 160, -1024, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* the tolerance clamp measures the offset both ways */
        seed((160 << 16) - half, Y16, PACKED);
        pgxp_set_tolerance(0.25f);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        pgxp_set_tolerance(0.75f);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        pgxp_set_tolerance(-1.0f);
        pgxp_set_preserve_projection(0);
    }

    /* --- pgxp_project_precise: the exact projection and its qualifiers --- */
    {
        /* V = (100, -50, 1000) through the identity: MAC = V * 4096 (sf=1),
         * IR = V, SZ3 = 1000, H = 300, OFX/OFY = (160, 120) in 16.16. The
         * exact screen position is (160 + 100 * 0.3, 120 - 50 * 0.3). */
        const int64_t m1 = 100 * 4096 + 1234;   /* fractional MAC bits     */
        const int64_t m2 = -50 * 4096 + 777;
        const int64_t m3 = 1000 * 4096 + 2048;
        const int32_t ofx = 160 << 16, ofy = 120 << 16;
        int32_t x, y;
        CHECK(pgxp_project_precise(m1, m2, m3, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), (uint32_t)(m3 >> 12),
                                   300, ofx, ofy, 1, 1, &x, &y) == 1);
        const double ex = 160.0 + (double)m1 * 300.0 / (double)m3;
        const double ey = 120.0 + (double)m2 * 300.0 / (double)m3;
        CHECK(x == (int32_t)std::floor(ex * 65536.0));
        CHECK(y == (int32_t)std::floor(ey * 65536.0));
        /* the horizontal widescreen factor is applied to X only */
        int32_t xs, ys;
        CHECK(pgxp_project_precise(m1, m2, m3, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), (uint32_t)(m3 >> 12),
                                   300, ofx, ofy, 3, 4, &xs, &ys) == 1);
        CHECK(ys == y);
        CHECK(xs == (int32_t)std::floor((160.0 + (double)m1 * 300.0 /
                                         (double)m3 * 0.75) * 65536.0));
        /* disqualified: sf=0, a clamped IR, SZ3 0 or clamped, divide
         * overflow (H >= 2*SZ3) -- the caller keeps the IR path */
        CHECK(pgxp_project_precise(m1, m2, m3, 0, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 1000, 300, ofx, ofy,
                                   1, 1, &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, m3, 12, 0x7FFF, (int32_t)(m2 >> 12),
                                   1000, 300, ofx, ofy, 1, 1, &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, 2048, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 0, 300, ofx, ofy, 1, 1,
                                   &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, (int64_t)0x12345 << 12, 12,
                                   (int32_t)(m1 >> 12), (int32_t)(m2 >> 12),
                                   0xFFFF, 300, ofx, ofy, 1, 1, &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, 150 * 4096, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 150, 300, ofx, ofy, 1, 1,
                                   &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, 151 * 4096, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 151, 300, ofx, ofy, 1, 1,
                                   &x, &y) == 1);
    }

    /* --- triangle census (G1.1 crack exposure) --- */
    {
        PGXPStats a, b;
        pgxp_get_stats(&a);
        pgxp_note_triangle(3);
        pgxp_note_triangle(2);
        pgxp_note_triangle(1);
        pgxp_note_triangle(0);
        pgxp_get_stats(&b);
        CHECK(b.tri_precise == a.tri_precise + 1);
        CHECK(b.tri_mixed == a.tri_mixed + 2);
        CHECK(b.tri_native == a.tri_native + 1);
    }

    /* --- suppression bracket: nothing records inside it --- */
    pgxp_invalidate_all();
    pgxp_suppress_begin();
    produce_at(ADDR_A);
    pgxp_suppress_end();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- deferred invalidate inside the bracket --- */
    produce_at(ADDR_A);
    pgxp_suppress_begin();
    pgxp_invalidate_all();                     /* deferred                   */
    pgxp_suppress_end();                       /* applies here               */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- invalidate-all + generation wrap --- */
    produce_at(ADDR_A);
    pgxp_invalidate_all();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_test_set_generation(0xFFFFFFFFu);
    pgxp_invalidate_all();
    CHECK(pgxp_test_generation() == 1u);

    /* --- test accessors mirror the SXY FIFO shadows --- */
    pgxp_test_seed_gte_sxy(2, PACKED, X16, Y16, SZ3, 1);
    {
        uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(valid && packed == PACKED && x == X16 && y == Y16 && z == SZ3);
        pgxp_test_seed_gte_sxy(2, 0, 0, 0, 0, 0);
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(!valid);
    }

    /* --- stats sanity: dataflow hits were counted --- */
    {
        PGXPStats st;
        pgxp_get_stats(&st);
        CHECK(st.lookups > 0);
        CHECK(st.dataflow_hit > 0);
        CHECK(st.native > 0);
        CHECK(st.fallback_hit > 0);
        CHECK(st.value_mismatch > 0);
    }

    if (g_failures) {
        std::fprintf(stderr, "test_pgxp: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("test_pgxp: all checks passed\n");
    return 0;
}
