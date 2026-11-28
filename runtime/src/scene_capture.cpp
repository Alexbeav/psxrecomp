/* Typed scene capture: GTE projections joined to GP0 primitives.
 * Contract and data model: include/scene_capture.h. Format of the per-game
 * adapter: docs/scene_capture.md. Host memory only; never touches guest
 * state. */

#include "scene_capture.h"
#include "cpu_state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
int g_scene_capture_on = 0;
}

namespace {

constexpr uint32_t kVertexCap = 1u << 17;
constexpr uint32_t kTransformCap = 1u << 14;
constexpr uint32_t kMaxFramePrims = 1u << 16;

struct TransformKeyFields {        /* the fields compared to detect a change */
    int16_t rt[9]; int32_t tr[3]; uint16_t h; int32_t ofx, ofy;
    int16_t l[9], lc[9]; int32_t bk[3], fc[3]; int16_t dqa; int32_t dqb;
    bool operator==(const TransformKeyFields &o) const {
        return std::memcmp(rt, o.rt, sizeof rt) == 0 && std::memcmp(tr, o.tr, sizeof tr) == 0 &&
               h == o.h && ofx == o.ofx && ofy == o.ofy &&
               std::memcmp(l, o.l, sizeof l) == 0 && std::memcmp(lc, o.lc, sizeof lc) == 0 &&
               std::memcmp(bk, o.bk, sizeof bk) == 0 && std::memcmp(fc, o.fc, sizeof fc) == 0 &&
               dqa == o.dqa && dqb == o.dqb;
    }
};

struct State {
    uint32_t window = 4;
    std::string adapter_game;

    uint32_t vblank = 0;
    uint32_t caller_ra = 0;
    uint32_t pending_rot = 0, pending_trans = 0;

    std::vector<SceneVertex> vertices = std::vector<SceneVertex>(kVertexCap);
    uint32_t next_vertex = 1;
    std::vector<SceneTransform> transforms = std::vector<SceneTransform>(kTransformCap);
    uint32_t next_transform = 1;
    bool have_transform = false;
    TransformKeyFields last{};
    std::unordered_map<uint32_t, uint32_t> by_sxy;          /* packed -> vertex id */
    std::unordered_map<uint32_t, uint32_t> ra_ordinal;      /* caller ra -> count this frame */

    /* open frame */
    std::vector<ScenePrim> prims;
    uint32_t ot_walk = 0, ot_rank = 0, frame_ot_walks = 0, draw_calls = 0;
    bool in_ot = false;
    uint32_t vblank_open = 0, stale = 0, ambiguous = 0;
    uint16_t draw_mode = 0;                                 /* last GP0(E1) */

    /* last closed frame */
    std::vector<ScenePrim> f_prims;
    std::vector<SceneVertex> f_vertices;
    std::vector<SceneTransform> f_transforms;
    SceneCaptureStats f_stats{};
    uint64_t frames = 0, projections = 0, transforms_made = 0;
};

State &S() {
    static State s;
    return s;
}

uint32_t mix(uint32_t h, uint32_t v) {     /* FNV-1a over a word */
    for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFFu; h *= 16777619u; }
    return h;
}

const SceneVertex *vertex_by_id(const State &s, uint32_t id) {
    if (id == 0 || id >= s.next_vertex || s.next_vertex - id > kVertexCap) return nullptr;
    return &s.vertices[id % kVertexCap];
}

const SceneTransform *transform_by_id(const State &s, uint32_t id) {
    if (id == 0 || id >= s.next_transform || s.next_transform - id > kTransformCap) return nullptr;
    return &s.transforms[id % kTransformCap];
}

