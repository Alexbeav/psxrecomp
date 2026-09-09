/* Optional source-neutral observation of a preloaded digital route.
 * Boundary N is before input N+1, after N records were supplied. This does
 * not claim equivalence with another engine's frame counter. */
#include "input_route_observer.h"
#include "psx_sha256.h"
#include "psx_memory.h"
#include "gpu.h"
#include "sio.h"
#include "cdrom.h"
#include "dma.h"
#include "timers.h"
#include "event_ring.h"
#include "debug_server.h"
#include "png_write.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern uint8_t *g_psx_ram;
extern void gl_renderer_sync_cpu(void);
extern void vk_renderer_sync_cpu(void);
static FILE *log_file;
static FILE *sio_trace_file;
static FILE *watch_file;
static FILE *cpu_file;
static FILE *video_file;
extern void gpu_observer_video_state(uint32_t *out);
extern void interrupts_observer_field_state(uint32_t *out);
extern uint64_t psx_cycle_count;
extern uint32_t i_stat, i_mask;
static uint32_t watch_u16[32];
static unsigned watch_count;
static uint32_t trace_seen;
static const char *output_dir;
static uint32_t total_inputs;
static uint32_t tail_ticks;
static uint32_t capture_every = 300;
static psx_sha256_ctx input_hash;
static psx_sha256_ctx delivered_hash;
static uint32_t delivered_inputs;
static uint16_t pending_word;
static int pending;
static int first_non_neutral_seen;

static void fail(const char *message) {
    fprintf(stderr, "input route observation failed: %s\n", message);
    exit(3);
}
static FILE *open_output(const char *name) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", output_dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) fail("output path too long");
    /* New output directories belong to the launcher; never overwrite evidence. */
    /* MSVCRT does not implement C11 fopen's x modifier. O_EXCL retains
     * atomic no-overwrite semantics on both Windows and POSIX. */
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
    FILE *f = fd < 0 ? NULL : _fdopen(fd, "wb");
    if (!f && fd >= 0) _close(fd);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    if (!f && fd >= 0) close(fd);
