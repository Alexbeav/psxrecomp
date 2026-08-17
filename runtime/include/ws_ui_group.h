#ifndef PSXRECOMP_WS_UI_GROUP_H
#define PSXRECOMP_WS_UI_GROUP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_UI_GROUP_JOIN_GAP 24

typedef struct {
    uint32_t key;
    int32_t x;
    int32_t width;
    int32_t anchor;
    /* Diagnostic only: index of the union-find representative this item
     * merged into, so an observer can tell "these two share an anchor because
     * they are one run" from "these two happen to land on the same third".
     * Anchor equality cannot distinguish those — there are only three anchor
     * values — and that distinction is the whole question when a HUD element
     * splits apart under the squash. Written by ws_ui_group_assign; never read
     * by the transform path. */
    uint32_t root;
} WsUiGroupItem;

/* Assign one thirds anchor to each complete spatial run. Items with the same
 * texture/row key join when their horizontal intervals touch or are separated
 * by at most WS_UI_GROUP_JOIN_GAP pixels. This is intentionally a whole-frame
 * operation: animated text is coherent on its first frame, not one frame late. */
void ws_ui_group_assign(WsUiGroupItem *items, size_t count,
                        int32_t display_width, int dense_menu);

int32_t ws_ui_anchor_for_bounds(int32_t x, int32_t width,
                                int32_t display_width);

#ifdef __cplusplus
}
#endif

#endif
