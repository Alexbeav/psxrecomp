/* Inactive modern enhancement seams from gpu_exec_stubs.inc; source device owners are real. */
/* Shared link stubs for tests that compile runtime/src/gpu.c directly and
 * execute GP0 packets without a GL context, mod bank or emulator core.
 * Include right after "../src/gpu.c". Define GPU_EXEC_REAL_UI_GROUP before
 * including to link the real ws_ui_group.c instead of the grouping stub.
 * Draw calls that tests observe are recorded below. */

uint64_t s_frame_count;
uint64_t psx_cycle_count;          /* gpu_timeline.c stamps notes with it */
int g_exec_phase;
int g_psx_render_pass_active;
void psx_mod_counter_add(const char *name, uint32_t delta) { (void)name; (void)delta; }
static int fixture_pgxp_enabled;
int pgxp_enabled(void) { return fixture_pgxp_enabled; }
static int fixture_pgxp_cpu_mode;
int pgxp_cpu_mode(void) { return fixture_pgxp_cpu_mode; }
void pgxp_set_cpu_mode(int enabled) { fixture_pgxp_cpu_mode=enabled; }
int pgxp_projective_scope_begin(PGXPProjectiveScope *scope) {
    if (!scope || scope->active) return 0;
    scope->saved_enabled=fixture_pgxp_enabled;
    scope->saved_cpu_mode=fixture_pgxp_cpu_mode;
    fixture_pgxp_enabled=1;
    fixture_pgxp_cpu_mode=1;
    scope->active=1;
    return 1;
}
void pgxp_projective_scope_end(PGXPProjectiveScope *scope) {
    if (!scope || !scope->active) return;
    fixture_pgxp_cpu_mode=scope->saved_cpu_mode;
    fixture_pgxp_enabled=scope->saved_enabled;
    scope->active=0;
}
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
CPUState *debug_cpu_ptr;

uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;

/* This fixture exercises stock GPU packets without a GL context or mod bank.
 * Keep the gpu.c dependencies explicit, including on linkers that retain
 * additional exported command handlers despite --gc-sections. */
static GrBackend fixture_backend = GR_BACKEND_SOFTWARE;

int gl_renderer_texture_banks_supported(void) { return 0; }
static int fixture_projective_supported, fixture_projective_draws;
static PSXProjectedVertex fixture_projective_vertices[3];
int gl_renderer_projective_supported(void) { return fixture_projective_supported; }
void gl_renderer_draw_projected_triangle(const PSXProjectedVertex v[3],
    uint16_t tp, uint16_t cx, uint16_t cy, int raw, int semi, int perspective) {
    memcpy(fixture_projective_vertices,v,sizeof fixture_projective_vertices);
    fixture_projective_draws++;
    (void)tp;(void)cx;(void)cy;(void)raw;(void)semi;(void)perspective;
}
static struct { uint32_t addr,word; PGXPProjection p; int valid; } fixture_homogeneous[4];
int pgxp_load_projection(uint32_t addr, uint32_t word, PGXPProjection *out) {
    for(int i=0;i<4;++i) if(fixture_homogeneous[i].valid &&
        fixture_homogeneous[i].addr==addr && fixture_homogeneous[i].word==word) {
        if(out)*out=fixture_homogeneous[i].p;
        return 1;
    }
    return 0;
}
float pgxp_get_unclamped_depth(uint32_t addr, uint32_t word) {
    PGXPProjection projection;
    return pgxp_load_projection(addr, word, &projection) ? projection.z : 0.0f;
}
static int fixture_projection_tracking;
int pgxp_projection_tracking(void) { return fixture_projection_tracking; }
void pgxp_set_projection_tracking(int enabled) { fixture_projection_tracking=!!enabled; }
int gl_renderer_select_texture_bank(uint16_t id) { return id == 0; }
int gl_renderer_select_texture_bank_live_clut(uint16_t id) { return id == 0; }
uint16_t mod_texture_packet_bank(uint32_t source, const uint32_t *words,
                                 uint32_t count) {
    (void)source; (void)words; (void)count;
    return 0;
}
int mod_texture_packet_precision(uint32_t source, float q[3], float xy[6]) {
    (void)source; (void)q; (void)xy;
    return 0;
}

static uint32_t test_ram[0x00200000u / 4u];
/* The enhancement GPU-DMA aperture (mod_memory.h): a test allocates it by
 * setting test_aperture_used; reads and keys then follow the real rules. */
static uint32_t test_aperture[0x00020000u / 4u];
static uint32_t test_aperture_used;

