/* Optional source-neutral observation of a preloaded digital route.
 * Boundary N is before input N+1, after N records were supplied. This does
 * not claim equivalence with another engine's frame counter. */
#include "input_route_observer.h"
#include "input_dualshock_delivery.h"
#include "input_route_session.h"
#include "pst_wire.h"
#include "psx_file_create_new.h"
#include "psx_sha256.h"
#include "psx_memory.h"
#include "gpu.h"
#include "sio.h"
#include "memcard.h"
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
static uint32_t terminal_frame;
static int defer_terminal;
uint32_t input_route_observer_terminal_frame(void) { return terminal_frame; }
static psx_sha256_ctx input_hash;
static psx_sha256_ctx delivered_hash;
static uint32_t delivered_inputs;
static uint32_t resumed_inputs;
static uint16_t pending_word;
static int pending;
static int first_non_neutral_seen;
static int dualshock_mode;
static psx_sha256_ctx expected_protocol_hash;
static uint8_t pending_controller[7];
static int last_protocol_mode;
/* PSXRTI3 two-digital-pad route: P2 is hashed beside P1, sample for sample. */
static int port2_mode;
static psx_sha256_ctx port2_input_hash;
static psx_sha256_ctx port2_delivered_hash;
static uint16_t pending_port2;
static int port2_pending;

/* Optional full-route audit. The original-input hash is over actual SIO
 * readback bytes and explicit source frame IDs, not requested input bytes.
 * Each audit row also names the real before-next-input runtime boundary. */
static int full_audit;
static FILE *full_audit_file;
static psx_sha256_ctx full_applied_hash, full_tail_hash, full_boundary_hash;
static uint32_t audit_source_frame, audit_written, audit_original_count, audit_tail_count;
static uint64_t audit_first_runtime, audit_last_runtime, audit_start_runtime;
static int audit_start_seen;
static uint8_t audit_last_p1[7], audit_last_p2[7];
static int checkpoint_restored;
#ifndef PSX_NO_DEBUG_TOOLS
static int checkpoint_apply(uint32_t consumed);
#endif

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
static void audit_u32(uint8_t *p, uint32_t n) {
    for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(n>>(8*i));
}
static void audit_u64(uint8_t *p, uint64_t n) {
    for (unsigned i=0;i<8;++i) p[i]=(uint8_t)(n>>(8*i));
}
/* A parsed route supplies its own absolute record number. A skipped or
 * duplicated request is rejected even when both records are neutral. */