uint32_t new_transform(State &s, const SceneGteProjection &p, const TransformKeyFields &k) {
    const uint32_t id = s.next_transform++;
    SceneTransform &t = s.transforms[id % kTransformCap];
    t = {};
    t.id = id;
    t.caller_ra = s.caller_ra;
    t.rot_src = s.pending_rot;
    t.trans_src = s.pending_trans;
    std::memcpy(t.rt, k.rt, sizeof t.rt);
    std::memcpy(t.tr, k.tr, sizeof t.tr);
    t.h = k.h; t.ofx = k.ofx; t.ofy = k.ofy;
    std::memcpy(t.l, k.l, sizeof t.l);
    std::memcpy(t.lc, k.lc, sizeof t.lc);
    std::memcpy(t.bk, k.bk, sizeof t.bk);
    std::memcpy(t.fc, k.fc, sizeof t.fc);
    t.dqa = k.dqa; t.dqb = k.dqb;
    /* Object key: the MATRIX the game passed when the adapter hooks it (one
     * per object in libgte games); otherwise the issuing code address and the
     * how-many-th transform it made this frame. */
    uint32_t key = 2166136261u;
    if (t.rot_src || t.trans_src) {
        key = mix(mix(mix(key, 1), t.rot_src), t.trans_src);
    } else {
        const uint32_t nth = s.ra_ordinal[s.caller_ra]++;
        key = mix(mix(mix(key, 2), s.caller_ra), nth);
    }
    t.object_key = key ? key : 1;
    (void)p;
    s.transforms_made++;
    return id;
}

int32_t sext11(uint32_t v) {
    int32_t x = (int32_t)(v & 0x7FFu);
    return (x & 0x400) ? x - 0x800 : x;
}

void close_frame(State &s) {
    std::unordered_set<uint32_t> vids, tids, objs;
    SceneCaptureStats st{};
    for (ScenePrim &p : s.prims) {
        uint32_t linked = 0;
        for (uint32_t i = 0; i < p.nverts; i++) {
            if (p.vertex[i] == SCENE_NO_VERTEX) continue;
            const SceneVertex *v = vertex_by_id(s, p.vertex[i]);
            if (!v) { p.vertex[i] = SCENE_NO_VERTEX; continue; }
            linked++;
            vids.insert(v->id);
            if (const SceneTransform *t = transform_by_id(s, v->transform)) {
                tids.insert(t->id);
                if (!p.object_key) p.object_key = t->object_key;
            }
        }
        if (p.object_key) objs.insert(p.object_key);
        st.prims++;
        if (p.kind == SCENE_PRIM_POLY) {
            st.polys++;
            if (linked == p.nverts) st.polys_linked++;
            else if (linked) st.polys_partial++;
            else st.polys_unlinked++;
        } else if (p.kind == SCENE_PRIM_LINE) {
            st.lines++;
        } else {
            st.rects++;
            if (linked) st.rects_linked++;
        }
    }
    s.f_vertices.clear();
    for (uint32_t id : vids) s.f_vertices.push_back(*vertex_by_id(s, id));
    std::sort(s.f_vertices.begin(), s.f_vertices.end(),
              [](const SceneVertex &a, const SceneVertex &b) { return a.id < b.id; });
    s.f_transforms.clear();
    for (uint32_t id : tids) s.f_transforms.push_back(*transform_by_id(s, id));
    std::sort(s.f_transforms.begin(), s.f_transforms.end(),
              [](const SceneTransform &a, const SceneTransform &b) { return a.id < b.id; });
    s.f_prims.swap(s.prims);
    s.prims.clear();

    s.frames++;
    st.frames = s.frames;
    st.frame_number = (uint32_t)s.frames;
    st.vblank_open = s.vblank_open;
    st.vblank_close = s.vblank;
    st.vertices = (uint32_t)s.f_vertices.size();
    st.transforms = (uint32_t)s.f_transforms.size();
    st.objects = (uint32_t)objs.size();
    st.ot_walks = s.frame_ot_walks;
    st.draw_calls = s.draw_calls;
    st.stale_links = s.stale;
    st.ambiguous_links = s.ambiguous;
    st.projections_total = s.projections;
    st.transforms_total = s.transforms_made;
    s.f_stats = st;

    s.frame_ot_walks = 0;
    s.draw_calls = 0;
    s.stale = 0;
    s.ambiguous = 0;
    s.ra_ordinal.clear();
    /* Drop SXY entries that fell out of the window so the map stays small. */
    if (s.by_sxy.size() > (kVertexCap >> 1)) {
        for (auto it = s.by_sxy.begin(); it != s.by_sxy.end();) {
            const SceneVertex *v = vertex_by_id(s, it->second);
            if (!v || s.vblank - v->vblank > s.window) it = s.by_sxy.erase(it);
            else ++it;
        }
    }
}

uint32_t link(State &s, uint32_t word) {
    auto it = s.by_sxy.find(word);
    if (it == s.by_sxy.end()) return SCENE_NO_VERTEX;
    const SceneVertex *v = vertex_by_id(s, it->second);
    if (!v || s.vblank - v->vblank > s.window) { s.stale++; return SCENE_NO_VERTEX; }
    if (v->ambiguous) s.ambiguous++;
    return v->id;
}


}  // namespace