static struct {
    int calls;
    int x, y, w, h;
    int u, v;
    uint16_t clut_x, clut_y, texpage;
} last_textured_rect;
static struct { int calls, x, y, w, h, u0, v0, u1, v1; } last_scaled_rect;

/* PGXP stub state: the quad case in main() switches geometry correction on
 * and makes the packet words in a bitmask (bit n = word n at g_test_quad_base)
 * report a dataflow-precise vertex. */
static int g_test_geometry_correction = 0;
static uint32_t g_test_quad_base = 0xFFFFFFFFu;
static uint32_t g_test_precise_words = 0;
static int g_test_rect_bypass = 0;
static int g_test_rect_partial = 0;

static int test_word_precise(uint32_t addr) {
    if (g_test_quad_base == 0xFFFFFFFFu || addr < g_test_quad_base) return 0;
    uint32_t word = (addr - g_test_quad_base) / 4u;
    return word < 32u && ((g_test_precise_words >> word) & 1u);
}
static int g_test_precise_triangles = 0;
static int g_test_textured_triangles = 0;

/* Bounding box of every flat/gouraud triangle drawn since the last reset. */
static struct { int calls, min_x, max_x, min_y, max_y; } gpu_exec_triangles;
static inline void gpu_exec_reset_triangles(void) {
    gpu_exec_triangles.calls = 0;
    gpu_exec_triangles.min_x = gpu_exec_triangles.min_y = 1 << 30;
    gpu_exec_triangles.max_x = gpu_exec_triangles.max_y = -(1 << 30);
}
static inline void gpu_exec_record_triangle(int x0, int y0, int x1, int y1,
                                     int x2, int y2) {
    const int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    gpu_exec_triangles.calls++;
    for (int i = 0; i < 3; i++) {
        if (xs[i] < gpu_exec_triangles.min_x) gpu_exec_triangles.min_x = xs[i];
        if (xs[i] > gpu_exec_triangles.max_x) gpu_exec_triangles.max_x = xs[i];
        if (ys[i] < gpu_exec_triangles.min_y) gpu_exec_triangles.min_y = ys[i];
        if (ys[i] > gpu_exec_triangles.max_y) gpu_exec_triangles.max_y = ys[i];
    }
}

uint32_t psx_read_word(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu, off;
    if (psx_mod_gpu_dma_aperture_offset_for(phys, 4u, test_aperture_used, &off))
        return test_aperture[off / 4u];
    if (phys <= 0x00200000u - 4u)
        return test_ram[phys / 4u];
    return 0;
}

uint16_t psx_read_half(uint32_t addr) {
    uint32_t word = psx_read_word(addr & ~3u);
    return (uint16_t)(word >> ((addr & 2u) ? 16 : 0));
}

uint8_t psx_read_byte(uint32_t addr) {
    uint32_t word = psx_read_word(addr & ~3u);
    return (uint8_t)(word >> ((addr & 3u) * 8u));
}

void psx_write_half(uint32_t addr, uint16_t val) {
    (void)addr;
    (void)val;
}

void psx_host_write_half(uint32_t addr, uint16_t val) {
    psx_write_half(addr, val);
}



void psx_fatal_halt(const char *reason) {
    fprintf(stderr,"source fixture halt: %s\n",reason);exit(1);
}

void pgxp_set_enabled(int enabled) { fixture_pgxp_enabled = enabled; }
int gte_geometry_correction_enabled(void) { return g_test_geometry_correction; }
int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                            int32_t int_x, int32_t int_y,
                            int32_t *x16, int32_t *y16, uint16_t *sz) {
    (void)packet_word;
    *x16 = int_x * 65536;
    *y16 = int_y * 65536;
    *sz = 0;
    if (test_word_precise(addr)) {
        *x16 += 0x4000;
        return PGXP_SRC_DATAFLOW;
    }
    return PGXP_SRC_NATIVE;
}
static struct {
    uint32_t addr, word;
    int32_t x16, y16;
    uint16_t z;
    int valid;
} fixture_projections[4];
int pgxp_probe_precise_vertex(uint32_t addr, uint32_t packet_word,
                              int32_t int_x, int32_t int_y) {
    (void)packet_word; (void)int_x; (void)int_y;
    return test_word_precise(addr) ? PGXP_SRC_DATAFLOW : PGXP_SRC_NATIVE;
}
void pgxp_note_triangle(int precise) { (void)precise; }
void pgxp_note_triangle_detail(const PGXPTriRecord *rec) { (void)rec; }
void pgxp_note_rect_bypass(int all_precise) {
    if (all_precise) g_test_rect_bypass++;
    else             g_test_rect_partial++;
}
int gte_precision_load_word(uint32_t addr, uint32_t packed,
                            int32_t *x16, int32_t *y16, uint16_t *z) {
    for (unsigned i = 0; i < 4; ++i) {
        if (fixture_projections[i].valid && fixture_projections[i].addr == addr &&
            fixture_projections[i].word == packed) {
            if (x16) *x16 = fixture_projections[i].x16;
            if (y16) *y16 = fixture_projections[i].y16;
            if (z) *z = fixture_projections[i].z;
            return fixture_projections[i].z != 0;
        }
    }
    return 0;
}


