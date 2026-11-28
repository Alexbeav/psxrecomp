#ifndef SCENE_CAPTURE_H
#define SCENE_CAPTURE_H

/* Typed scene capture: what the game drew, with the 3D data behind it.
 *
 * The PS1 has no scene graph. A game multiplies each vertex by a GTE matrix,
 * stores the projected screen position into a GPU packet, and links the
 * packet into an ordering table that DMA channel 2 walks into GP0. This
 * module records both ends and joins them:
 *
 *   GTE side  (gte.cpp, RTPS/RTPT): one vertex record per projection, with
 *             the model-space input V, the view-space result (MAC1-3 before
 *             the divide), the 16.16 screen position, SZ, and the id of the
 *             transform in force. A transform is a snapshot of RT, TR, H,
 *             OFX/OFY, the light and colour matrices, BK, FC and DQA/DQB,
 *             taken whenever any of them changed since the last projection.
 *   GPU side  (gpu.c, GP0 execute): one primitive record per draw command,
 *             with its packet address and ordering-table rank (from the DMA2
 *             walk), its material (texpage, CLUT, blend, textured, raw), and
 *             its screen vertices.
 *   Join:     a primitive vertex links to the newest vertex record whose
 *             packed SXY word is exactly the vertex word in the packet, if
 *             that record is no older than the adapter's link window. Games
 *             build packets one frame and draw them the next, so the window
 *             spans several VBlanks.
 *
 * A frame closes at the first VBlank after at least one primitive. The last
 * closed frame is kept whole for the debug server (scene_capture,
 * scene_dump); nothing is written to disk.
 *
 * Objects: a transform carries the guest return address of the code that
 * issued the projection and, where the adapter hooks SetRotMatrix /
 * SetTransMatrix, the guest address of the MATRIX the game passed. The
 * object key is a hash of those, so it stays the same across frames while
 * the game keeps drawing that object from the same place.
 *
 * Cost and safety: every hook is behind the single global
 * g_scene_capture_on, off by default. Capture is host memory only; it never
 * writes guest state, so gameplay, timing and saves are unchanged. Replay
 * sandboxes and speculative GTE runs are not recorded (the caller checks).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_scene_capture_on;

/* Inputs of one projection, filled by gte.cpp. Arrays point at GTEState. */
typedef struct SceneGteProjection {
    const int16_t *v;          /* V0/V1/V2 vector projected (model space) */
    int32_t view[3];           /* MAC1..3 accumulators >> 12 (view space) */
    int32_t sx16, sy16;        /* screen position, 16.16, before saturation */
    uint32_t packed_sxy;       /* the SXY2 word the game can store into a packet */
    uint16_t sz;               /* SZ3 */
    const int16_t *rt;         /* RT[3][3] */
    const int32_t *tr;         /* TR[3] */
    const int16_t *l;          /* L[3][3] */
    const int16_t *lc;         /* LC[3][3] */
    const int32_t *bk;         /* BK[3] */
    const int32_t *fc;         /* FC[3] */
    uint16_t h;
    int32_t ofx, ofy;
    int16_t dqa;
    int32_t dqb;
} SceneGteProjection;

typedef struct SceneTransform {
    uint32_t id;               /* monotonic */
    uint32_t caller_ra;        /* guest ra when the GTE command ran */
    uint32_t rot_src;          /* guest MATRIX* passed to SetRotMatrix, or 0 */
    uint32_t trans_src;        /* guest MATRIX* passed to SetTransMatrix, or 0 */
    uint32_t object_key;
    int16_t  rt[9];
    int32_t  tr[3];
    uint16_t h;
    int32_t  ofx, ofy;
    int16_t  l[9], lc[9];
    int32_t  bk[3], fc[3];
    int16_t  dqa;
    int32_t  dqb;
} SceneTransform;

typedef struct SceneVertex {
    uint32_t id;               /* monotonic */
    uint32_t transform;        /* SceneTransform.id */
    uint32_t vblank;           /* VBlank count when projected */
    int16_t  v[3];
    int32_t  view[3];
    int32_t  sx16, sy16;
    uint32_t packed_sxy;
    uint16_t sz;
    uint8_t  ambiguous;        /* an earlier vertex in the window had the same SXY and a different view position */
} SceneVertex;

enum {
    SCENE_PRIM_POLY = 1,
    SCENE_PRIM_LINE = 2,
    SCENE_PRIM_RECT = 3,
};