void input_route_observer_source_frame(uint32_t frame) {
    if (!full_audit) return;
    if (audit_source_frame || frame != delivered_inputs+1u)
        fail("full audit missing/duplicate/out-of-order source frame");
    audit_source_frame=frame;
}
static void audit_applied(const uint8_t row[7]) {
    if (!full_audit) return;
    if (!audit_source_frame || audit_source_frame != delivered_inputs+1u)
        fail("full audit input has no exact source frame");
    uint8_t frame[4], port=1;
    audit_u32(frame,audit_source_frame);
    psx_sha256_ctx *hash=delivered_inputs<total_inputs ? &full_applied_hash : &full_tail_hash;
    psx_sha256_update(hash,frame,4);
    psx_sha256_update(hash,&port,1);
    psx_sha256_update(hash,row,7);
    memcpy(audit_last_p1,row,7);
    memset(audit_last_p2,0,7);
    if (input_route_neutral_p2()) {
        uint8_t st[4];sio_get_pad_sticks(1,st);
        uint16_t buttons=sio_get_pad_buttons_slot(1);
        const uint8_t p2[7]={(uint8_t)buttons,(uint8_t)(buttons>>8),st[1],st[0],st[3],st[2],(uint8_t)sio_get_pad_mode_button(1)};
        port=2;
        psx_sha256_update(hash,&port,1);
        psx_sha256_update(hash,p2,7);
        memcpy(audit_last_p2,p2,7);
    }
    if(delivered_inputs<total_inputs) ++audit_original_count; else ++audit_tail_count;
    audit_source_frame=0;
}
static void audit_boundary(uint32_t completed,uint64_t runtime_frame) {
    if (!full_audit) return;
    if (audit_source_frame) fail("full audit requested frame was never applied");
    if (!completed) {
        if (audit_written || audit_start_seen) fail("full audit boundary rewound or duplicated");
        audit_start_runtime=runtime_frame;audit_start_seen=1;
        return;
    }
    if (completed!=audit_written+1u || completed!=audit_original_count+audit_tail_count)
        fail("full audit missing/duplicate/out-of-order boundary");
    if (!audit_start_seen || runtime_frame!=audit_start_runtime+completed)
        fail("full audit application is early or late from the boot boundary");
    if (audit_written && runtime_frame!=audit_last_runtime+1u)
        fail("full audit runtime boundary shifted");
    if (!audit_written) audit_first_runtime=runtime_frame;
    audit_last_runtime=runtime_frame;
    /* Fixed 32-byte row: source frame u32,runtime boundary u64,phase=1,
     * P1/P2 device codes,reserved=0,P1/P2 readback 7 bytes,reserved[2]=0.
     * Phase1 means after application, before the next route input. */
    uint8_t r[32]={0};audit_u32(r,completed);audit_u64(r+4,runtime_frame);
    r[12]=1;r[13]=2;r[14]=input_route_neutral_p2()?2:0;
    memcpy(r+16,audit_last_p1,7);memcpy(r+23,audit_last_p2,7);
    if(fwrite(r,1,sizeof r,full_audit_file)!=sizeof r) fail("full audit row write");
    psx_sha256_update(&full_boundary_hash,r,sizeof r);
    audit_written=completed;
    if(completed%60u==0 || completed==total_inputs || completed==total_inputs+tail_ticks)
        if(fflush(full_audit_file))fail("full audit row flush");
}
static void audit_json(FILE *f) {
    if(!full_audit)return;
    uint8_t bytes[32];char original[65],tail[65],boundaries[65];
    psx_sha256_ctx h=full_applied_hash;psx_sha256_final(&h,bytes);hex_hash(bytes,original);
    h=full_tail_hash;psx_sha256_final(&h,bytes);hex_hash(bytes,tail);
    h=full_boundary_hash;psx_sha256_final(&h,bytes);hex_hash(bytes,boundaries);
    fprintf(f,",\"full_applied_input_schema\":\"TAS_SOURCE_CONTROLLER_V1\",\"full_applied_input_sha256\":\"%s\","
              "\"full_applied_original_frames\":%u,\"tail_applied_input_sha256\":\"%s\",\"tail_applied_frames\":%u,"
              "\"runtime_boundary_rows_sha256\":\"%s\",\"runtime_boundary_rows\":%u,"
              "\"first_delivery_end_runtime_frame\":%llu,\"last_delivery_end_runtime_frame\":%llu,"
              "\"missing_frames\":0,\"duplicate_frames\":0,\"out_of_order_frames\":0",
              original,audit_original_count,tail,audit_tail_count,boundaries,audit_written,
              (unsigned long long)audit_first_runtime,(unsigned long long)audit_last_runtime);
}

