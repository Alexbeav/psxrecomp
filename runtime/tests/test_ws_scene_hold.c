#include "ws_scene_hold.h"
#undef NDEBUG /* the Release test target must execute its assertions */
#include <assert.h>
#include <stdio.h>

int main(void) {
    WsSceneHold s;
    ws_scene_hold_reset(&s);
    assert(ws_scene_hold_classify(&s,0,0,0)==0); /* world */
    for (int i=0;i<300;++i)
        assert(ws_scene_hold_classify(&s,1,1,0)==0); /* no GTE during load */
    assert(ws_scene_hold_classify(&s,0,1,0)==1); /* actual menu replaces it */
    assert(ws_scene_hold_classify(&s,1,0,0)==1); /* never force a 2D scene wide */
    assert(ws_scene_hold_classify(&s,0,0,0)==0);
    assert(ws_scene_hold_classify(&s,1,0,1)==1); /* FMV veto */
    assert(ws_scene_hold_classify(&s,1,0,0)==1);
    assert(ws_scene_hold_classify(&s,0,0,0)==0);
    ws_scene_hold_reset(&s); /* cannot reuse unrelated pre-restore margins */
    assert(ws_scene_hold_classify(&s,1,1,0)==1);
    assert(ws_scene_hold_classify(&s,0,0,0)==0);
    puts("PASS retained scene, menu release, FMV veto and timeline reset");
    return 0;
}
