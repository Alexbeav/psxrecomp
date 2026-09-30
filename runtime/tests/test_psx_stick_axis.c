/* One-axis stick transform for the neGcon twist.
 *
 * Steering is one-dimensional. The DualShock stick transform is radial: it
 * rescales the whole stick vector, so its X byte also depends on Y. Used for
 * the twist, pushing the stick up or down while steering moved the twist, and
 * a full diagonal stopped short of full lock. The twist now comes from
 * psx_stick_axis_to_byte, which takes X alone, so Y cannot reach it;
 * test_negcon_twist_wiring.py checks that the neGcon sampler uses it.
 *
 * This pins, for every X: the twist equals the radial reading with Y centred
 * (a stick pushed straight left or right reads as before), whatever Y the
 * stick is at; it never decreases left to right; and the ends and centre are
 * exact. It also shows the radial reading moving with Y over the same sweep,
 * which is why the twist must not use it. */
#include "../src/psx_stick.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static void check(int okay, const char *what) {
    checks++;
    if (!okay) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static uint8_t radial_x(int x, int y, int deadzone, int anti_deadzone) {
    uint8_t bx, by;
    psx_stick_to_dualshock((int16_t)x, (int16_t)y, deadzone, anti_deadzone, &bx, &by);
    return bx;
}

static uint8_t axis(int x, int deadzone, int anti_deadzone) {
    return psx_stick_axis_to_byte((int16_t)x, deadzone, anti_deadzone);
}

int main(void) {
    /* The leak, on the runtime's default deadzone (3277). */
    check(radial_x(10000, 0, 3277, 0) != radial_x(10000, 20000, 3277, 0),
          "radial: X moves when only Y moves");
    check(radial_x(3000, 0, 3277, 0) == 0x80 && radial_x(3000, 20000, 3277, 0) != 0x80,
          "radial: Y pulls a centred X out of the deadzone");
    check(radial_x(32767, 32767, 3277, 0) < 0xFF,
          "radial: a full diagonal falls short of full X");

    static const int deadzones[] = { 0, 3277, 8000, 16000 };
    static const int anti_deadzones[] = { 0, 3000 };
    static const int ys[] = { -32768, -20000, -3000, 3000, 20000, 32767 };
    for (unsigned d = 0; d < sizeof deadzones / sizeof deadzones[0]; ++d) {
        for (unsigned a = 0; a < sizeof anti_deadzones / sizeof anti_deadzones[0]; ++a) {
            const int dz = deadzones[d], adz = anti_deadzones[a];
            int prev = -1;
            unsigned radial_moved = 0;
            for (int x = -32768; x <= 32767; ++x) {
                const uint8_t t = axis(x, dz, adz);
                const uint8_t straight = radial_x(x, 0, dz, adz);
                if (t != straight) {
                    fprintf(stderr, "FAIL: dz=%d adz=%d x=%d: twist %02X, straight radial %02X\n",
                            dz, adz, x, t, straight);
                    exit(1);
                }
                if (t < prev) {
                    fprintf(stderr, "FAIL: dz=%d adz=%d x=%d: twist %02X after %02X\n",
                            dz, adz, x, t, prev);
                    exit(1);
                }
                prev = t;
                for (unsigned k = 0; k < sizeof ys / sizeof ys[0]; ++k)
                    if (radial_x(x, ys[k], dz, adz) != straight) radial_moved++;
            }
            checks += 2;   /* straight parity and monotonic, over all 65536 X */
            check(radial_moved > 0, "over the same sweep, the radial X moves with Y");
            check(axis(-32768, dz, adz) == 0x00, "full left reads 00h");
            check(axis(0, dz, adz) == 0x80, "centre reads 80h");
            check(axis(32767, dz, adz) == 0xFF, "full right reads FFh");
            if (dz > 0)
                check(axis(dz, dz, adz) == 0x80 && axis(-dz, dz, adz) == 0x80,
                      "the deadzone edge reads centre");
        }
    }

    printf("psx_stick_axis: %u checks passed\n", checks);
    return 0;
}
