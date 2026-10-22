#include "dma_gpu_ll.h"

#include <string.h>

/* GPU linked-list walk timing (PS1G-93 model note, measured on psx-beetle
 * 5759277b by the lane's probe disc, cases 1a-1f and 2d).
 *
 * The walk moves only at a service. A service at time t lets the walk run
 * while its own clock is below t + DMA_GPU_LL_LEAD. Services are the kick,
 * every DMA_GPU_LL_PERIOD grid edge, and a channel 2 CHCR write. The walk
 * keeps its clock as `credit`: the room left below the latest limit, stored
 * as of the latest grid edge so no service time needs to be kept.
 *
 * Costs: a header with no words 10, a header with words 15, a payload word 1.
 * A header starts while credit is above zero and is charged in full (the
 * overrun is carried). Payload words are paid only while credit is above
 * zero. Before every header, empty or not, the walk waits until the device
 * is ready (case 1f). Room left while the walk waits is dropped, so after a
 * wait the walk gains one grid period per service (1f: 13 empty headers per
 * service). The CPU is never held. */
#define DMA_GPU_LL_EMPTY_HEADER 10
#define DMA_GPU_LL_HEADER       15
#define DMA_GPU_LL_WORD          1
#define DMA_GPU_LL_LEAD         64
#define DMA_GPU_LL_PERIOD      128u

static int32_t get_credit(const DMAGPULinkedList *s) {
    return (int32_t)s->cycles_remaining;
}

static void set_credit(DMAGPULinkedList *s, int32_t credit) {
    s->cycles_remaining = (uint32_t)credit;
}

static uint32_t resolve_address(const DMAGPULinkedListOps *ops, void *opaque,
                                uint32_t address) {
    return ops->resolve_address ? ops->resolve_address(opaque, address) : address;
}

static void finish(DMAGPULinkedList *state,
                   const DMAGPULinkedListOps *ops, void *opaque) {
    int hit_limit = state->hit_limit != 0;
    state->active = 0;
    state->phase = DMA_GPU_LL_PHASE_IDLE;
    state->cycles_remaining = 0;
    if (ops->complete) ops->complete(opaque, hit_limit);
}

void dma_gpu_ll_start(DMAGPULinkedList *state, uint32_t start_addr,
                      uint32_t max_nodes) {
    memset(state, 0, sizeof(*state));
    state->active = 1;
    state->phase = DMA_GPU_LL_PHASE_HEADER;
    state->start_addr = start_addr;
    state->current_addr = start_addr;
    state->max_nodes = max_nodes;
    state->empty_rank = UINT32_MAX;
}

void dma_gpu_ll_cancel(DMAGPULinkedList *state) {
    memset(state, 0, sizeof(*state));
}

/* Move the payload of the node in progress by one word. */
static void pay_word(DMAGPULinkedList *state,
                     const DMAGPULinkedListOps *ops, void *opaque) {
    uint32_t word_addr = resolve_address(
        ops, opaque, state->current_addr + 4u + state->payload_index * 4u);
    if (state->emit_node) {
        uint32_t word = ops->read_word(opaque, word_addr);
        if (ops->emit_word) ops->emit_word(opaque, word_addr, word);
    }
    state->payload_index++;
    state->total_words++;
}

/* After the last payload word: end the list or go to the next header. */
static void end_node(DMAGPULinkedList *state,
                     const DMAGPULinkedListOps *ops, void *opaque) {
    if (state->next_addr == 0x00FFFFFFu) {
        finish(state, ops, opaque);
        return;
    }
    state->phase = DMA_GPU_LL_PHASE_HEADER;
    state->current_addr = resolve_address(ops, opaque, state->next_addr);
}

/* Take steps while there is credit. Returns with the walk finished, out of
 * credit, waiting for the device before a header, or blocked by a full FIFO. */
