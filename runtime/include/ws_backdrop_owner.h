#ifndef PSXRECOMP_WS_BACKDROP_OWNER_H
#define PSXRECOMP_WS_BACKDROP_OWNER_H

#include <stdint.h>

/* Per-frame semantic owner for a title-opted textured background pass.  The
 * first consumed OT rank which submits a textured polygon becomes that frame's
 * background owner.  Rank 0xFFFF is non-OT/unknown and can never establish
 * ownership. */
typedef struct WsBackdropOwner {
    uint32_t frame;
    uint16_t rank;
    uint8_t latched;
} WsBackdropOwner;

static inline int ws_backdrop_owner_match(WsBackdropOwner *owner,
                                          uint32_t frame,
                                          uint16_t rank,
                                          int textured_polygon) {
    if (owner->frame != frame) {
        owner->frame = frame;
        owner->rank = 0xFFFFu;
        owner->latched = 0;
    }
    if (!textured_polygon || rank == 0xFFFFu)
        return 0;
    if (!owner->latched) {
        owner->rank = rank;
        owner->latched = 1;
    }
    return owner->rank == rank;
}

#endif