#endif
    if (!f) { fprintf(stderr, "output %s: %s\n", path, strerror(errno));
              fail("cannot create new output"); }
    return f;
}
static void hex_hash(const uint8_t bytes[32], char text[65]) {
    static const char h[] = "0123456789abcdef";
    for (unsigned i=0; i<32; ++i) {
        text[i*2] = h[bytes[i] >> 4]; text[i*2+1] = h[bytes[i] & 15];
    }
    text[64] = 0;
}
FILE *input_route_observer_output(const char *name) { return open_output(name); }
static void capture_cd_metadata(void) {
    const CDROMCommandHistoryEntry *commands;
    const CDROMSectorHistoryEntry *sectors;
    uint64_t total = cdrom_debug_get_command_history(&commands);
    FILE *f = open_output("cd-commands.jsonl");
    uint64_t start = total > CDROM_COMMAND_HISTORY_CAP ? total - CDROM_COMMAND_HISTORY_CAP : 0;
    for (uint64_t i = start; i < total; ++i) {
        const CDROMCommandHistoryEntry *e = &commands[i % CDROM_COMMAND_HISTORY_CAP];
        if (e->seq != i) fail("CD command history sequence mismatch");
        fprintf(f, "{\"seq\":%llu,\"frame\":%u,\"cycle\":%llu,\"kind\":%u,\"cmd\":%u,\"mode\":%u,\"pc\":%u,\"func\":%u,\"stat\":%u,\"reading\":%u,\"read_msf\":[%u,%u,%u],\"params\":[",
            (unsigned long long)i, e->frame, (unsigned long long)e->cycle, e->kind, e->cmd, e->mode,
            e->pc, e->func, e->stat, e->reading, e->read_min, e->read_sec, e->read_sect);
        for (unsigned j = 0; j < e->param_count; ++j) fprintf(f, "%s%u", j ? "," : "", e->params[j]);
        fputs("]",f);
        if(e->source_clock) fprintf(f,",\"source_clock\":true,\"random_cursor\":%u,\"random_calls\":%u",
            e->source_random_cursor,e->source_random_calls);
        fputs("}\n", f);
    }
    if (ferror(f) || fclose(f)) fail("CD command metadata write");
    total = cdrom_debug_get_sector_history(&sectors);
    f = open_output("cd-sectors.jsonl");
    start = total > CDROM_SECTOR_HISTORY_CAP ? total - CDROM_SECTOR_HISTORY_CAP : 0;
    for (uint64_t i = start; i < total; ++i) {
        const CDROMSectorHistoryEntry *e = &sectors[i % CDROM_SECTOR_HISTORY_CAP];
        if (e->seq != i) fail("CD sector history sequence mismatch");
        fprintf(f, "{\"seq\":%llu,\"frame\":%u,\"lba\":%d,\"size\":%d,\"mode\":%u,\"data_delivered\":%u}\n",
            (unsigned long long)i, e->frame, e->lba, e->size, e->mode, e->data_delivered);
    }
    if (ferror(f) || fclose(f)) fail("CD sector metadata write");
    CdBurstRecord bursts[128];
    int n = cdrom_get_bursts(bursts, 128);
    f = open_output("cd-bursts.jsonl");
    for (int i = n - 1; i >= 0; --i) fprintf(f,
        "{\"start_frame\":%u,\"end_frame\":%u,\"sectors\":%u,\"divisor\":%u}\n",
        bursts[i].start_frame, bursts[i].end_frame, bursts[i].sectors, bursts[i].divisor);
    if (ferror(f) || fclose(f)) fail("CD burst metadata write");
    const DMACDROMHistoryEntry *dma;
    total = dma_debug_get_cdrom_history(&dma);
    start = total > DMA_CDROM_HISTORY_CAP ? total - DMA_CDROM_HISTORY_CAP : 0;
    f = open_output("cd-dma.jsonl");
    for (uint64_t i = start; i < total; ++i) {
        const DMACDROMHistoryEntry *e = &dma[i % DMA_CDROM_HISTORY_CAP];
        if (e->seq != i) fail("CD DMA history sequence mismatch");
        /* Retain transfer metadata only, never the ring's retail payload words. */
        fprintf(f,"{\"seq\":%llu,\"frame_start\":%u,\"frame_end\":%u,\"address\":%u,\"requested_words\":%u,\"moved_words\":%u,\"lba\":%d,\"sector_offset\":%d,\"completed\":%u,\"pc\":%u}\n",
            (unsigned long long)i, e->frame_start, e->frame_end, e->start_addr,
            e->requested_words,e->moved_words,e->lba,e->sector_read_pos_start,e->completed,e->pc);
    }
    if (ferror(f) || fclose(f)) fail("CD DMA metadata write");
    total = cdrom_timing_total();
    start = total > 4096 ? total - 4096 : 0;
    f = open_output("cd-timing.jsonl");
    for (uint64_t i = start; i < total; ++i) {
        CdTimingPub e;
        if (!cdrom_timing_record(i, &e) || e.seq != i) fail("CD timing history sequence mismatch");
        fprintf(f,"{\"seq\":%llu,\"frame\":%u,\"lba\":%d,\"due_cycle\":%llu,\"buffer_cycle\":%llu,\"irq_arm_cycle\":%llu,\"intc_cycle\":%llu,\"flags\":%u}\n",
            (unsigned long long)i,e.frame,e.lba,(unsigned long long)e.due_cycle,
            (unsigned long long)e.buffer_cycle,(unsigned long long)e.irq_arm_cycle,
            (unsigned long long)e.intc_cycle,e.flags);
    }
    if (ferror(f) || fclose(f)) fail("CD timing metadata write");
    f = open_output("device-events.json");
    if (event_ring_dump_stream(f) < 0 || fclose(f)) fail("device event metadata write");
    const CDROMTraceEntry *registers;
    total = cdrom_debug_get_trace(&registers);
    start = total > CDROM_TRACE_CAP ? total - CDROM_TRACE_CAP : 0;
    f = open_output("cd-registers.jsonl");
    for (uint64_t i = start; i < total; ++i) {
        const CDROMTraceEntry *e = &registers[i % CDROM_TRACE_CAP];
        if (e->seq != i) fail("CD register history sequence mismatch");
        /* Exclude data-port payloads; retain status, command and IRQ metadata. */
        if (e->kind == 'D' || (e->kind == 'R' && (e->addr & 3u) == 2u)) continue;
        fprintf(f,"{\"seq\":%llu,\"cycle\":%llu,\"frame\":%u,\"kind\":%u,\"addr\":%u,\"val\":%u,\"pc\":%u,\"guest_ra\":%u,\"index\":%u,\"istat\":%u,\"irq_enable\":%u,\"irq_flag\":%u,\"response_read\":%u,\"response_count\":%u,\"sector_available\":%u,\"sector_pos\":%d}\n",
            (unsigned long long)i,(unsigned long long)e->cycle,e->frame,e->kind,e->addr,e->val,
            e->pc,e->guest_ra,e->index_reg,e->i_stat,e->irq_enable,e->irq_flag,e->response_read,
            e->response_count,e->sector_available,e->sector_read_pos);
    }
    if (ferror(f) || fclose(f)) fail("CD register metadata write");
}
int input_route_observer_init(uint32_t total) {
    if (log_file || total == 0) return 0;
    output_dir = getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
    if (!output_dir) return 1;
    if (!output_dir[0]) return 0;
    total_inputs = total;
    const char *watch = getenv("PSX_INPUT_ROUTE_WATCH_U16");
    if (watch) {
        const char *p = watch;
        do {
            char *end;
            if (*p < '0' || *p > '9' || watch_count == 32) return 0;
            errno = 0;
            unsigned long address = strtoul(p, &end, 0);
            if (errno || end == p || (*end && *end != ',') ||
                address > PSX_MAIN_RAM_BYTES - 2 || (address & 1)) return 0;
            for (unsigned i = 0; i < watch_count; ++i)
                if (watch_u16[i] == address) return 0;
            watch_u16[watch_count++] = (uint32_t)address;
            p = *end ? end + 1 : end;
            if (*end && !*p) return 0;
        } while (*p);
    }
    const char *every = getenv("PSX_INPUT_ROUTE_CAPTURE_EVERY");
    if (every) {
        char *end;
        unsigned long n = strtoul(every, &end, 10);
        if (!every[0] || *end || !n || n > 10000) return 0;
        capture_every = (uint32_t)n;
    }
    const char *tail = getenv("PSX_INPUT_ROUTE_NEUTRAL_TAIL");
    if (tail) {
        char *end;
        unsigned long n = strtoul(tail, &end, 10);
        if (!tail[0] || *end || n > 60000) return 0;
        tail_ticks = (uint32_t)n;
    }
    psx_sha256_init(&input_hash);
    psx_sha256_init(&delivered_hash);
    delivered_inputs = 0;
    pending = 0;
    first_non_neutral_seen = 0;
    log_file = open_output("checkpoints.jsonl");
    const char *cpu_state = getenv("PSX_INPUT_ROUTE_CPU_STATE");
    if (cpu_state && strcmp(cpu_state, "1") == 0)
        cpu_file = open_output("cpu-state.jsonl");
    if (getenv("PSX_INPUT_ROUTE_VIDEO_STATE") && strcmp(getenv("PSX_INPUT_ROUTE_VIDEO_STATE"),"1")==0)
        video_file = open_output("video-state.jsonl");
    if (watch_count) watch_file = open_output("ram-u16.jsonl");
    const char *trace = getenv("PSX_INPUT_ROUTE_TRACE");
    if (trace && strcmp(trace, "1") == 0) sio_trace_file = open_output("sio-bytes.jsonl");
    return 1;
}
void input_route_observer_input(uint16_t buttons) {
    if (!log_file) return;
    if (pending) fail("previous input was not delivered to SIO");
    pending_word = buttons;
    pending = 1;
    const uint8_t word[2] = {(uint8_t)buttons, (uint8_t)(buttons >> 8)};
    if (delivered_inputs < total_inputs)
        psx_sha256_update(&input_hash, word, sizeof(word));
    else if (buttons != 0xFFFF) fail("post-input sample was not neutral");
}
void input_route_observer_set_end(uint32_t total) {
    if (!total || total != delivered_inputs || pending) fail("invalid dynamic input end");
    total_inputs=total;
}
void input_route_observer_applied(uint16_t buttons, int connected, int analog) {
    if (!log_file) return;
    if (!pending || pending_word != buttons || !connected || analog)
        fail("SIO delivery differs from digital route");
    const uint8_t word[2] = {(uint8_t)buttons, (uint8_t)(buttons >> 8)};
    if (delivered_inputs < total_inputs)
        psx_sha256_update(&delivered_hash, word, sizeof(word));
    ++delivered_inputs;
    pending = 0;
    if (buttons != 0xFFFF && !first_non_neutral_seen) {
        fprintf(stdout, "input_route_first_non_neutral: record=%u buttons=%04X connected=%d analog=%d\n",
                (unsigned)delivered_inputs, (unsigned)buttons, connected, analog);
        fflush(stdout);
        first_non_neutral_seen = 1;
    }
}
void input_route_observer_boundary(uint32_t completed, uint64_t runtime_frame) {
    if (log_file && (pending || completed != delivered_inputs))
        fail("boundary does not follow exact SIO delivery count");
    if (video_file) {
        uint32_t gpu[5],clock[3];
        gpu_observer_video_state(gpu); interrupts_observer_field_state(clock);
        fprintf(video_file,"{\"frame\":%u,\"runtime_frame\":%llu,\"boundary\":\"before_next_input\",\"cycle\":%llu,\"display_mode\":%u,\"vertical_start\":%u,\"vertical_end\":%u,\"gpu_field\":%u,\"lcf\":%u,\"cycles_since_vblank\":%u,\"next_period\":%u,\"profile_field\":%u}\n",
                completed,(unsigned long long)runtime_frame,(unsigned long long)psx_cycle_count,
                gpu[0],gpu[1],gpu[2],gpu[3],gpu[4],clock[0],clock[1],clock[2]);
        if(ferror(video_file) || fflush(video_file))fail("video state write");
    }
    if (cpu_file) {
        const CPUState *cpu = debug_cpu_ptr;
        if (!cpu) fail("CPU state unavailable");
        uint16_t counter[3], target[3];
        uint32_t mode[3], frac[3];
        int32_t irq_line[3];
        timers_get_snapshot(counter, mode, target, irq_line, frac);
        fprintf(cpu_file, "{\"frame\":%u,\"runtime_frame\":%llu,\"boundary\":\"before_next_input\",\"pc\":%u,\"ra\":%u,\"sr\":%u,\"cause\":%u,\"epc\":%u,\"i_stat\":%u,\"i_mask\":%u,\"timers\":[",
                completed, (unsigned long long)runtime_frame, cpu->pc, cpu->gpr[31],
                cpu->cop0[12], cpu->cop0[13], cpu->cop0[14], i_stat, i_mask);
        for (unsigned i=0; i<3; ++i)
            fprintf(cpu_file, "%s{\"channel\":%u,\"counter\":%u,\"mode\":%u,\"target\":%u,\"irq_line\":%d,\"fraction\":%u}",
                    i ? "," : "", i, counter[i], mode[i], target[i], irq_line[i], frac[i]);
        fputs("]}\n", cpu_file);
        if (ferror(cpu_file) || fflush(cpu_file)) fail("CPU state write");
    }
    if (sio_trace_file) {
        const SioTraceEntry *ring;
        int next;
        uint32_t total = sio_get_trace(&ring, &next);
        if (total - trace_seen > SIO_TRACE_CAP) fail("SIO trace overwritten");
        while (trace_seen < total) {
            const SioTraceEntry *e = &ring[trace_seen % SIO_TRACE_CAP];
            if (e->seq != trace_seen) fail("SIO trace sequence mismatch");
            if (fprintf(sio_trace_file,
                "{\"boundary\":%u,\"seq\":%u,\"tx\":%u,\"rx\":%u,\"ctrl\":%u,\"dev_pre\":%u,\"dev_post\":%u}\n",
                completed, e->seq, e->tx, e->rx, e->ctrl, e->dev_pre, e->dev_post) < 0)
                fail("SIO trace write");
            ++trace_seen;
        }
        if (fflush(sio_trace_file)) fail("SIO trace flush");
    }
    if (watch_file) {
        fprintf(watch_file, "{\"frame\":%u,\"runtime_frame\":%llu,\"boundary\":\"before_next_input\",\"cpu_pc\":%u,\"cpu_ra\":%u,\"ram_u16\":{",
                completed, (unsigned long long)runtime_frame,
                debug_cpu_ptr ? debug_cpu_ptr->pc : 0,
                debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0);
        for (unsigned i = 0; i < watch_count; ++i) {
            uint32_t a = watch_u16[i];
            unsigned value = g_psx_ram[a] | ((unsigned)g_psx_ram[a + 1] << 8);
            fprintf(watch_file, "%s\"%06X\":%u", i ? "," : "", a, value);
        }
        if (fputs("}}\n", watch_file) < 0 || fflush(watch_file)) fail("RAM watch write");
    }
    if (!log_file || (completed % (completed > total_inputs && capture_every > 60 ? 60 : capture_every)
        && completed != total_inputs && completed != total_inputs + tail_ticks)) return;
    uint8_t digest[32]; char ram_hash[65], applied_hash[65], supplied_hash[65], name[80];
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (!di.depth24 && !di.disabled) {
        gl_renderer_sync_cpu(); vk_renderer_sync_cpu();
    }
    psx_sha256_compute(g_psx_ram, PSX_MAIN_RAM_BYTES, digest);
    hex_hash(digest, ram_hash);
    psx_sha256_ctx supplied = input_hash;
    psx_sha256_final(&supplied, digest); hex_hash(digest, supplied_hash);
    psx_sha256_ctx applied = delivered_hash;
    psx_sha256_final(&applied, digest); hex_hash(digest, applied_hash);
    unsigned w = di.disabled ? 1u : (unsigned)di.width;
    unsigned h = di.disabled ? 1u : (unsigned)di.height;
    if (!w || w > 640) w = 1;
    if (!h || h > 512) h = 1;
    uint8_t *rgb = (uint8_t *)calloc((size_t)w*h, 3);
    if (!rgb) fail("screenshot allocation");
    if (!di.disabled) for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w; ++x) {
        uint8_t *p = rgb + ((size_t)y*w+x)*3;
        gpu_display_pixel_rgb(&di, x, y, p, p+1, p+2);
    }
    snprintf(name, sizeof(name), "frame-%06u.png", (unsigned)completed);
    FILE *png = open_output(name);
    int ok = png_write_rgb(png, rgb, w, h);
    free(rgb);
    if (fclose(png) || !ok) fail("screenshot write");
    if (fprintf(log_file,
        "{\"frame\":%u,\"runtime_frame\":%llu,\"boundary\":\"before_next_input\","
        "\"ram_bytes\":%u,\"ram_sha256\":\"%s\",\"applied_words_sha256\":\"%s\","
        "\"supplied_words_sha256\":\"%s\",\"sio_samples\":%u,"
        "\"input_frames\":%u,\"neutral_tail_ticks\":%u,"
        "\"width\":%u,\"height\":%u,\"display_disabled\":%s}\n",
        (unsigned)completed, (unsigned long long)runtime_frame,
        (unsigned)PSX_MAIN_RAM_BYTES, ram_hash, applied_hash, supplied_hash,
        (unsigned)delivered_inputs,
        (unsigned)(completed < total_inputs ? completed : total_inputs),
        (unsigned)(completed > total_inputs ? completed - total_inputs : 0), w, h,
        di.disabled ? "true" : "false") < 0 || fflush(log_file)) fail("checkpoint write");
    if (completed == total_inputs) {
        FILE *input_end = open_output("input-end.json");
        if (fprintf(input_end,
            "{\"frame\":%u,\"applied_words_sha256\":\"%s\",\"neutral_tail_ticks_planned\":%u}\n",
            (unsigned)completed, applied_hash, (unsigned)tail_ticks) < 0 || fclose(input_end))
            fail("input end write");
    }
    if (completed == total_inputs + tail_ticks) {
        if (watch_count) {
            FILE *writes = open_output("ram-writes.jsonl");
            debug_server_dump_watched_writes(writes, watch_u16, watch_count);
            if (ferror(writes) || fclose(writes)) fail("RAM write metadata export");
        }
        if (watch_file && fclose(watch_file)) fail("RAM watch close");
        if (cpu_file && fclose(cpu_file)) fail("CPU state close");
        if (video_file && fclose(video_file)) fail("video state close");
        video_file = NULL;
        watch_file = NULL;
        if (sio_trace_file) capture_cd_metadata();
        if (sio_trace_file && fclose(sio_trace_file)) fail("SIO trace close");
        if (fclose(log_file)) fail("checkpoint close");
        log_file = NULL;
        FILE *done = open_output("complete.json");
        if (fprintf(done,
            "{\"frame\":%u,\"input_frames\":%u,\"neutral_tail_ticks\":%u,"
            "\"stop_reason\":\"declared_observation_end_before_next_sample\","
            "\"applied_words_sha256\":\"%s\",\"game_outcome\":\"unclassified\"}\n",
            (unsigned)completed, (unsigned)total_inputs, (unsigned)tail_ticks,
            applied_hash) < 0 || fclose(done)) fail("completion write");
        fprintf(stdout, "input_route_complete: frames=%u words_sha256=%s\n", completed, applied_hash);
        fflush(stdout);
        exit(0);
    }
}