static void run(DMAGPULinkedList *state,
                const DMAGPULinkedListOps *ops, void *opaque) {
    while (state->active && get_credit(state) > 0) {
        if (state->phase == DMA_GPU_LL_PHASE_HEADER) {
            if (ops->ready && !ops->ready(opaque)) return;
            state->current_addr = resolve_address(
                ops, opaque, state->current_addr);
            uint32_t header = ops->read_word(opaque, state->current_addr);
            if (ops->observe_header)
                ops->observe_header(opaque, state->current_addr, header);
            state->word_count = header >> 24;
            state->payload_index = 0;
            state->next_addr = header & 0x00FFFFFFu;
            state->nodes_processed++;
            state->total_words++;
            set_credit(state, get_credit(state) -
                       (state->word_count ? DMA_GPU_LL_HEADER
                                          : DMA_GPU_LL_EMPTY_HEADER));
            if (state->word_count == 0)
                state->empty_rank = state->empty_rank == UINT32_MAX
                    ? 0u : state->empty_rank + 1u;
            state->emit_node = ops->begin_node
                ? (uint8_t)(ops->begin_node(opaque, state->current_addr,
                                            state->word_count) != 0)
                : 1u;
            if (state->word_count == 0) {
                end_node(state, ops, opaque);
            } else {
                state->phase = DMA_GPU_LL_PHASE_PAYLOAD;
            }
        } else if (state->phase == DMA_GPU_LL_PHASE_SETUP) {
            /* A state saved by the previous walker between header and payload. */
            state->emit_node = ops->begin_node
                ? (uint8_t)(ops->begin_node(opaque, state->current_addr,
                                            state->word_count) != 0)
                : 1u;
            state->phase = DMA_GPU_LL_PHASE_PAYLOAD;
        } else if (state->phase == DMA_GPU_LL_PHASE_PAYLOAD) {
            if (ops->has_space && !ops->has_space(opaque)) return;
            pay_word(state, ops, opaque);
            set_credit(state, get_credit(state) - DMA_GPU_LL_WORD);
            if (state->payload_index >= state->word_count)
                end_node(state, ops, opaque);
        } else {
            state->hit_limit = 1;
            finish(state, ops, opaque);
        }
    }
}

/* One service with `credit` room: run, then drop room the walk could not use. */
static int32_t serve(DMAGPULinkedList *state, int32_t credit,
                     const DMAGPULinkedListOps *ops, void *opaque) {
    set_credit(state, credit);
    run(state, ops, opaque);
    if (!state->active) return 0;
    credit = get_credit(state);
    return credit > 0 ? 0 : credit;
}

void dma_gpu_ll_kick(DMAGPULinkedList *state, uint32_t start_addr,
                     uint32_t max_nodes, uint64_t now,
                     const DMAGPULinkedListOps *ops, void *opaque) {
    uint32_t phase = (uint32_t)(now % DMA_GPU_LL_PERIOD);
    dma_gpu_ll_start(state, start_addr, max_nodes);
    if (!ops || !ops->read_word) return;
    int32_t left = serve(state, DMA_GPU_LL_LEAD, ops, opaque);
    if (state->active) set_credit(state, left - (int32_t)phase);
}

void dma_gpu_ll_serve_at(DMAGPULinkedList *state, uint64_t now,
                         const DMAGPULinkedListOps *ops, void *opaque) {
    if (!state->active || !ops || !ops->read_word) return;
    int32_t phase = (int32_t)(now % DMA_GPU_LL_PERIOD);
    int32_t left = serve(state, get_credit(state) + phase, ops, opaque);
    if (state->active) set_credit(state, left - phase);
}

void dma_gpu_ll_serve_edges(DMAGPULinkedList *state, uint64_t from,
                            uint64_t to, const DMAGPULinkedListOps *ops,
                            void *opaque) {
    if (!state->active || !ops || !ops->read_word || to <= from) return;
    uint64_t edge = from - from % DMA_GPU_LL_PERIOD + DMA_GPU_LL_PERIOD;
    for (; edge <= to && state->active; edge += DMA_GPU_LL_PERIOD) {
        int32_t left = serve(state, get_credit(state) +
                             (int32_t)DMA_GPU_LL_PERIOD, ops, opaque);
        if (state->active) set_credit(state, left);
    }
}

uint32_t dma_gpu_ll_cycles_to_event(const DMAGPULinkedList *state,
                                    uint64_t now) {
    if (!state->active) return UINT32_MAX;
    return DMA_GPU_LL_PERIOD - (uint32_t)(now % DMA_GPU_LL_PERIOD);
}

int dma_gpu_ll_finish_word(DMAGPULinkedList *state,
                           const DMAGPULinkedListOps *ops, void *opaque) {
    if (!state->active || state->phase != DMA_GPU_LL_PHASE_PAYLOAD ||
        !ops || !ops->read_word)
        return 0;
    pay_word(state, ops, opaque);
    if (state->payload_index >= state->word_count) end_node(state, ops, opaque);
    return 1;
}
