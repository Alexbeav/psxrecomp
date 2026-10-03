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
 *   inert   print the release entry points with nothing armed
 *   pictures <route> <frames> [fault]        release only: the frame loop of a
 *                                              release product with a route, for
 *                                              the route pictures (PS1B-404) */
#include "input_route_session.h"
#include "disc_digest_cache.h"
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
static int analog[PSX_MAX_PLAYERS];
static uint8_t sticks[PSX_MAX_PLAYERS][4];
void sio_set_pad_sticks(int slot, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
{ sticks[slot][0] = lx; sticks[slot][1] = ly; sticks[slot][2] = rx; sticks[slot][3] = ry; }
void sio_set_pad_analog(int slot, int enabled, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{ analog[slot] = enabled; sio_set_pad_sticks(slot, a, b, c, d); }
void sio_set_pad_state_slot(int slot, uint16_t buttons) { pad[slot] = buttons; }

#ifdef PSX_NO_DEBUG_TOOLS
/* A release product links the route observer and drives it from the release
 * replay when PSX_INPUT_ROUTE_CAPTURE_DIR is set (PS1B-404). The machine the
 * observer reads is stood in for here: a 4x2 display whose pixels carry the
 * frame counter, so each picture names the boundary it was taken at. */
#include "debug_server.h"
#include "gpu.h"
#include "cdrom.h"
#include "dma.h"
uint64_t s_frame_count;
CPUState *debug_cpu_ptr = NULL;
uint32_t i_stat, i_mask;
int sio_get_multitap(void) { return multitap > 0; }
int sio_get_pad_connected(int slot) { return connected[slot]; }
int sio_get_pad_config_capable(int slot) { return config_capable[slot]; }
int sio_get_pad_analog(int slot) { return analog[slot]; }
uint16_t sio_get_pad_buttons_slot(int slot) { return pad[slot]; }
void sio_get_pad_sticks(int slot, uint8_t out[4]) { memcpy(out, sticks[slot], 4); }
uint32_t sio_get_trace(const SioTraceEntry **out, int *index) { *out = NULL; *index = 0; return 0; }
void psx_crash_trace_set_exit_origin(const char *origin) { printf("exit_origin=%s\n", origin); }
void gpu_get_display_info(GpuDisplayInfo *out)
{ memset(out, 0, sizeof(*out)); out->width = 4; out->height = 2; }
void gpu_display_pixel_rgb(const GpuDisplayInfo *di, uint32_t x, uint32_t y,
                           uint8_t *r, uint8_t *g, uint8_t *b)
{ (void)di; *r = (uint8_t)s_frame_count; *g = (uint8_t)(x * 16u + y); *b = (uint8_t)pad[0]; }
void gl_renderer_sync_cpu(void) {}
void vk_renderer_sync_cpu(void) {}
void gpu_observer_video_state(uint32_t *out) { memset(out, 0, 5 * sizeof(*out)); }
void interrupts_observer_field_state(uint32_t *out) { memset(out, 0, 3 * sizeof(*out)); }
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3], uint16_t target[3],
                         int32_t irq_line[3], uint32_t frac[3])
{ (void)counter; (void)mode; (void)target; (void)irq_line; (void)frac; }
/* One card in slot 1 when PSX_TEST_CARD is set: a DualShock route declares
 * its cards, and the observer refuses a card nobody declared. */
int memcard_is_present(int slot) { return slot == 0 && getenv("PSX_TEST_CARD") != NULL; }
int memcard_debug_read_buffer(int slot, uint32_t offset, uint32_t length, uint8_t *out)
{ (void)slot; (void)offset; memset(out, 0, length); return (int)length; }
int event_ring_dump_stream(FILE *f) { fputs("[]\n", f); return 0; }
uint64_t cdrom_debug_get_command_history(const CDROMCommandHistoryEntry **out) { *out = NULL; return 0; }
uint64_t cdrom_debug_get_trace(const CDROMTraceEntry **out) { *out = NULL; return 0; }
uint64_t cdrom_debug_get_sector_history(const CDROMSectorHistoryEntry **out) { *out = NULL; return 0; }
uint64_t cdrom_timing_total(void) { return 0; }
int cdrom_timing_record(uint64_t seq, CdTimingPub *out) { (void)seq; (void)out; return 0; }
int cdrom_get_bursts(void *out, int max) { (void)out; (void)max; return 0; }
uint64_t dma_debug_get_cdrom_history(const DMACDROMHistoryEntry **out) { *out = NULL; return 0; }
void debug_server_dump_watched_writes(FILE *f, const uint32_t *a, uint32_t count)
{ (void)f; (void)a; (void)count; }
void psx_slice_diag_write(const char *dir) { (void)dir; }
#endif

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
    if (argc >= 3 && !strcmp(argv[1], "armed")) {
        /* Any admitted route, whatever its format, reports armed. */
        const int before = input_route_session_armed();
        const int ok = input_route_session_admit(argv[2]);
        printf("before=%d admitted=%d armed=%d\n", before, ok, input_route_session_armed());
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
#ifdef PSX_NO_DEBUG_TOOLS
    if (argc >= 4 && !strcmp(argv[1], "pictures")) {
        /* The vblank of a release product (main.cpp): the frame counter, the
         * route boundary, the next route word, then the word applied to SIO.
         * `fault` breaks the delivery of the third word: "wrong" delivers
         * another word, "analog" delivers it to a pad in analog mode. A
         * PSXRTI1 route declares no device, so the product's own pad setup
         * is stood in for by one connected pad. */
        const uint32_t count = (uint32_t)strtoul(argv[3], NULL, 10);
        const char *fault = argc > 4 ? argv[4] : "";
        connected[0] = 1;
        if (!input_route_session_admit(argv[2])) return 30;
        for (uint32_t frame = 0; frame < count; ++frame) {
            ++s_frame_count;
            if (input_route_session_boundary() >= 0) return 31;
            const int word = input_route_session_release_override();
            if (word < 0 || word > 0xffff) return 32;
            if (frame == 2 && !strcmp(fault, "analog")) analog[0] = 1;
            if (frame == 2 && !strcmp(fault, "wrong")) pad[0] = (uint16_t)~word;
            else if (!input_route_session_release_dualshock(word)) pad[0] = (uint16_t)word;
            guest_frame((uint16_t)word, 0);
        }
        printf("pictures: %u frames ran and the process is still here\n", (unsigned)count);
        return 8;
    }
#endif
    if (argc < 5) return 64;
    /* A player-replay digest cache may be configured in the same process; a
     * route's identity must still come from a full hash of the disc. */
    if (getenv("PSX_TEST_DISC_DIGEST_CACHE"))
        disc_digest_cache_set_path(getenv("PSX_TEST_DISC_DIGEST_CACHE"));
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
