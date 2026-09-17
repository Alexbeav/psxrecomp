/* Record -> replay driver for input_route_session.c, run by
 * test_input_route_session.py. Built twice: once as the diagnostic product
 * (recorder) and once with PSX_NO_DEBUG_TOOLS (release replay). A tiny guest
 * model stands in for the machine: every input word is folded into RAM and
 * the cycle counter, so a replay that delivers a different word, or delivers
 * it on a different frame, reaches a different checkpoint.
 *
 *   record  <route> <disc> <bios> <frames>   write a route with MENU at frame
 *                                              40 and GAMEPLAY at frame 120
 *   replay  <route> <disc> <bios> [perturb]  replay until the markers are done
 *   inert   print the release entry points with nothing armed */
#include "input_route_session.h"
#include "sio.h"
#include <stdlib.h>
#include <string.h>

static uint8_t ram[INPUT_ROUTE_RAM_PAGES * 4096u];
uint8_t *g_psx_ram = ram;
uint64_t psx_cycle_count;

static int connected[PSX_MAX_PLAYERS], config_capable[PSX_MAX_PLAYERS], multitap = -1;
static uint16_t pad[PSX_MAX_PLAYERS];
void sio_set_multitap(int enabled) { multitap = enabled; }
void sio_set_pad_connected(int slot, int value) { connected[slot] = value; }
void sio_set_pad_config_capable(int slot, int value) { config_capable[slot] = value; }
void sio_set_pad_analog(int slot, int enabled, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{ (void)slot; (void)enabled; (void)a; (void)b; (void)c; (void)d; }
void sio_set_pad_state_slot(int slot, uint16_t buttons) { pad[slot] = buttons; }
void sio_set_pad_sticks(int slot, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
{ (void)slot; (void)lx; (void)ly; (void)rx; (void)ry; }

static uint16_t player_word(uint32_t frame)
{
    if (frame < 30) return 0xffff;
    if (frame < 36) return 0xfff7;                /* Start */
    if (frame < 90) return (frame / 7) % 2 ? 0xbfff : 0xffff;
    return 0xffef;                                /* Up */
}

static void guest_frame(uint16_t word, int perturb)
{
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t a = (uint32_t)((psx_cycle_count * 2654435761u + i * 40503u) % sizeof(ram));
        ram[a] = (uint8_t)(ram[a] * 31u + (word >> (i % 16)) + i);
    }
    psx_cycle_count += 2000u + (word & 0x3fu) + (uint64_t)perturb;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "inert")) {
        printf("override=%d boundary=%d owns_ports=%d recording=%d dualshock=%d\n",
               input_route_session_release_override(), input_route_session_boundary(),
               input_route_session_owns_ports(), input_route_session_recording(),
               input_route_session_release_dualshock(0xffff));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "words")) {
        /* Release replay of any route format without identity checks. */
        const uint32_t count = (uint32_t)strtoul(argv[3], NULL, 10);
        if (!input_route_session_admit(argv[2])) return 30;
        for (uint32_t frame = 0; frame < count; ++frame)
            printf("%s%04x", frame ? "," : "", input_route_session_release_override());
        printf("\n");
        return 0;
    }
    if (argc < 5) return 64;
    input_route_session_set_product("SLUS-00662", argv[3], argv[4]);
    if (!strcmp(argv[1], "record")) {
        uint32_t frames = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 10) : 200u;
        if (!input_route_session_record_begin(argv[2])) return 10;
        if (!input_route_session_verify_identity(1, 1)) return 11;
        if (connected[0] != 1 || connected[1] != 0 || multitap != 0 || config_capable[0])
            return 12;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            if (frame == 40) input_route_session_request_marker(INPUT_ROUTE_MARKER_MENU);
            if (frame == 119) input_route_session_request_marker(INPUT_ROUTE_MARKER_GAMEPLAY);
            if (input_route_session_boundary() >= 0) return 13;
            uint16_t word = player_word(frame);
            input_route_session_record_input(word);
            guest_frame(word, 0);
        }
        char status[9000];
        input_route_session_record_status(status, sizeof(status));
        printf("status: {%s}\n", status);
        return 0; /* the route is written by the atexit handler */
    }
    if (!strcmp(argv[1], "replay")) {
        const int perturb = argc > 5 ? atoi(argv[5]) : 0;
        if (!input_route_session_admit(argv[2])) return 20;
        if (!input_route_session_verify_identity(1, 1)) return 21;
        for (uint32_t frame = 0; frame < 100000u; ++frame) {
            int status = input_route_session_boundary();
            if (status >= 0) return status;
            int word = input_route_session_release_override();
            if (word < 0 || word > 0xffff) return 22;
            if (input_route_session_owns_ports() &&
                (connected[0] != 1 || connected[1] != 0 || multitap != 0))
                return 23;
            guest_frame((uint16_t)word, frame == 100 ? perturb : 0);
        }
        return 24;
    }
    return 64;
}
