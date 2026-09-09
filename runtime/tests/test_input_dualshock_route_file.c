/* Authored wire-format cases, with no retail movie or game data. */
#include "input_dualshock_route_file.h"
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void check(int okay, const char *what)
{
    if (!okay) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}
static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
static FILE *wire(uint32_t frames, int fault, int alternating_axis)
{
    unsigned char h[24] = {0}, r[12] = {0};
    FILE *f = tmpfile();
    check(f != NULL, "temporary wire file");
    memcpy(h, "PSXRTI2\0", 8);
    put32(h + 8, 2); put32(h + 12, 12); put32(h + 16, frames);
    if (fault == 1) h[6] = '1';
    if (fault == 2) h[8] = 1;
    if (fault == 3) h[12] = 8;
    if (fault == 4) h[20] = 1;
    if (fault == 5) put32(h + 16, INPUT_ROUTE_MAX_FRAMES + 1);
    fwrite(h, 1, fault == 6 ? 23 : 24, f);
    if (fault == 6) { rewind(f); return f; }
    for (uint32_t i = 1; i <= frames; ++i) {
        put32(r, i);
        r[4] = 0xF7; r[5] = 0xFF;
        r[6] = alternating_axis ? (unsigned char)(i & 1) : 1;
        r[7] = 0; r[8] = 128; r[9] = 255; r[10] = 0; r[11] = 0;
        if (i == frames && fault == 7) put32(r, i + 1);
        if (i == frames && fault == 8) r[10] = 2;
        if (i == frames && fault == 9) r[11] = 1;
        fwrite(r, 1, i == frames && fault == 10 ? 11 : 12, f);
    }
    if (fault == 11) fputc(0, f);
    rewind(f);
    return f;
}
int main(int argc, char **argv)
{
    InputDualShockRouteStep steps[INPUT_ROUTE_MAX_STEPS];
    InputRouteStep old_steps[INPUT_ROUTE_MAX_STEPS];
    uint32_t count, frames;
    if (argc == 2) {
        FILE *input = fopen(argv[1], "rb");
        check(input != NULL, "open controller route");
        check(input_dualshock_route_read(input, steps, &count, &frames) == NULL,
              "decode complete controller route");
        check(fclose(input) == 0, "close controller route");
#ifdef _WIN32
        check(_setmode(_fileno(stdout), _O_BINARY) != -1, "binary decoded output");
#endif
        for (uint32_t i = 0; i < count; ++i) {
            unsigned char row[7] = {(unsigned char)steps[i].buttons,
                                   (unsigned char)(steps[i].buttons >> 8)};
            memcpy(row + 2, steps[i].axes_ly_lx_ry_rx, 4);
            row[6] = steps[i].analog_button;
            for (uint32_t frame = 0; frame < steps[i].frames; ++frame)
                check(fwrite(row, 1, sizeof(row), stdout) == sizeof(row), "decoded row output");
        }
        check(fflush(stdout) == 0, "decoded stream flush");
        return 0;
    }
    check(argc == 1, "argument count");
    FILE *f = wire(4, 0, 0);
    check(input_dualshock_route_read(f, steps, &count, &frames) == NULL,
          "valid complete controller stream");
    fclose(f);
    check(count == 1 && frames == 4 && steps[0].frames == 4 &&
          steps[0].buttons == 0xFFF7 && steps[0].axes_ly_lx_ry_rx[0] == 1 &&
          steps[0].axes_ly_lx_ry_rx[1] == 0 && steps[0].axes_ly_lx_ry_rx[2] == 128 &&
          steps[0].axes_ly_lx_ry_rx[3] == 255 && steps[0].analog_button == 0,
          "all seven controller bytes retained with explicit axis order");
    f = wire(4, 0, 1);
    check(input_dualshock_route_read(f, steps, &count, &frames) == NULL && count == 4,
          "axis-only transitions must not merge");
    fclose(f);
    f = wire(3, 0, 0);
    check(fseek(f, 24 + 12 + 10, SEEK_SET) == 0, "seek physical button");
    fputc(1, f); rewind(f);
    check(input_dualshock_route_read(f, steps, &count, &frames) == NULL && count == 3 &&
          steps[1].analog_button == 1 && steps[2].analog_button == 0,
          "physical Analog press and release remain distinct in format");
    fclose(f);
    for (int fault = 1; fault <= 11; ++fault) {
        f = wire(3, fault, 0);
        count = frames = 99;
        check(input_dualshock_route_read(f, steps, &count, &frames) != NULL &&
              count == 0 && frames == 0, "malformed file never publishes a partial route");
        fclose(f);
    }
    f = wire(0, 0, 0);
    check(input_dualshock_route_read(f, steps, &count, &frames) != NULL, "empty route");
    fclose(f);
    f = wire(INPUT_ROUTE_MAX_STEPS, 0, 1);
    check(input_dualshock_route_read(f, steps, &count, &frames) == NULL &&
          count == INPUT_ROUTE_MAX_STEPS, "maximum step capacity");
    fclose(f);
    f = wire(INPUT_ROUTE_MAX_STEPS + 1, 0, 1);
    check(input_dualshock_route_read(f, steps, &count, &frames) != NULL &&
          count == 0 && frames == 0, "step overflow rejected");
    fclose(f);
    f = wire(INPUT_ROUTE_MAX_FRAMES, 0, 0);
    check(input_dualshock_route_read(f, steps, &count, &frames) == NULL &&
          count == 1 && frames == INPUT_ROUTE_MAX_FRAMES, "maximum frame count");
    fclose(f);
    f = wire(3, 0, 0);
    check(input_route_read(f, old_steps, &count, &frames) != NULL && count == 0 && frames == 0,
          "legacy digital reader still rejects the different format");
    fclose(f);
    puts("DualShock route format cases passed");
    return 0;
}