extern "C" {

void scene_capture_set_enabled(int on) {
    if (on && !g_scene_capture_on) scene_capture_reset();
    g_scene_capture_on = on ? 1 : 0;
}

void scene_capture_reset(void) {
    State &s = S();
    const uint32_t window = s.window;
    const std::string game = s.adapter_game;
    const uint32_t vb = s.vblank;
    s.~State();
    new (&s) State();
    s.window = window;
    s.adapter_game = game;
    s.vblank = vb;
}

void scene_capture_set_link_window(uint32_t vblanks) {
    S().window = vblanks < 1 ? 1 : (vblanks > 60 ? 60 : vblanks);
}

uint32_t scene_capture_link_window(void) { return S().window; }

const char *scene_capture_adapter_game(void) { return S().adapter_game.c_str(); }

void scene_capture_set_adapter_game(const char *game) { S().adapter_game = game ? game : ""; }

void scene_capture_gte_command(uint32_t caller_ra) { S().caller_ra = caller_ra; }

void scene_capture_note_rot_matrix(uint32_t matrix_addr) { S().pending_rot = matrix_addr; }
void scene_capture_note_trans_matrix(uint32_t matrix_addr) { S().pending_trans = matrix_addr; }
void scene_capture_note_draw_call(uint32_t, uint32_t) { S().draw_calls++; }

void scene_capture_gte_project(const SceneGteProjection *p) {
    if (!p) return;
    State &s = S();
    TransformKeyFields k{};
    std::memcpy(k.rt, p->rt, sizeof k.rt);
    std::memcpy(k.tr, p->tr, sizeof k.tr);
    k.h = p->h; k.ofx = p->ofx; k.ofy = p->ofy;
    std::memcpy(k.l, p->l, sizeof k.l);
    std::memcpy(k.lc, p->lc, sizeof k.lc);
    std::memcpy(k.bk, p->bk, sizeof k.bk);
    std::memcpy(k.fc, p->fc, sizeof k.fc);
    k.dqa = p->dqa; k.dqb = p->dqb;
    const SceneTransform *cur = s.have_transform ? transform_by_id(s, s.next_transform - 1) : nullptr;
    const bool same_src = cur && cur->rot_src == s.pending_rot && cur->trans_src == s.pending_trans &&
                          cur->caller_ra == s.caller_ra;
    uint32_t tid;
    if (cur && same_src && k == s.last) {
        tid = cur->id;
    } else {
        tid = new_transform(s, *p, k);
        s.last = k;
        s.have_transform = true;
    }

    const uint32_t id = s.next_vertex++;
    SceneVertex &v = s.vertices[id % kVertexCap];
    v = {};
    v.id = id;
    v.transform = tid;
    v.vblank = s.vblank;
    std::memcpy(v.v, p->v, sizeof v.v);
    std::memcpy(v.view, p->view, sizeof v.view);
    v.sx16 = p->sx16; v.sy16 = p->sy16;
    v.packed_sxy = p->packed_sxy;
    v.sz = p->sz;
    s.projections++;

    auto it = s.by_sxy.find(p->packed_sxy);
    if (it != s.by_sxy.end()) {
        SceneVertex *old = const_cast<SceneVertex *>(vertex_by_id(s, it->second));
        if (old && s.vblank - old->vblank <= s.window &&
            (old->view[0] != v.view[0] || old->view[1] != v.view[1] || old->view[2] != v.view[2])) {
            old->ambiguous = 1;
            v.ambiguous = 1;
        }
        it->second = id;
    } else {
        s.by_sxy.emplace(p->packed_sxy, id);
    }
}

void scene_capture_ot_begin(uint32_t) {
    State &s = S();
    s.in_ot = true;
    s.ot_walk = s.frame_ot_walks++;
    s.ot_rank = 0;
}

void scene_capture_ot_node(uint32_t, uint32_t word_count) {
    if (word_count == 0) S().ot_rank++;
}

void scene_capture_ot_end(void) { S().in_ot = false; }

void scene_capture_gp0(const uint32_t *w, uint32_t count, uint32_t src_addr) {
    if (!w || count == 0) return;
    State &s = S();
    const uint8_t op = (uint8_t)(w[0] >> 24);
    if (op == 0xE1) { s.draw_mode = (uint16_t)(w[0] & 0x3FFFu); return; }
    if (op < 0x20 || op > 0x7F) return;
    if (s.prims.size() >= kMaxFramePrims) return;
    if (s.prims.empty()) s.vblank_open = s.vblank;

    ScenePrim p{};
    p.index = (uint32_t)s.prims.size();
    p.packet_addr = src_addr == 0xFFFFFFFFu ? 0 : src_addr;
    p.ot_index = s.in_ot ? s.ot_walk : 0xFFFFFFFFu;
    p.ot_rank = s.in_ot ? s.ot_rank : 0;
    p.opcode = op;
    p.semi_trans = (op >> 1) & 1;
    p.raw_texture = op & 1;
    p.texpage = s.draw_mode;
    for (uint32_t &vid : p.vertex) vid = SCENE_NO_VERTEX;

    uint32_t vw[SCENE_PRIM_MAX_VERTS] = {};
    uint32_t i = 1;
    auto word = [&](uint32_t n) { return n < count ? w[n] : 0u; };
    if (op < 0x40) {                                    /* polygon */
        p.kind = SCENE_PRIM_POLY;
        p.gouraud = (op >> 4) & 1;
        p.textured = (op >> 2) & 1;
        p.nverts = (op & 0x08) ? 4 : 3;
        for (uint32_t k = 0; k < p.nverts; k++) {
            if (p.gouraud && k > 0) i++;
            vw[k] = word(i++);
            if (p.textured) {
                const uint32_t uv = word(i++);
                if (k == 0) p.clut = (uint16_t)(uv >> 16);
                if (k == 1) p.texpage = (uint16_t)(uv >> 16);
            }
        }
    } else if (op < 0x60) {                             /* line, first segment */
        p.kind = SCENE_PRIM_LINE;
        p.gouraud = (op >> 4) & 1;
        p.nverts = 2;
        vw[0] = word(1);
        vw[1] = word(p.gouraud ? 3 : 2);
    } else {                                            /* rectangle */
        p.kind = SCENE_PRIM_RECT;
        p.textured = (op >> 2) & 1;
        p.nverts = 1;
        vw[0] = word(1);
        if (p.textured) p.clut = (uint16_t)(word(2) >> 16);
    }
    for (uint32_t k = 0; k < p.nverts; k++) {
        p.x[k] = (int16_t)sext11(vw[k]);
        p.y[k] = (int16_t)sext11(vw[k] >> 16);
        p.vertex[k] = link(s, vw[k]);
    }
    s.prims.push_back(p);
}

void scene_capture_vblank(void) {
    State &s = S();
    if (!s.prims.empty()) close_frame(s);
    s.vblank++;
}

/* PSX_SCENE_CAPTURE_FRAMES="<on>,<off>" turns capture on at guest frame <on>
 * and off at <off>, from boot, so a file replay captures the same frame every
 * run without a TCP command racing the route. The last closed frame stays
 * readable after <off>. Capture is host memory only; the guest never sees it. */
void scene_capture_frame_boundary(uint64_t frame) {
    static int parsed = 0;
    static uint64_t on_at = 0, off_at = 0;
    if (!parsed) {
        parsed = 1;
        const char *env = std::getenv("PSX_SCENE_CAPTURE_FRAMES");
        unsigned long long a = 0, b = 0;
        if (env && std::sscanf(env, "%llu,%llu", &a, &b) == 2 && a > 0 && b > a) {
            on_at = a;
            off_at = b;
        }
    }
    if (!on_at) return;
    if (frame == on_at) scene_capture_set_enabled(1);
    else if (frame == off_at) scene_capture_set_enabled(0);
}

void scene_capture_stats(SceneCaptureStats *out) {
    if (out) *out = S().f_stats;
}

uint32_t scene_capture_frame_prims(const ScenePrim **out) {
    if (out) *out = S().f_prims.data();
    return (uint32_t)S().f_prims.size();
}

uint32_t scene_capture_frame_vertices(const SceneVertex **out) {
    if (out) *out = S().f_vertices.data();
    return (uint32_t)S().f_vertices.size();
}

uint32_t scene_capture_frame_transforms(const SceneTransform **out) {
    if (out) *out = S().f_transforms.data();
    return (uint32_t)S().f_transforms.size();
}

}  // extern "C"
