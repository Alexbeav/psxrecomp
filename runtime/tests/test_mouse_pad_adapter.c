#include "mouse_pad_adapter.h"
#include <assert.h>

int main(void) {
    mouse_pad_configure(1, 10, 4);
    mouse_pad_set_focus(1);
    mouse_pad_add_motion(5, -5);
    unsigned short p = mouse_pad_merge(0xFFFFu, PSX_MOUSE_RIGHT | PSX_MOUSE_LEFT);
    assert((p & (1u << 15)) == 0); /* left mouse -> Square/fire */
    assert((p & (1u << 10)) == 0); /* right mouse -> L1/aim */
    assert((p & (1u << 5)) == 0);  /* positive X -> D-pad right */
    assert((p & (1u << 6)) == 0);  /* mouse up -> authored non-inverted view */
    mouse_pad_commit_frame();
    mouse_pad_reset();
    mouse_pad_add_motion(5, 0);
    p = mouse_pad_merge(0xFFFFu, 0);
    assert((p & (1u << 5)) != 0);  /* below the normal chase threshold */
    p = mouse_pad_merge(0xFFFFu, PSX_MOUSE_RIGHT);
    assert((p & (1u << 5)) == 0);  /* same delta triggers at aimed sensitivity */
    assert((p & (1u << 4)) != 0);  /* no vertical motion was queued */
    mouse_pad_commit_frame();
    mouse_pad_set_focus(0);
    p = mouse_pad_merge(0xFFFFu, PSX_MOUSE_LEFT);
    assert(p == 0xFFFFu);          /* focus loss clears and releases */
    return 0;
}
