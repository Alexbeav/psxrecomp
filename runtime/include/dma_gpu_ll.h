#ifndef PSXRECOMP_DMA_GPU_LL_H
#define PSXRECOMP_DMA_GPU_LL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DMA_GPU_LL_PHASE_IDLE = 0,
    DMA_GPU_LL_PHASE_HEADER = 1,
    DMA_GPU_LL_PHASE_SETUP = 2,
    DMA_GPU_LL_PHASE_PAYLOAD = 3
};

typedef struct {
    uint8_t active;
    uint8_t phase;
    uint8_t emit_node;
    uint8_t hit_limit;
    uint32_t start_addr;
    uint32_t current_addr;
    uint32_t next_addr;
    uint32_t word_count;
    uint32_t payload_index;
    uint32_t cycles_remaining;
    uint32_t nodes_processed;
    uint32_t max_nodes;
    uint32_t total_words;
    uint32_t empty_rank;
} DMAGPULinkedList;

typedef struct {
    uint32_t (*resolve_address)(void *opaque, uint32_t address);
    uint32_t (*read_word)(void *opaque, uint32_t address);
    void (*observe_header)(void *opaque, uint32_t address, uint32_t header);
    int (*begin_node)(void *opaque, uint32_t address, uint32_t word_count);
    void (*emit_word)(void *opaque, uint32_t address, uint32_t word);
    void (*complete)(void *opaque, int hit_limit);
    /* The device can take the next header (checked before every header). */
    int (*ready)(void *opaque);
    /* The device can take the next payload word. */
    int (*has_space)(void *opaque);
} DMAGPULinkedListOps;

void dma_gpu_ll_start(DMAGPULinkedList *state, uint32_t start_addr,
                      uint32_t max_nodes);
void dma_gpu_ll_cancel(DMAGPULinkedList *state);
/* Start a walk at a kick at `now` and serve it at once. While a walk is
 * active, cycles_remaining holds its credit (signed) as of the latest grid
 * edge. */
void dma_gpu_ll_kick(DMAGPULinkedList *state, uint32_t start_addr,
                     uint32_t max_nodes, uint64_t now,
                     const DMAGPULinkedListOps *ops, void *opaque);
/* A service at `now` that is not a grid edge (a channel 2 CHCR write). */
void dma_gpu_ll_serve_at(DMAGPULinkedList *state, uint64_t now,
                         const DMAGPULinkedListOps *ops, void *opaque);
/* Serve each grid edge in (from, to]. */
void dma_gpu_ll_serve_edges(DMAGPULinkedList *state, uint64_t from,
                            uint64_t to, const DMAGPULinkedListOps *ops,
                            void *opaque);
/* Cycles from `now` to the next service of an active walk. */
uint32_t dma_gpu_ll_cycles_to_event(const DMAGPULinkedList *state,
                                    uint64_t now);
/* Move one payload word of the node in progress without credit (a stop
 * finishing its packet). Returns 0 when no payload is in progress. */
int dma_gpu_ll_finish_word(DMAGPULinkedList *state,
                           const DMAGPULinkedListOps *ops, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