static void capture_initial_cards(void) {
    const char *expected[2] = {getenv("PSX_INPUT_ROUTE_CARD1_SHA256"), getenv("PSX_INPUT_ROUTE_CARD2_SHA256")};
    if (!expected[0] && !expected[1] && !dualshock_mode) return;
    int present[2]; char text[2][65] = {{0},{0}};
    for (int slot=0; slot<2; ++slot) {
        if (expected[slot]) {
            if (strlen(expected[slot]) != 64) fail("invalid initial card identity");
            for (unsigned i=0;i<64;++i)
                if (!((expected[slot][i]>='0' && expected[slot][i]<='9') ||
                      (expected[slot][i]>='a' && expected[slot][i]<='f'))) fail("invalid initial card identity");
        }
        present[slot] = memcard_is_present(slot) != 0;
        if (present[slot] != (expected[slot] != NULL)) fail("initial card presence differs");
        if (present[slot]) {
            uint8_t bytes[4096], result[32]; psx_sha256_ctx hash;
            psx_sha256_init(&hash);
            for (unsigned offset=0;offset<MEMCARD_SIZE;offset+=sizeof bytes) {
                if (memcard_debug_read_buffer(slot,offset,sizeof bytes,bytes)!=sizeof bytes)
                    fail("short initial card buffer read");
                psx_sha256_update(&hash,bytes,sizeof bytes);
            }
            psx_sha256_final(&hash,result);hex_hash(result,text[slot]);
            if (strcmp(text[slot],expected[slot])) fail("loaded initial card bytes differ");
        }
    }
    FILE *f=open_output("initial-cards.json");
    fprintf(f,"{\"card1_present\":%s,\"card2_present\":%s,\"card1_sha256\":",
            present[0] ? "true" : "false", present[1] ? "true" : "false");
    if (present[0]) fprintf(f,"\"%s\"",text[0]); else fputs("null",f);
    if (present[1]) fprintf(f,",\"card2_sha256\":\"%s\"",text[1]);
    fprintf(f,",\"bytes\":%u}\n",(unsigned)MEMCARD_SIZE*(present[0]+present[1]));
    if (ferror(f) || fclose(f)) fail("initial card observation write");
}
static const char *applied_key(void) {
    return dualshock_mode ? "applied_controller_sha256" : "applied_words_sha256";
}
static const char *supplied_key(void) {
    return dualshock_mode ? "supplied_controller_sha256" : "supplied_words_sha256";
}
static void close_observation_json(FILE *f) {
    audit_json(f);
    if (dualshock_mode) {
        if (input_route_neutral_p2()) fputs(",\"port2_controller\":\"dualshock-neutral\"", f);
        uint8_t bytes[32]; char expected[65], supplied[65];
        psx_sha256_ctx hash = expected_protocol_hash;
        psx_sha256_final(&hash, bytes); hex_hash(bytes, expected);
        hash = input_hash;
        psx_sha256_final(&hash, bytes); hex_hash(bytes, supplied);
        fprintf(f, ",\"controller_profile\":\"%s\","
                   "\"expected_protocol_sha256\":\"%s\",\"original_controller_sha256\":\"%s\","
                   "\"guest_analog_mode\":%d", input_dualshock_controller_profile(),
                   expected, supplied, last_protocol_mode);
    }
    if (port2_mode) {
        uint8_t bytes[32]; char applied[65], supplied[65];
        psx_sha256_ctx hash = port2_delivered_hash;
        psx_sha256_final(&hash, bytes); hex_hash(bytes, applied);
        hash = port2_input_hash;
        psx_sha256_final(&hash, bytes); hex_hash(bytes, supplied);
        fprintf(f, ",\"port_layout\":\"two-digital\",\"applied_port2_words_sha256\":\"%s\","
                   "\"supplied_port2_words_sha256\":\"%s\"", applied, supplied);
    }
    if (resumed_inputs) fprintf(f,",\"resumed_inputs\":%u",resumed_inputs);
    if (fputs("}\n", f) < 0 || ferror(f)) fail("observation JSON write");
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
        if (e->source_cdda_play.valid) {
            const CDROMSourceCDDAPlayDebugState *p = &e->source_cdda_play;
            fprintf(f, ",\"source_cdda_play\":{\"cycle\":%llu,\"new_seek\":%u,\"requested_track\":%u,\"motor\":%u,\"paused\":%u,\"mode\":%u,\"was_playing\":%u,\"was_seeking\":%u,\"setloc_pending\":%u,\"head_valid\":%u,\"head_lba\":%d,\"head_target_lba\":%d,\"head_due_cycle\":%llu,\"origin_lba\":%d,\"target_lba\":%d,\"lower_bound_cycles\":%d,\"jitter_cycles\":%u,\"delay_cycles\":%u,\"computed_due_cycle\":%llu,\"random_cursor_before\":%u,\"random_cursor_after\":%u,\"random_calls_before\":%u,\"random_calls_after\":%u}",
                (unsigned long long)p->cycle, p->new_seek, p->requested_track,
                p->motor, p->paused, p->mode, p->was_playing, p->was_seeking,
                p->setloc_pending, p->head_valid, p->head_lba, p->head_target_lba,
                (unsigned long long)p->head_due_cycle, p->origin_lba, p->target_lba,
                p->lower_bound_cycles, p->jitter_cycles, p->delay_cycles,
                (unsigned long long)p->computed_due_cycle, p->random_cursor_before,
                p->random_cursor_after, p->random_calls_before, p->random_calls_after);
        }
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
    capture_initial_cards();
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
                address > memory_get_ram_bytes() - 2 || (address & 1)) return 0;
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
    const char *terminal=getenv("PSX_SOURCE_TERMINAL_RETURN");
    if(terminal && strcmp(terminal,"1"))return 0;
    defer_terminal=terminal!=NULL;terminal_frame=0;
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
int input_route_observer_dualshock_init(uint32_t total) {
    if (log_file || dualshock_mode || !total) return 0;
    dualshock_mode = 1;
    psx_sha256_init(&expected_protocol_hash);
    const char *audit=getenv("PSX_INPUT_ROUTE_FULL_AUDIT");
    full_audit=audit && !strcmp(audit,"1");
    if(full_audit) {
        uint8_t devices[8]={2,0,0,0,0,0,0,0};
        devices[4]=input_route_neutral_p2()?2:0;
        static const uint8_t prefix[]="TAS_SOURCE_CONTROLLER_V1";
        static const uint8_t tail[]="TAS_TAIL_CONTROLLER_V1";
        psx_sha256_init(&full_applied_hash);psx_sha256_init(&full_tail_hash);
        psx_sha256_init(&full_boundary_hash);
        psx_sha256_update(&full_applied_hash,prefix,sizeof prefix);
        psx_sha256_update(&full_applied_hash,devices,sizeof devices);
        psx_sha256_update(&full_tail_hash,tail,sizeof tail);
        psx_sha256_update(&full_tail_hash,devices,sizeof devices);
    }
    int ok=input_route_observer_init(total);
    if(ok && full_audit) full_audit_file=open_output("applied-input-boundaries.bin");
    return ok;
}
int input_route_observer_two_digital_init(uint32_t total) {
    if (log_file || dualshock_mode || port2_mode || !total) return 0;
    port2_mode = 1;
    port2_pending = 0;
    psx_sha256_init(&port2_input_hash);
    psx_sha256_init(&port2_delivered_hash);
    return input_route_observer_init(total);
}
/* P2 of the same record as the P1 input just supplied; called before either
 * sample is applied, so delivered_inputs still counts the previous records. */
