#include "dma_gpu_ll.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The list walk's service clock (dma_gpu_ll.c): a kick lead of 64, 128 more
 * at each grid edge, headers 10 (empty) or 15, payload words 1, readiness
 * before every header, and room dropped while the walk waits. */

static uint32_t ram[256];
static uint32_t emitted[64];
static uint32_t emitted_count;
static uint32_t completed;
static uint32_t hit_limit;
static int device_ready = 1;
static int fifo_space = 1;

static uint32_t resolve(void *opaque, uint32_t address) {
    (void)opaque;
    return address & 0x3FCu;
}

static uint32_t read_word(void *opaque, uint32_t address) {
    (void)opaque;
    return ram[(address & 0x3FCu) >> 2];
}

static int begin_node(void *opaque, uint32_t address, uint32_t word_count) {
    (void)opaque;
    (void)address;
    (void)word_count;
    return 1;
}

static void emit_word(void *opaque, uint32_t address, uint32_t word) {
    (void)opaque;
    (void)address;
    if (emitted_count < 64u) emitted[emitted_count] = word;
    emitted_count++;
}

static void complete(void *opaque, int limited) {
    (void)opaque;
    completed++;
    hit_limit = (uint32_t)limited;
}

static int ready(void *opaque) {
    (void)opaque;
    return device_ready;
}

static int has_space(void *opaque) {
    (void)opaque;
    return fifo_space;
}

static const DMAGPULinkedListOps ops = {
    resolve, read_word, NULL, begin_node, emit_word, complete, ready, has_space
};

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void reset(void) {
    memset(ram, 0, sizeof(ram));
    memset(emitted, 0, sizeof(emitted));
    emitted_count = completed = hit_limit = 0u;
    device_ready = fifo_space = 1;
}

/* n empty nodes from word 0, the last one ending the list. */
static void empty_list(uint32_t n) {
    for (uint32_t i = 0; i + 1u < n; i++) ram[i] = (i + 1u) * 4u;
    ram[n - 1u] = 0x00FFFFFFu;
}

