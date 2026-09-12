/* Per-field sentinel round-trip for the four comparison-profile wire sections:
 * BS_SEC_GPU_SERVICE, BS_SEC_DMA_SRC, BS_SEC_IRQ_TIMING, BS_SEC_TIMER_SRC.
 *
 * Why per-FIELD sentinels: a value reused across same-width neighbours hides a
 * writer/reader field-ORDER divergence. That is exactly what shipped in
 * source_gpu_service_wire_read(), where `error` and the two word arrays were
 * consumed at the wrong offsets while both directions still totalled 240 bytes,
 * so section_shape_ok() and the _Static_assert(sizeof) both passed. Every field
 * here therefore carries a DISTINCT sentinel with its field index baked in.
 *
 * Method: build the canonical byte image in the writer's field order (names and
 * widths are the writer's, transcribed below), read() it into the module globals,
 * write() it back, and require the image to reproduce byte-for-byte. A reader
 * that consumes the tail in a different order cannot round-trip.
 *
 * The section codecs live in module translation units that own their state, so
 * this TU links those four TUs; the driver (test_boot_state_section_wire.py)
 * supplies link stubs for their unrelated external references.
 *
 * The prototypes are declared here rather than pulled from the runtime headers:
 * those headers are compiled with the module build's relaxed warning set, and
 * -Werror on this TU is part of the contract. They must match
 * runtime/include/{source_gpu_runtime,dma,interrupts,timers}.h.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t source_gpu_service_wire_bytes(void);
void     source_gpu_service_wire_write(uint8_t *out);
int      source_gpu_service_wire_read(const uint8_t *in, uint32_t len);
uint32_t dma_src_wire_bytes(void);
void     dma_src_wire_write(uint8_t *out);
int      dma_src_wire_read(const uint8_t *in, uint32_t len);
uint32_t interrupts_timing_wire_bytes(void);
void     interrupts_timing_wire_write(uint8_t *out);
int      interrupts_timing_wire_read(const uint8_t *in, uint32_t len);
uint32_t timers_source_wire_bytes(void);
void     timers_source_wire_write(uint8_t *out);
int      timers_source_wire_read(const uint8_t *in, uint32_t len);

typedef struct { const char *name; unsigned width; unsigned repeat; } Field;
typedef struct {
    const char *section;
    const Field *fields;
    unsigned     count;
    uint32_t   (*bytes)(void);
    void       (*write)(uint8_t *);
    int        (*read)(const uint8_t *, uint32_t);
} Section;

#define F(n, w)     { n, w, 1u }
#define REP(n, w, r) { n, w, r }

/* ---- canonical field order, transcribed from each writer ---- */
static const Field gpu_service_fields[] = {
    F("clock.cycle", 8),               F("clock.gpu_deadline", 8),
    F("clock.dma_deadline", 8),        F("clock.frame_request_cycle", 8),
    F("clock.zero_reached", 4),        F("clock.frame_pending", 4),
    F("clock.frame_returns", 4),       F("command.budget", 4),
    F("command.count", 4),             F("command.phase", 4),
    F("command.command", 4),           F("command.last_update", 8),
    F("command.clip_x0", 4),           F("command.clip_y0", 4),
    F("command.clip_x1", 4),           F("command.clip_y1", 4),
    F("command.offset_x", 4),          F("command.offset_y", 4),
    F("command.draw_mode", 4),         F("command.texture_window", 4),
    F("command.mask_bits", 4),         F("command.display_mode", 4),
    F("command.dma_direction", 4),     F("command.field_valid", 4),
    F("command.skip_field", 4),        F("command.first_triangles", 4),
    F("command.second_triangles", 4),  REP("command.polygon_words", 4, 12),
    F("command.transfer_words", 4),    F("command.dispatch.kind", 4),
    F("command.dispatch.count", 4),    REP("command.dispatch.words", 4, 12),
    F("command.error", 4),
};