void input_route_observer_port2_input(uint16_t buttons) {
    if (!log_file) return;
    if (!port2_mode) fail("P2 input used without a two-pad route");
    if (port2_pending) fail("previous P2 input was not delivered to SIO");
    pending_port2 = buttons;
    port2_pending = 1;
    const uint8_t word[2] = {(uint8_t)buttons, (uint8_t)(buttons >> 8)};
    if (delivered_inputs < total_inputs)
        psx_sha256_update(&port2_input_hash, word, sizeof(word));
    else if (buttons != 0xFFFF) fail("post-input P2 sample was not neutral");
}
/* Called before input_route_observer_applied() for the same record. */
void input_route_observer_port2_applied(uint16_t buttons, int connected, int analog) {
    if (!log_file) return;
    if (!port2_mode) fail("P2 observation used without a two-pad route");
    if (!port2_pending || pending_port2 != buttons || !connected || analog)
        fail("SIO delivery differs from P2 route");
    const uint8_t word[2] = {(uint8_t)buttons, (uint8_t)(buttons >> 8)};
    if (delivered_inputs < total_inputs)
        psx_sha256_update(&port2_delivered_hash, word, sizeof(word));
    port2_pending = 0;
}
void input_route_observer_input(uint16_t buttons) {
    if (!log_file) return;
    if (dualshock_mode) fail("digital input used for DualShock route");
    if (pending) fail("previous input was not delivered to SIO");
    pending_word = buttons;
    pending = 1;
    const uint8_t word[2] = {(uint8_t)buttons, (uint8_t)(buttons >> 8)};
    if (delivered_inputs < total_inputs)
        psx_sha256_update(&input_hash, word, sizeof(word));
    else if (buttons != 0xFFFF) fail("post-input sample was not neutral");
}
void input_route_observer_set_end(uint32_t total) {
    if (!total || total != delivered_inputs || pending || port2_pending) fail("invalid dynamic input end");
    total_inputs=total;
}
/* TAS checkpoint resume: the delivered-input counter is observer state that
 * starts at zero in each process, while the route is seeked to an ABSOLUTE
 * return. Without this, the first post-resume boundary compares the absolute
 * consumed index against a suffix-only delivery count and fails
 * ("boundary does not follow exact SIO delivery count"). Seed the counter to
 * the resumed return and drop any half-applied sample, so the boundary
 * invariant (completed == delivered_inputs) holds from the resume point on. */