uint32_t psx_ws_widen_angle_q12(uint32_t vanilla, int extent_pixels) {
    (void)extent_pixels;
    return vanilla;
}
int psx_ws_aspect_cone_contains(int32_t x, int32_t z, int32_t y,
                                int32_t fx, int32_t fz, int32_t fy,
                                uint32_t threshold, int extent_pixels) {
    (void)x; (void)z; (void)y; (void)fx; (void)fz; (void)fy;
    (void)threshold; (void)extent_pixels;
    return 0;
}
#ifndef GPU_EXEC_REAL_UI_GROUP
void ws_ui_group_assign(WsUiGroupItem *items, size_t count,
                        int32_t display_width, int dense_menu, int in_place) {
    (void)items; (void)count; (void)display_width; (void)dense_menu;
    (void)in_place;
}
int32_t ws_ui_anchor_for_bounds(int32_t x, int32_t width,
                                int32_t display_width) {
    (void)width;
    if (x * 3 < display_width) return -1;
    if (x * 3 > display_width * 2) return 1;
    return 0;
}
#endif




void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}

int psx_netplay_active(void) { return 0; }
int sio_hold_present_for_card(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
void mod_runtime_on_vblank(void) {}
bool screen_kind_from_name(const char *name, ScreenKind *out) {
    (void)name;
    if (out) *out = SCREEN_RAW;
    return true;
}
ColorLut *color_lut_create(const ColorSettings *settings) {
    (void)settings;
    return NULL;
}
void color_lut_destroy(ColorLut *lut) { (void)lut; }
bool color_lut_is_passthrough(const ColorLut *lut) {
    (void)lut;
    return true;
}
void color_lut_map555(const ColorLut *lut, uint16_t bgr555,
                      uint8_t *r, uint8_t *g, uint8_t *b) {
    (void)lut;
    if (r) *r = (uint8_t)((bgr555 & 0x1Fu) << 3);
    if (g) *g = (uint8_t)(((bgr555 >> 5) & 0x1Fu) << 3);
    if (b) *b = (uint8_t)(((bgr555 >> 10) & 0x1Fu) << 3);
}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t address) {
    return psx_mod_gpu_dma_resolve_address_for(address, test_aperture_used);
}
int psx_mod_gpu_dma_memory_contains(uint32_t address, uint32_t bytes) {
    return bytes && psx_mod_gpu_dma_aperture_offset_for(
        address, bytes, test_aperture_used, NULL);
}
uint32_t psx_gpu_packet_key(uint32_t address) {
    return psx_gpu_packet_key_for(address, test_aperture_used);
}
void text_xlate_vram_upload(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
uint32_t debug_guest_ra(void) { return 0; }
uint32_t debug_guest_sp(void) { return 0; }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)test_ram; }











static struct { int enabled; int32_t xy[6]; } fixture_precise_triangle;




























static struct { int enabled, shift, left, right; } fixture_wide_view;








int gte_nclip_native_wide_sign(int32_t mac0, int* sign) {
    (void)mac0; (void)sign; return 0;
}
int gte_nclip_native_wide_previous_sign(int32_t mac0, int* sign) {
    (void)mac0; (void)sign; return 0;
}
int gte_nclip_zero_positive(int32_t mac0, uint32_t pc) {
    (void)mac0; (void)pc; return 0;
}
static int fixture_triangle_recovered;
void gl_renderer_note_wide_triangle_recovery(int enabled) {
    fixture_triangle_recovered=enabled;
}
/* Smooth motion (frame generation) vertex sources: off in these fixtures. */
int gl_renderer_frame_generation(void) { return 0; }
/* PGXP renderer features: off in these fixtures. */
int gl_renderer_pgxp_render_wanted(void) { return 0; }
void gl_renderer_fg_source(const uint32_t id[3], const int32_t pc[9], const int32_t h[3],
                           const int32_t x[3], const int32_t y[3]) {
    (void)id; (void)pc; (void)h; (void)x; (void)y;
}
void gte_fg_source_set(int enabled) { (void)enabled; }
int gte_fg_source_enabled(void) { return 0; }
int gte_fg_source_lookup(uint32_t packed, GteFgSrc out[2]) { (void)packed; (void)out; return 0; }