int main(void) {
    DMAGPULinkedList s;

    /* Kick on a grid edge: a lead of 64 starts 7 empty headers (0..60). Each
     * edge then adds 128 of room; the overrun is carried, so edges start 13
     * or 12 headers. */
    reset();
    empty_list(40);
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 1024u, &ops, NULL);
    CHECK(s.active && s.nodes_processed == 7u);
    CHECK(dma_gpu_ll_cycles_to_event(&s, 1024u) == 128u);
    dma_gpu_ll_serve_edges(&s, 1024u, 1152u, &ops, NULL);
    CHECK(s.nodes_processed == 20u);
    dma_gpu_ll_serve_edges(&s, 1152u, 1280u, &ops, NULL);
    CHECK(s.nodes_processed == 32u);                 /* 120 of room: 12 */
    /* A late call serves every edge it passed. */
    dma_gpu_ll_serve_edges(&s, 1280u, 1536u, &ops, NULL);
    CHECK(!s.active && completed == 1u && hit_limit == 0u);
    CHECK(s.total_words == 40u);

    /* Kick 100 clocks past an edge: the next edge is 28 clocks later and its
     * limit is that edge + 64, so the walk has 64 + 28 of room by then. */
    reset();
    empty_list(40);
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 1124u, &ops, NULL);
    CHECK(s.nodes_processed == 7u);                  /* 70 spent of 64 */
    CHECK(dma_gpu_ll_cycles_to_event(&s, 1124u) == 28u);
    dma_gpu_ll_serve_edges(&s, 1124u, 1152u, &ops, NULL);
    CHECK(s.nodes_processed == 10u);                 /* clock 70 to 92: 3 */

    /* A CHCR write is a service at its own time + 64. */
    reset();
    empty_list(40);
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 1024u, &ops, NULL); /* 7 nodes, clock 70 */
    dma_gpu_ll_serve_at(&s, 1024u + 50u, &ops, NULL);     /* limit 114 */
    CHECK(s.nodes_processed == 12u);                       /* clock 120 */
    dma_gpu_ll_serve_edges(&s, 1074u, 1152u, &ops, NULL); /* limit 192 */
    CHECK(s.nodes_processed == 20u);

    /* Payload words are paid only while there is room; the header is charged
     * in full. A 200-word node started on the kick lead shows the next node
     * at once and pays its words over the next edges. */
    reset();
    ram[0] = (200u << 24) | 0x00FFFFFFu;
    for (uint32_t i = 1; i <= 200u; i++) ram[i] = i;
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 0u, &ops, NULL);
    CHECK(s.nodes_processed == 1u && emitted_count == 49u);  /* 64 - 15 */
    dma_gpu_ll_serve_edges(&s, 0u, 128u, &ops, NULL);
    CHECK(emitted_count == 177u && s.active);
    dma_gpu_ll_serve_edges(&s, 128u, 256u, &ops, NULL);
    CHECK(emitted_count == 200u && !s.active && completed == 1u);

    /* Each payload word is read when it is paid: a CPU rewrite of a word not
     * yet paid is what the GPU receives. */
    reset();
    ram[0] = (100u << 24) | 0x00FFFFFFu;
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 0u, &ops, NULL);       /* 49 words */
    ram[60] = 0xA5A5A5A5u;
    dma_gpu_ll_serve_edges(&s, 0u, 128u, &ops, NULL);
    CHECK(emitted_count == 100u && emitted[59] == 0xA5A5A5A5u);

    /* Readiness is checked before every header, empty ones included. Room
     * left while the walk waits is dropped: after the wait the walk gains one
     * grid period per edge, 13 empty headers. */
    reset();
    empty_list(60);
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 0u, &ops, NULL);
    CHECK(s.nodes_processed == 7u);
    device_ready = 0;
    for (uint64_t e = 0; e < 1280u; e += 128u)
        dma_gpu_ll_serve_edges(&s, e, e + 128u, &ops, NULL);
    CHECK(s.nodes_processed == 7u && s.active);
    device_ready = 1;
    dma_gpu_ll_serve_edges(&s, 1280u, 1408u, &ops, NULL);
    CHECK(s.nodes_processed == 20u);
    dma_gpu_ll_serve_edges(&s, 1408u, 1536u, &ops, NULL);
    CHECK(s.nodes_processed == 33u);

    /* A full FIFO stops the payload; the walk goes on at the next edge. */
    reset();
    ram[0] = (8u << 24) | 0x00FFFFFFu;
    fifo_space = 0;
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 0u, &ops, NULL);
    CHECK(s.nodes_processed == 1u && emitted_count == 0u && s.active);
    fifo_space = 1;
    dma_gpu_ll_serve_edges(&s, 0u, 128u, &ops, NULL);
    CHECK(emitted_count == 8u && !s.active && completed == 1u);

    /* A stop finishing its packet moves words without credit. */
    reset();
    ram[0] = (100u << 24) | 0x00FFFFFFu;
    dma_gpu_ll_kick(&s, 0u, 0x40000u, 0u, &ops, NULL);
    while (dma_gpu_ll_finish_word(&s, &ops, NULL)) {}
    CHECK(emitted_count == 100u && !s.active && completed == 1u);

    /* A cycle stays busy; only the end marker or a cancel ends it. */
    reset();
    ram[0] = 0u;
    dma_gpu_ll_kick(&s, 0u, 1u, 0u, &ops, NULL);
    dma_gpu_ll_serve_edges(&s, 0u, 1280u, &ops, NULL);
    CHECK(s.active && completed == 0u && hit_limit == 0u);
    CHECK(s.nodes_processed == 135u);                /* 64 + 1280 at 10 */
    ram[0] = 0x00FFFFFFu;
    dma_gpu_ll_serve_edges(&s, 1280u, 1408u, &ops, NULL);
    CHECK(!s.active && completed == 1u && hit_limit == 0u);
    ram[0] = 0u;
    completed = 0u;
    dma_gpu_ll_kick(&s, 0u, 1u, 0u, &ops, NULL);
    dma_gpu_ll_cancel(&s);
    dma_gpu_ll_serve_edges(&s, 0u, 1280u, &ops, NULL);
    CHECK(!s.active && completed == 0u);
    CHECK(dma_gpu_ll_cycles_to_event(&s, 0u) == UINT32_MAX);

    puts("dma_gpu_linked_list_timing_test: PASS");
    return 0;
}