void input_route_observer_resume(uint32_t consumed) {
    if (!log_file) return;
    if (full_audit) {
#ifndef PSX_NO_DEBUG_TOOLS
        if(!checkpoint_apply(consumed))fail("full audit actual checkpoint restore failed");
#endif
        if(!checkpoint_restored || consumed!=delivered_inputs)
            fail("full audit resume requires its verified actual observer checkpoint");
        resumed_inputs=consumed;checkpoint_restored=0;
        return;
    }
    if (consumed >= total_inputs + tail_ticks) fail("resume is past the observation interval");
    delivered_inputs = consumed;
    resumed_inputs = consumed;
    pending = 0;
}
void input_route_observer_applied(uint16_t buttons, int connected, int analog) {
    if (!log_file) return;
    if (dualshock_mode) fail("digital observation used for DualShock route");
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
void input_route_observer_dualshock_input(uint16_t buttons, const uint8_t source_axes[4], uint8_t mode_button) {
    if (!log_file) return;
    if (!dualshock_mode || pending) fail("invalid DualShock sample sequence");
    uint8_t row[7] = {(uint8_t)buttons, (uint8_t)(buttons >> 8), 0,0,0,0,0};
    memcpy(row + 2, source_axes, 4);
    row[6] = mode_button;
    if (delivered_inputs < total_inputs) psx_sha256_update(&input_hash, row, sizeof(row));
    else if (buttons != 0xFFFF || source_axes[0] != 128 || source_axes[1] != 128 ||
             source_axes[2] != 128 || source_axes[3] != 128 || mode_button)
        fail("post-input DualShock sample was not neutral");
    memcpy(pending_controller, row, sizeof(row));
    for (unsigned i = 2; i < 6; ++i)
        pending_controller[i] = input_dualshock_protocol_axis(row[i]);
    if (delivered_inputs < total_inputs)
        psx_sha256_update(&expected_protocol_hash, pending_controller, sizeof(pending_controller));
    pending = 1;
}
void input_route_observer_dualshock_applied(uint16_t buttons, const uint8_t sticks[4],
                                          int connected, int config_capable, int analog) {
    if (!log_file) return;
    if (input_route_neutral_p2() && !input_route_dualshock_ports_match())
        fail("declared neutral port 2 delivery differs");
    uint8_t row[7] = {(uint8_t)buttons, (uint8_t)(buttons >> 8),
                     sticks[1], sticks[0], sticks[3], sticks[2], (uint8_t)sio_get_pad_mode_button(0)};
    if (!dualshock_mode || !pending || !connected || !config_capable ||
        (analog != 0 && analog != 1) || memcmp(row, pending_controller, sizeof(row)))
        fail("SIO delivery differs from complete DualShock route");
    if (delivered_inputs < total_inputs)
        psx_sha256_update(&delivered_hash, row, sizeof(row));
    audit_applied(row);
    last_protocol_mode = analog;
    ++delivered_inputs;
    pending = 0;
}
void input_route_observer_boundary(uint32_t completed, uint64_t runtime_frame) {
    if (log_file && (pending || port2_pending || completed != delivered_inputs))
        fail("boundary does not follow exact SIO delivery count");
    audit_boundary(completed,runtime_frame);
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
    psx_sha256_compute(g_psx_ram, memory_get_ram_bytes(), digest);
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
        "\"ram_bytes\":%u,\"ram_sha256\":\"%s\",\"%s\":\"%s\","
        "\"%s\":\"%s\",\"sio_samples\":%u,"
        "\"input_frames\":%u,\"neutral_tail_ticks\":%u,"
        "\"width\":%u,\"height\":%u,\"display_disabled\":%s",
        (unsigned)completed, (unsigned long long)runtime_frame,
        (unsigned)memory_get_ram_bytes(), ram_hash, applied_key(), applied_hash, supplied_key(), supplied_hash,
        (unsigned)delivered_inputs,
        (unsigned)(completed < total_inputs ? completed : total_inputs),
        (unsigned)(completed > total_inputs ? completed - total_inputs : 0), w, h,
        di.disabled ? "true" : "false") < 0) fail("checkpoint write");
    close_observation_json(log_file);
    if (fflush(log_file)) fail("checkpoint flush");
    if (completed == total_inputs) {
        FILE *input_end = open_output("input-end.json");
        if (fprintf(input_end,
            "{\"frame\":%u,\"%s\":\"%s\",\"neutral_tail_ticks_planned\":%u",
            (unsigned)completed, applied_key(), applied_hash, (unsigned)tail_ticks) < 0)
            fail("input end write");
        close_observation_json(input_end);
        if (fclose(input_end)) fail("input end close");
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
        if (full_audit_file && fclose(full_audit_file)) fail("full audit close");
        full_audit_file=NULL;
        if (fclose(log_file)) fail("checkpoint close");
        log_file = NULL;
        FILE *done = open_output("complete.json");
        if (fprintf(done,
            "{\"frame\":%u,\"input_frames\":%u,\"neutral_tail_ticks\":%u,"
            "\"stop_reason\":\"declared_observation_end_before_next_sample\","
            "\"%s\":\"%s\",\"game_outcome\":\"unclassified\"",
            (unsigned)completed, (unsigned)total_inputs, (unsigned)tail_ticks,
            applied_key(), applied_hash) < 0) fail("completion write");
        close_observation_json(done);
        if (fclose(done)) fail("completion close");
        fprintf(stdout, "input_route_complete: frames=%u words_sha256=%s\n", completed, applied_hash);
        { extern void psx_slice_diag_write(const char *dir); psx_slice_diag_write(output_dir); }
        fflush(stdout);
        if(defer_terminal){terminal_frame=completed;return;}
        exit(0);
    }
}

/* The guest .pst deliberately knows nothing about the frontend's input hash
 * contexts or files. Carry their ACTUAL history beside it, bound by the TAS
 * manifest. A resumed audit never manufactures its prefix from the tape. */
#ifndef PSX_NO_DEBUG_TOOLS
extern uint64_t s_frame_count;
#define OBS_CHECKPOINT_HEADER 2048u
#define OBS_U32(X) \
 X(total_inputs) X(tail_ticks) X(capture_every) X(terminal_frame) X(defer_terminal) \
 X(delivered_inputs) X(resumed_inputs) X(pending_word) X(pending) \
 X(first_non_neutral_seen) X(dualshock_mode) X(last_protocol_mode) X(port2_mode) \
 X(pending_port2) X(port2_pending) X(audit_source_frame) X(audit_written) \
 X(audit_original_count) X(audit_tail_count) X(audit_start_seen)
