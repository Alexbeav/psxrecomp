#include "netplay_snap_ring.h"
#include "boot_state.h"

#include <stdlib.h>
#include <string.h>

typedef struct NetplaySnapSlot {
    int      valid;
    uint32_t tick;
    uint8_t* data;
    size_t   size;
} NetplaySnapSlot;

struct NetplaySnapRing {
    NetplaySnapSlot* slots;
    uint32_t         depth;
    uint32_t         count;
    /* Next write index in the circular buffer (0 .. depth-1). */
    uint32_t         head;
};

static void slot_clear(NetplaySnapSlot* s) {
    free(s->data);
    s->data = NULL;
    s->size = 0;
    s->tick = 0;
    s->valid = 0;
}

static int find_slot(const NetplaySnapRing* r, uint32_t tick) {
    uint32_t i;
    if (!r) return -1;
    for (i = 0; i < r->depth; i++) {
        if (r->slots[i].valid && r->slots[i].tick == tick)
            return (int)i;
    }
    return -1;
}

NetplaySnapRing* netplay_snap_ring_create(uint32_t depth) {
    NetplaySnapRing* r;
    if (depth == 0)
        depth = NETPLAY_SNAP_RING_DEFAULT_DEPTH;
    if (depth > 512u)
        depth = 512u;
    r = (NetplaySnapRing*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->slots = (NetplaySnapSlot*)calloc(depth, sizeof(NetplaySnapSlot));
    if (!r->slots) {
        free(r);
        return NULL;
    }
    r->depth = depth;
    return r;
}

void netplay_snap_ring_destroy(NetplaySnapRing* r) {
    if (!r) return;
    netplay_snap_ring_clear(r);
    free(r->slots);
    free(r);
}

void netplay_snap_ring_clear(NetplaySnapRing* r) {
    uint32_t i;
    if (!r) return;
    for (i = 0; i < r->depth; i++)
        slot_clear(&r->slots[i]);
    r->count = 0;
    r->head = 0;
}

uint32_t netplay_snap_ring_depth(const NetplaySnapRing* r) {
    return r ? r->depth : 0u;
}

uint32_t netplay_snap_ring_count(const NetplaySnapRing* r) {
    return r ? r->count : 0u;
}

int netplay_snap_ring_has(const NetplaySnapRing* r, uint32_t tick) {
    return find_slot(r, tick) >= 0;
}

int netplay_snap_ring_store(NetplaySnapRing* r, uint32_t tick,
                            uint8_t* data, size_t size) {
    int existing;
    NetplaySnapSlot* s;
    if (!r || !data || !size) return 0;

    existing = find_slot(r, tick);
    if (existing >= 0) {
        s = &r->slots[existing];
        free(s->data);
        s->data = data;
        s->size = size;
        s->tick = tick;
        s->valid = 1;
        return 1;
    }

    if (r->count < r->depth) {
        /* Prefer an unused slot; walk from head for locality. */
        uint32_t i;
        for (i = 0; i < r->depth; i++) {
            uint32_t idx = (r->head + i) % r->depth;
            if (!r->slots[idx].valid) {
                s = &r->slots[idx];
                s->data = data;
                s->size = size;
                s->tick = tick;
                s->valid = 1;
                r->count++;
                r->head = (idx + 1u) % r->depth;
                return 1;
            }
        }
    }

    /* Evict the oldest (lowest tick) occupied slot. */
    {
        uint32_t i;
        int victim = -1;
        uint32_t oldest = 0xffffffffu;
        for (i = 0; i < r->depth; i++) {
            if (!r->slots[i].valid) continue;
            if (r->slots[i].tick <= oldest) {
                oldest = r->slots[i].tick;
                victim = (int)i;
            }
        }
        if (victim < 0) {
            free(data);
            return 0;
        }
        s = &r->slots[victim];
        free(s->data);
        s->data = data;
        s->size = size;
        s->tick = tick;
        s->valid = 1;
        r->head = ((uint32_t)victim + 1u) % r->depth;
        return 1;
    }
}

const uint8_t* netplay_snap_ring_peek(const NetplaySnapRing* r, uint32_t tick,
                                      size_t* size_out) {
    int idx = find_slot(r, tick);
    if (idx < 0) {
        if (size_out) *size_out = 0;
        return NULL;
    }
    if (size_out) *size_out = r->slots[idx].size;
    return r->slots[idx].data;
}

int netplay_snap_ring_save(NetplaySnapRing* r, uint32_t tick,
                           const CPUState* cpu, uint32_t bios_checksum,
                           uint32_t entry_pc) {
    uint8_t* data = NULL;
    size_t len = 0;
    if (!r || !cpu) return 0;
    if (!boot_state_save_buffer(cpu, bios_checksum, entry_pc, &data, &len))
        return 0;
    if (!netplay_snap_ring_store(r, tick, data, len))
        return 0; /* store frees data on hard failure */
    return 1;
}

int netplay_snap_ring_load(NetplaySnapRing* r, uint32_t tick, CPUState* cpu,
                           uint32_t bios_checksum, uint32_t entry_pc) {
    size_t size = 0;
    const uint8_t* data = netplay_snap_ring_peek(r, tick, &size);
    if (!data || !size || !cpu) return 0;
    return boot_state_load_buffer(data, size, bios_checksum, entry_pc, cpu);
}

uint32_t netplay_snap_ring_oldest_tick(const NetplaySnapRing* r) {
    uint32_t i, oldest = 0xffffffffu;
    int any = 0;
    if (!r) return 0;
    for (i = 0; i < r->depth; i++) {
        if (!r->slots[i].valid) continue;
        if (!any || r->slots[i].tick < oldest) {
            oldest = r->slots[i].tick;
            any = 1;
        }
    }
    return any ? oldest : 0u;
}

uint32_t netplay_snap_ring_newest_tick(const NetplaySnapRing* r) {
    uint32_t i, newest = 0;
    int any = 0;
    if (!r) return 0;
    for (i = 0; i < r->depth; i++) {
        if (!r->slots[i].valid) continue;
        if (!any || r->slots[i].tick > newest) {
            newest = r->slots[i].tick;
            any = 1;
        }
    }
    return any ? newest : 0u;
}