static const Field dma_src_fields[] = {
    F("upload.remaining", 4),  F("upload.block_size", 4), F("upload.in_block", 4),
    F("upload.address", 4),    F("upload.budget", 4),
    F("upload.last_cycle", 8), F("upload.next_cycle", 8),
    F("ll.active", 4),         F("ll.address", 4),        F("ll.remaining", 4),
    F("ll.nodes", 4),          F("ll.budget", 4),
    F("ll.last_cycle", 8),     F("ll.next_cycle", 8),
    F("spu.remaining", 4),     F("spu.block_size", 4),    F("spu.in_block", 4),
    F("spu.address", 4),       F("spu.total_words", 4),   F("spu.start_addr", 4),
    F("spu.budget", 4),        F("spu.last_cycle", 8),    F("spu.next_cycle", 8),
    F("otc.remaining", 4),     F("otc.address", 4),
    F("otc.last_cycle", 8),    F("otc.next_cycle", 8),
};

static const Field irq_timing_fields[] = {
    F("field_clock.remainder", 4),            F("field_clock.line_phase", 4),
    F("field_clock.field", 4),                F("field_clock.current_cycles", 4),
    F("input_route_raster_pending", 4),       F("last_sio_seq_seen", 4),
    F("last_sio_progress_cycle", 8),          F("post_exception_cooldown_until", 8),
    F("source_irq_slot.pc", 4),               F("source_irq_slot.target", 4),
    F("source_irq_slot.cause", 4),            F("defer_switch.from", 4),
    F("defer_switch.target", 4),              F("defer_switch.pending", 4),
};

static const Field timer_src_fields[] = {
    F("timer1.mode", 4),       F("timer1.counter", 4), F("timer1.target", 4),
    F("timer1.counting", 4),   F("timer1.blank", 4),   F("timer1.cycle", 8),
    F("timer2.counter", 4),    F("timer2.mode", 4),    F("timer2.target", 4),
    F("timer2.divider", 4),    F("timer2.irq_done", 4),F("timer2.counting", 4),
    F("timer2.elapsed", 4),    F("timer2.deadline", 4),
};

static const Section sections[] = {
    { "GPU_SERVICE", gpu_service_fields,
      (unsigned)(sizeof gpu_service_fields / sizeof gpu_service_fields[0]),
      source_gpu_service_wire_bytes, source_gpu_service_wire_write,
      source_gpu_service_wire_read },
    { "DMA_SRC", dma_src_fields,
      (unsigned)(sizeof dma_src_fields / sizeof dma_src_fields[0]),
      dma_src_wire_bytes, dma_src_wire_write, dma_src_wire_read },
    { "IRQ_TIMING", irq_timing_fields,
      (unsigned)(sizeof irq_timing_fields / sizeof irq_timing_fields[0]),
      interrupts_timing_wire_bytes, interrupts_timing_wire_write,
      interrupts_timing_wire_read },
    { "TIMER_SRC", timer_src_fields,
      (unsigned)(sizeof timer_src_fields / sizeof timer_src_fields[0]),
      timers_source_wire_bytes, timers_source_wire_write,
      timers_source_wire_read },
};
#define SECTION_CAP 512u
#define POISON 0xAAu