#define OBS_U64(X) X(audit_first_runtime) X(audit_last_runtime) X(audit_start_runtime)
typedef struct ObserverCheckpoint {
#define OBS_DECLARE32(n) uint32_t n;
    OBS_U32(OBS_DECLARE32)
#undef OBS_DECLARE32
#define OBS_DECLARE64(n) uint64_t n;
    OBS_U64(OBS_DECLARE64)
#undef OBS_DECLARE64
    uint64_t runtime_frame,journal_bytes,event_bytes;
    uint32_t cpu_return,neutral_p2;
    uint32_t session[INPUT_ROUTE_SESSION_CHECKPOINT_WORDS];
    uint8_t controller[7],last_p1[7],last_p2[7];
    psx_sha256_ctx hashes[8];
} ObserverCheckpoint;
static ObserverCheckpoint staged_checkpoint;
static uint8_t *staged_journal,*staged_events;
static int checkpoint_prepared;
static psx_sha256_ctx *const checkpoint_hashes[]={&input_hash,&delivered_hash,&expected_protocol_hash,
    &port2_input_hash,&port2_delivered_hash,&full_applied_hash,&full_tail_hash,&full_boundary_hash};

static int checkpoint_hash_file(const char *path,char sha[65]) {
    FILE *f=fopen(path,"rb");uint8_t block[65536],digest[32];size_t n;psx_sha256_ctx h;
    if(!f)return 0;
    psx_sha256_init(&h);
    while((n=fread(block,1,sizeof block,f)))psx_sha256_update(&h,block,n);
    int ok=!ferror(f);if(fclose(f))ok=0;
    if(ok){psx_sha256_final(&h,digest);hex_hash(digest,sha);}return ok;
}
static int checkpoint_read_output(const char *name,int optional,uint8_t **bytes,size_t *len) {
    char path[4096];*bytes=NULL;*len=0;
    int n=snprintf(path,sizeof path,"%s/%s",output_dir,name);
    if(n<0 || (size_t)n>=sizeof path)return 0;
    FILE *f=fopen(path,"rb");
    if(!f)return optional && errno==ENOENT;
    if(fseek(f,0,SEEK_END)){fclose(f);return 0;}
    long size=ftell(f);
    if(size<0 || fseek(f,0,SEEK_SET)){fclose(f);return 0;}
    *len=(size_t)size;
    if(*len){*bytes=(uint8_t*)malloc(*len);if(!*bytes){fclose(f);return 0;}}
    int ok=fread(*bytes,1,*len,f)==*len && !ferror(f);if(fclose(f))ok=0;
    if(!ok){free(*bytes);*bytes=NULL;*len=0;}return ok;
}
static int checkpoint_hash_write(PstW *w,const psx_sha256_ctx *h) {
    uint8_t tail[64]={0};
    if(h->buffered>63u || h->total%64u!=h->buffered)return 0;
    memcpy(tail,h->buffer,h->buffered);
    for(unsigned j=0;j<8;j++)if(!pst_w_u32(w,h->h[j]))return 0;
    return pst_w_u64(w,h->total) && pst_w_u32(w,(uint32_t)h->buffered) && pst_w_bytes(w,tail,sizeof tail);
}
static int checkpoint_hash_read(PstR *r,psx_sha256_ctx *h) {
    uint32_t used;
    for(unsigned j=0;j<8;j++)if(!pst_r_u32(r,&h->h[j]))return 0;
    if(!pst_r_u64(r,&h->total)||!pst_r_u32(r,&used)||used>63u||h->total%64u!=used||
       !pst_r_bytes(r,h->buffer,sizeof h->buffer))return 0;
    h->buffered=used;return 1;
}
static int checkpoint_header_write(uint8_t header[OBS_CHECKPOINT_HEADER],const ObserverCheckpoint *s) {
    PstW w;pst_w_init(&w,header,OBS_CHECKPOINT_HEADER);memset(header,0,OBS_CHECKPOINT_HEADER);
    int ok=pst_w_bytes(&w,"PSXIOB1",8);
#define OBS_WRITE32(n) ok=ok && pst_w_u32(&w,s->n);
    OBS_U32(OBS_WRITE32)
#undef OBS_WRITE32
#define OBS_WRITE64(n) ok=ok && pst_w_u64(&w,s->n);
    OBS_U64(OBS_WRITE64)
#undef OBS_WRITE64
    ok=ok && pst_w_u64(&w,s->runtime_frame) && pst_w_u32(&w,s->cpu_return) && pst_w_u32(&w,s->neutral_p2);
    for(unsigned j=0;j<8;j++)ok=ok && pst_w_u32(&w,s->session[j]);
    ok=ok && pst_w_bytes(&w,s->controller,7) && pst_w_bytes(&w,s->last_p1,7) && pst_w_bytes(&w,s->last_p2,7);
    for(unsigned j=0;j<8;j++)ok=ok && checkpoint_hash_write(&w,&s->hashes[j]);
    return ok && pst_w_u64(&w,s->journal_bytes) && pst_w_u64(&w,s->event_bytes);
}
static int checkpoint_header_read(const uint8_t header[OBS_CHECKPOINT_HEADER],ObserverCheckpoint *s) {
    PstR r;uint8_t magic[8];pst_r_init(&r,header,OBS_CHECKPOINT_HEADER);memset(s,0,sizeof *s);
    int ok=pst_r_bytes(&r,magic,8) && !memcmp(magic,"PSXIOB1",8);
#define OBS_READ32(n) ok=ok && pst_r_u32(&r,&s->n);
    OBS_U32(OBS_READ32)
#undef OBS_READ32
#define OBS_READ64(n) ok=ok && pst_r_u64(&r,&s->n);
    OBS_U64(OBS_READ64)
#undef OBS_READ64
    ok=ok && pst_r_u64(&r,&s->runtime_frame) && pst_r_u32(&r,&s->cpu_return) && pst_r_u32(&r,&s->neutral_p2);
    for(unsigned j=0;j<8;j++)ok=ok && pst_r_u32(&r,&s->session[j]);
    ok=ok && pst_r_bytes(&r,s->controller,7) && pst_r_bytes(&r,s->last_p1,7) && pst_r_bytes(&r,s->last_p2,7);
    for(unsigned j=0;j<8;j++)ok=ok && checkpoint_hash_read(&r,&s->hashes[j]);
    ok=ok && pst_r_u64(&r,&s->journal_bytes) && pst_r_u64(&r,&s->event_bytes);
    while(ok && r.p<r.end)if(*r.p++)ok=0;
    return ok;
}
int input_route_observer_checkpoint_active(void) {
    return full_audit && dualshock_mode && log_file && full_audit_file;
}
int input_route_observer_checkpoint_save(const char *path,uint32_t consumed,uint32_t cpu_return,char sha[65]) {
    ObserverCheckpoint s;uint8_t header[OBS_CHECKPOINT_HEADER],*journal=NULL,*events=NULL;size_t journal_len=0,event_len=0;
    if(!path||!sha||!input_route_observer_checkpoint_active()||pending||port2_pending||audit_source_frame||
       consumed!=delivered_inputs||fflush(full_audit_file)||!input_route_session_checkpoint_flush())return 0;
    memset(&s,0,sizeof s);
#define OBS_COPY32(n) s.n=(uint32_t)n;
    OBS_U32(OBS_COPY32)
#undef OBS_COPY32
#define OBS_COPY64(n) s.n=n;
    OBS_U64(OBS_COPY64)
#undef OBS_COPY64
    s.runtime_frame=s_frame_count;s.cpu_return=cpu_return;s.neutral_p2=(uint32_t)input_route_neutral_p2();
    if(!input_route_session_checkpoint_get(s.session))return 0;
    memcpy(s.controller,pending_controller,7);memcpy(s.last_p1,audit_last_p1,7);memcpy(s.last_p2,audit_last_p2,7);
    for(unsigned j=0;j<8;j++)s.hashes[j]=*checkpoint_hashes[j];
    int ok=checkpoint_read_output("applied-input-boundaries.bin",0,&journal,&journal_len) &&
           checkpoint_read_output("disc-events.jsonl",1,&events,&event_len);
    s.journal_bytes=journal_len;s.event_bytes=event_len;
    ok=ok && s.journal_bytes==(uint64_t)audit_written*32u && checkpoint_header_write(header,&s);
    FILE *f=ok?psx_file_create_new(path,1):NULL;
    ok=f && fwrite(header,1,sizeof header,f)==sizeof header &&
       fwrite(journal,1,journal_len,f)==journal_len && fwrite(events,1,event_len,f)==event_len;
    if(f && fclose(f))ok=0;
    free(journal);free(events);
    if(ok)ok=checkpoint_hash_file(path,sha);
    if(ok)fprintf(stderr,"[input-state] saved return=%u consumed=%u audited=%u runtime=%llu event_cursor=%u applied_mode_button=%u\n",
                  cpu_return,consumed,audit_written,(unsigned long long)s.runtime_frame,s.session[5],(unsigned)s.last_p1[6]);
    return ok;
}
int input_route_observer_checkpoint_prepare(const char *path,const char *sha,uint32_t consumed,uint32_t cpu_return) {
    uint8_t header[OBS_CHECKPOINT_HEADER],digest[32];char actual[65];ObserverCheckpoint *s=&staged_checkpoint;
    if(!path||!input_route_observer_checkpoint_active()||audit_written||ftell(full_audit_file)!=0||
       checkpoint_prepared||checkpoint_restored||!sha||strlen(sha)!=64||
       !checkpoint_hash_file(path,actual)||strcmp(actual,sha))return 0;
    FILE *f=fopen(path,"rb");if(!f)return 0;
    int ok=fread(header,1,sizeof header,f)==sizeof header && checkpoint_header_read(header,s);
    ok=ok && s->total_inputs==total_inputs && s->tail_ticks==tail_ticks && s->capture_every==capture_every &&
       s->dualshock_mode==(uint32_t)dualshock_mode && s->port2_mode==(uint32_t)port2_mode &&
       s->defer_terminal==(uint32_t)defer_terminal && s->neutral_p2==(uint32_t)input_route_neutral_p2() &&
       !s->terminal_frame && !s->pending && !s->port2_pending && !s->audit_source_frame &&
       s->delivered_inputs==consumed && s->cpu_return==cpu_return && consumed<=total_inputs+tail_ticks &&
       cpu_return<total_inputs+tail_ticks && s->resumed_inputs<=consumed &&
       s->first_non_neutral_seen<=1u && s->audit_start_seen==1u && s->last_protocol_mode<=1u &&
       s->pending_word<=0xffffu && s->pending_port2<=0xffffu &&
       s->audit_written<=consumed && s->audit_original_count<=total_inputs && s->audit_tail_count<=tail_ticks &&
       (uint64_t)s->audit_original_count+s->audit_tail_count==consumed &&
       s->journal_bytes==(uint64_t)s->audit_written*32u &&
       /* The event writer bounds a path to4096 bytes, escaped at most6x,
        * plus its bounded reason and numeric fields. */
       s->event_bytes<=s->session[5]*UINT64_C(32768) &&
       s->journal_bytes==(uint64_t)(size_t)s->journal_bytes && s->event_bytes==(uint64_t)(size_t)s->event_bytes &&
       input_route_session_checkpoint_validate(s->session);
    if(ok && s->journal_bytes){staged_journal=(uint8_t*)malloc((size_t)s->journal_bytes);ok=staged_journal!=NULL;}
    if(ok && s->event_bytes){staged_events=(uint8_t*)malloc((size_t)s->event_bytes);ok=staged_events!=NULL;}
    if(ok)ok=fread(staged_journal,1,(size_t)s->journal_bytes,f)==s->journal_bytes &&
             fread(staged_events,1,(size_t)s->event_bytes,f)==s->event_bytes && fgetc(f)==EOF && !ferror(f);
    if(fclose(f))ok=0;
    if(ok){
        psx_sha256_ctx h=s->hashes[7];psx_sha256_final(&h,digest);
        uint8_t observed[32];psx_sha256_compute(staged_journal,(size_t)s->journal_bytes,observed);
        ok=!memcmp(observed,digest,32);
        uint32_t lines=0;
        for(size_t j=0;j<(size_t)s->event_bytes;j++)if(staged_events[j]=='\n')++lines;
        ok=ok && lines==s->session[5] && (!s->event_bytes || staged_events[s->event_bytes-1u]=='\n');
    }
    if(!ok){free(staged_journal);free(staged_events);staged_journal=staged_events=NULL;return 0;}
    checkpoint_prepared=1;return 1;
}
static int checkpoint_apply(uint32_t consumed) {
    ObserverCheckpoint *s=&staged_checkpoint;
    if(!checkpoint_prepared||s->delivered_inputs!=consumed||!full_audit_file||
       fwrite(staged_journal,1,(size_t)s->journal_bytes,full_audit_file)!=s->journal_bytes ||
       fflush(full_audit_file)||!input_route_session_checkpoint_restore(s->session,staged_events,(size_t)s->event_bytes))return 0;
#define OBS_APPLY32(n) n=s->n;
    OBS_U32(OBS_APPLY32)
#undef OBS_APPLY32
#define OBS_APPLY64(n) n=s->n;
    OBS_U64(OBS_APPLY64)
#undef OBS_APPLY64
    s_frame_count=s->runtime_frame;
    memcpy(pending_controller,s->controller,7);memcpy(audit_last_p1,s->last_p1,7);memcpy(audit_last_p2,s->last_p2,7);
    for(unsigned j=0;j<8;j++)*checkpoint_hashes[j]=s->hashes[j];
    free(staged_journal);free(staged_events);staged_journal=staged_events=NULL;
    checkpoint_prepared=0;checkpoint_restored=1;return 1;
}
#else
int input_route_observer_checkpoint_active(void){return 0;}
int input_route_observer_checkpoint_save(const char *p,uint32_t n,uint32_t f,char h[65]){(void)p;(void)n;(void)f;(void)h;return 0;}
int input_route_observer_checkpoint_prepare(const char *p,const char *h,uint32_t n,uint32_t f){(void)p;(void)h;(void)n;(void)f;return 0;}
#endif