#define SCENE_PRIM_MAX_VERTS 4
#define SCENE_NO_VERTEX 0xFFFFFFFFu

typedef struct ScenePrim {
    uint32_t index;            /* order of submission in the frame */
    uint32_t packet_addr;      /* guest address of the command's first word, or 0 */
    uint32_t ot_index;         /* which ordering-table walk of the frame (0-based) */
    uint32_t ot_rank;          /* empty-node count before it in that walk */
    uint8_t  opcode;
    uint8_t  kind;             /* SCENE_PRIM_* */
    uint8_t  nverts;
    uint8_t  textured, semi_trans, raw_texture, gouraud;
    uint16_t texpage;          /* GP0(E1)-format bits for this draw */
    uint16_t clut;
    int16_t  x[SCENE_PRIM_MAX_VERTS], y[SCENE_PRIM_MAX_VERTS];
    uint32_t vertex[SCENE_PRIM_MAX_VERTS];   /* SceneVertex.id or SCENE_NO_VERTEX */
    uint32_t object_key;       /* of the first linked vertex's transform, or 0 */
} ScenePrim;

typedef struct SceneCaptureStats {
    uint64_t frames;           /* frames closed */
    uint32_t frame_number;     /* the kept frame's number */
    uint32_t vblank_open, vblank_close;
    uint32_t prims, polys, lines, rects;
    uint32_t polys_linked;     /* every vertex linked */
    uint32_t polys_partial;    /* some, not all */
    uint32_t polys_unlinked;
    uint32_t rects_linked;     /* sprite anchored at a projected point */
    uint32_t vertices;         /* records referenced by the frame */
    uint32_t transforms;       /* referenced by the frame */
    uint32_t objects;          /* distinct object keys in the frame */
    uint32_t ot_walks;
    uint32_t draw_calls;       /* DrawOTag-family entries seen (adapter hook) */
    uint32_t stale_links;      /* vertex word known, record older than the window */
    uint32_t ambiguous_links;
    uint64_t projections_total;
    uint64_t transforms_total;
} SceneCaptureStats;

/* Control. */
void scene_capture_set_enabled(int on);
void scene_capture_reset(void);
void scene_capture_set_link_window(uint32_t vblanks);   /* default 4 */
uint32_t scene_capture_link_window(void);

/* Adapter: per-game TOML (see docs/scene_capture.md). Returns 1 when the file
 * was read; on error writes a message into err and leaves the old adapter. */
int scene_capture_load_adapter(const char *path, char *err, uint32_t err_size);
const char *scene_capture_adapter_game(void);
void scene_capture_set_adapter_game(const char *game);
const char *scene_capture_adapter_error(void);   /* last load error, or "" */

/* GTE side (gte.cpp). caller_ra is set once per GTE command. */
void scene_capture_gte_command(uint32_t caller_ra);
void scene_capture_gte_project(const SceneGteProjection *p);
/* libgte entry hooks (adapter): the guest MATRIX* in a0. */
void scene_capture_note_rot_matrix(uint32_t matrix_addr);
void scene_capture_note_trans_matrix(uint32_t matrix_addr);
void scene_capture_note_draw_call(uint32_t function_addr, uint32_t ot_addr);

/* GPU side (dma.c / gpu.c). */
void scene_capture_ot_begin(uint32_t start_addr);
void scene_capture_ot_node(uint32_t node_addr, uint32_t word_count);
void scene_capture_ot_end(void);
/* words = one complete GP0 command; src_addr = guest address of its first
 * word, or 0xFFFFFFFF when it did not come from RAM by DMA. */
void scene_capture_gp0(const uint32_t *words, uint32_t count, uint32_t src_addr);
void scene_capture_vblank(void);
/* Once per guest frame, after the frame counter advances. Applies the
 * PSX_SCENE_CAPTURE_FRAMES="<on>,<off>" window from boot. */
void scene_capture_frame_boundary(uint64_t frame);

/* Readout of the last closed frame. Returned pointers stay valid until the
 * next frame closes or the capture is reset. */
void scene_capture_stats(SceneCaptureStats *out);
uint32_t scene_capture_frame_prims(const ScenePrim **out);
uint32_t scene_capture_frame_vertices(const SceneVertex **out);
uint32_t scene_capture_frame_transforms(const SceneTransform **out);

#ifdef __cplusplus
}
#endif

#endif /* SCENE_CAPTURE_H */
