/* The debug present ring across a renderer restart, on a real hidden GL
 * context (from the fmv_chroma fixture header).
 *
 * A synthetic 4:2:0-style 24-bit frame (constant luma, chroma constant per
 * 2x2 block and flipping between saturated hues block to block, plus a sharp
 * luma edge on the right) is presented through gl_renderer_present, the
 * CPU/24-bit present path:
 *  - off, and on for a frame that is not a 24-bit scanout, are byte-identical;
 *  - on, chroma steps between neighbouring output pixels shrink a lot while
 *    luma stays (the flat-luma area keeps its luma within a few levels, and
 *    the sharp luma edge stays as sharp);
 *  - the smoothed-present counter counts only 24-bit presents.
 * Original source-owned scene; no retail payload. */
#define GL_PRESENT_TEST_HOOK(gen) cap_hook(gen)
static void cap_hook(int gen);
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;(void)w;(void)h;(void)p;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int x,int y,int w,int h){(void)sx;(void)sy;(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
#ifndef PSX_NO_DEBUG_TOOLS
int present_image_ring_accepting(void){return 1;}
int present_image_ring_thumb_w(int w,int h){(void)h;return w > 0 ? 64 : 0;}
void present_image_ring_push(uint32_t f,const uint8_t* p,int w,int pitch,int flip){(void)f;(void)p;(void)w;(void)pitch;(void)flip;}
void present_image_ring_push_argb(uint32_t f,const uint32_t* p,int w,int h,int pitch){(void)f;(void)p;(void)w;(void)h;(void)pitch;}
#endif
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
void psx_host_sleep_micros(unsigned us) { SDL_Delay((us + 999u) / 1000u); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }


static void cap_hook(int gen) { (void)gen; }

#define SW 64
#define SH 48
static uint32_t src[SW * SH];

/* Gpu tests run without PSX_NO_DEBUG_TOOLS, so the ring capture is live. */
static SDL_Window *open_ctx(void) {
    SDL_Window *win = SDL_CreateWindow("present ring restart test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) return NULL;
    glb_init(vram); glb_set_scale(1); gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { SDL_DestroyWindow(win); return NULL; }
    return win;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    for (int i = 0; i < SW * SH; i++) src[i] = 0xFF336699u;
    SDL_Window *win = open_ctx();
    if (!win) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    for (int i = 0; i < 3; i++) gl_renderer_present(src, SW, SH, 0, 0, 0);
    check(s_pir_fbo != 0 && s_pir_pbo[0] != 0, "ring objects made in the first context");
    gl_renderer_shutdown();
    /* The context and its objects are gone: no name may survive. A stale
     * pack-buffer name made the next readback write through a null client
     * pointer (glReadPixels crash after a lobby rematch). */
    check(s_pir_fbo == 0 && s_pir_tex == 0 && s_pir_pbo[0] == 0 && s_pir_pbo[1] == 0,
          "ring names forgotten at shutdown");
    check(s_pir_pending[0] == 0 && s_pir_pending[1] == 0, "no readback pending across contexts");
    SDL_DestroyWindow(win);
    win = open_ctx();
    if (!win) { fprintf(stderr, "SKIP no second GL context\n"); return 77; }
    for (int i = 0; i < 3; i++) gl_renderer_present(src, SW, SH, 0, 0, 0);
    check(s_pir_fbo != 0 && s_pir_pbo[0] != 0, "ring rebuilt in the new context");
    check(glIsTexture(s_pir_tex) == GL_TRUE, "ring texture is a live object of the new context");
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