static int failures = 0;
static void check(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

/* Distinct per field: the field index is folded into every byte, so every
 * same-width neighbour differs. */
static uint64_t field_value(unsigned field_index, unsigned width) {
    uint64_t v = 0xA5A5A5A5A5A5A5A5ULL ^
                 ((uint64_t)(field_index + 1u) * 0x0101010101010101ULL);
    if (width < 8u) v &= (UINT64_C(1) << (width * 8u)) - 1u;
    return v;
}

/* Two fields are constrained by their readers and cannot hold an arbitrary
 * sentinel: the GPU reader refuses count!=0 (an unsynchronized queue is a stub),
 * and the IRQ reader normalizes pending to 0/1. Both stay distinct from every
 * other field; they just cannot be arbitrary. */
static int seeded_constant(const char *name, unsigned width, uint8_t *out) {
    if (strcmp(name, "command.count") == 0) {
        memset(out, 0, width);
        return 1;
    }
    if (strcmp(name, "defer_switch.pending") == 0) {
        memset(out, 0, width);
        out[0] = 1u;
        return 1;
    }
    return 0;
}

static void expand_name(char *dst, size_t cap, const char *name,
                        unsigned repeat, unsigned idx) {
    if (repeat > 1u) snprintf(dst, cap, "%s[%u]", name, idx);
    else             snprintf(dst, cap, "%s", name);
}

/* Fills the canonical image; returns its total byte length, or ~0u on overflow. */
static unsigned build_image(const Section *s, uint8_t *buf) {
    unsigned off = 0;
    char label[96];
    for (unsigned f = 0; f < s->count; ++f) {
        for (unsigned r = 0; r < s->fields[f].repeat; ++r) {
            unsigned w = s->fields[f].width;
            if (off + w > SECTION_CAP) return ~0u;
            expand_name(label, sizeof label, s->fields[f].name,
                        s->fields[f].repeat, r);
            if (!seeded_constant(label, w, buf + off)) {
                uint64_t v = field_value(off / 4u, w);
                for (unsigned b = 0; b < w; ++b)
                    buf[off + b] = (uint8_t)(v >> (8u * b));
            }
            off += w;
        }
    }
    return off;
}

/* Names the field containing byte offset `off`. */
static void label_at(const Section *s, unsigned off, char *dst, size_t cap) {
    unsigned cursor = 0;
    char label[96];
    for (unsigned f = 0; f < s->count; ++f) {
        for (unsigned r = 0; r < s->fields[f].repeat; ++r) {
            unsigned w = s->fields[f].width;
            if (off < cursor + w) {
                expand_name(label, sizeof label, s->fields[f].name,
                            s->fields[f].repeat, r);
                snprintf(dst, cap, "%s @%u", label, cursor);
                return;
            }
            cursor += w;
        }
    }
    snprintf(dst, cap, "past-end @%u", off);
}

static void run_section(const Section *s) {
    uint8_t in[SECTION_CAP], out[SECTION_CAP], poisoned[SECTION_CAP];
    char label[128];
    char what[256];

    unsigned encoded = build_image(s, in);
    uint32_t declared = s->bytes();
    int accepted;
    unsigned touched;

    snprintf(what, sizeof what, "%s: canonical image fits", s->section);
    check(encoded != ~0u, what);
    if (encoded == ~0u) return;

    /* 1. declared section length must equal the bytes actually encoded. A gap
     *    leaves the section tail unwritten (and in boot_state.c uninitialized). */
    snprintf(what, sizeof what,
             "%s: declared wire size (%u) equals encoded size (%u)",
             s->section, (unsigned)declared, encoded);
    check(encoded == declared, what);

    /* 2. the writer must touch exactly `encoded` bytes of the payload. */
    memset(poisoned, POISON, sizeof poisoned);
    s->write(poisoned);
    touched = 0;
    for (unsigned i = 0; i < encoded; ++i)
        if (poisoned[i] != POISON) touched = i + 1u;
    snprintf(what, sizeof what, "%s: writer emits every declared byte (%u)",
             s->section, encoded);
    check(touched == encoded, what);
    for (unsigned i = encoded; i < sizeof poisoned; ++i) {
        if (poisoned[i] != POISON) {
            snprintf(what, sizeof what, "%s: writer overruns its declared size",
                     s->section);
            check(0, what);
            break;
        }
    }

    /* 3. canonical image -> read -> write must reproduce every field. */
    accepted = s->read(in, declared);
    snprintf(what, sizeof what, "%s: read accepts the canonical image", s->section);
    check(accepted == 1, what);

    memset(out, 0, sizeof out);
    s->write(out);
    for (unsigned i = 0; i < encoded; ++i) {
        if (in[i] != out[i]) {
            label_at(s, i, label, sizeof label);
            snprintf(what, sizeof what,
                     "%s: byte %u (%s) differs after read->write (0x%02X -> 0x%02X)",
                     s->section, i, label, in[i], out[i]);
            check(0, what);
            break;
        }
    }
}

int main(void) {
    for (unsigned i = 0; i < sizeof sections / sizeof sections[0]; ++i)
        run_section(&sections[i]);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: per-field sentinel round-trip for GPU_SERVICE, DMA_SRC, "
         "IRQ_TIMING, TIMER_SRC");
    return 0;
}

