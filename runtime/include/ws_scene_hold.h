#pragma once

/* Host presentation history, not guest hardware state. Never serialize it:
 * a restored canonical framebuffer cannot reconstruct discarded wide strips. */
typedef struct WsSceneHold { int valid, native_43; } WsSceneHold;
static inline void ws_scene_hold_reset(WsSceneHold* state) {
    state->valid = 0; state->native_43 = 1;
}
static inline int ws_scene_hold_classify(WsSceneHold* state, int hold,
                                          int native_43, int fmv) {
    /* FMV always wins, and cannot seed a later wide retained scene. */
    if (fmv) {
        state->valid = 1; state->native_43 = 1;
        return 1;
    }
    if (hold && state->valid) return state->native_43;
    state->valid = 1; state->native_43 = native_43 != 0;
    return state->native_43;
}
