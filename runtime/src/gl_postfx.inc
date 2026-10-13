/* ---- Post-effect pipeline (docs/POST_EFFECTS.md) ---------------------------
 * One executor for every post effect on the composed game image: chains of
 * full-screen fragment passes (postfx_chain.h). Three stages, in order, each
 * a chain that reads a fresh copy of the image and writes it back:
 *   1. bloom      built-in "bloom" preset, [video] bloom (strength uniform)
 *   2. user       [video] post_shader / PSX_POST_SHADER: any .pfx preset
 *   3. AA         built-in "fxaa"/"fxaa_hq" preset, [video] antialiasing_mode
 * All are elective and off by default; with no stage active nothing here
 * runs. Like the former fixed passes this runs in the window rect before
 * hold-last capture and the OSD, on whichever thread presents (the render
 * thread included), and Smooth motion's generated frames run stages 1 and 2.
 *
 * Inputs a pass may read: `source` (the composed image, one shared copy),
 * `mask` (optional: the primitive-derived "may glow" mask, see below), and
 * earlier passes' targets. The executor owns target allocation and lifetime,
 * program build/cache, uniform binding (u_rcp, u_size, u_org and the preset's
 * uniforms) and GL state: everything the pass touches is put back.
 *
 * The mask. While an active chain reads `mask`, every draw keeps a mark in
 * stencil bit 1 of the surface it draws into (BCLS_DRAW in gpu_gl_renderer.c):
 * set by 3D (PGXP-proven) and additive primitives, cleared by opaque 2D ones
 * (sprites, fills, the HUD). The marks travel with the picture (native-wide
 * centre copies, Smooth motion surfaces). At the present they are blitted
 * from the presented surface into a window-sized stencil and turned into an
 * R8 texture (1 = lit). A temporal-blending in-between present reuses the last
 * mask. A chain that does not read `mask` costs none of this. */
/* The GL-free preset parser is compiled into this translation unit (every
 * build and GL fixture that compiles the renderer gets it with no extra
 * source); runtime/tests/test_postfx_chain.c builds it alone. */
#include "postfx_chain.c"

typedef struct {
    PfxChain chain;
    int      loaded, failed;
    GLuint   prog[PFX_MAX_PASSES];
    GLuint   tex[PFX_MAX_PASSES], fbo[PFX_MAX_PASSES];
    int      tw[PFX_MAX_PASSES], th[PFX_MAX_PASSES];
} PfxStage;
enum { PFX_STAGE_BLOOM, PFX_STAGE_USER, PFX_STAGE_AA, PFX_STAGE_HQ, PFX_NSTAGES };
static PfxStage s_pfx[PFX_NSTAGES];
static char   s_pfx_user_path[1024] = "";
static int    s_pfx_user_env = 0;

static float  s_bloom_thr = 0.55f;          /* bloom bright-pass threshold */
static int    s_bloom_view = 0;             /* debug: 1 bloom only, 2 split */
static uint64_t s_bloom_passes = 0;
static int    s_bloom_time = 0;             /* debug: glFinish-bracketed cost */
static double s_bloom_us_sum = 0.0; static uint64_t s_bloom_us_n = 0;   /* real frames */
static double s_bloom_gus_sum = 0.0; static uint64_t s_bloom_gus_n = 0; /* generated frames */
static uint64_t s_pfx_reused = 0;           /* passes skipped on generated frames */

/* The mask (see above). */
static GLuint s_mask_src_fbo = 0;
static int    s_mask_src[4];                /* x0,y0,x1,y1 in source FBO px (y0 = top) */
static int    s_mask_src_valid = 0, s_mask_ok = 0;
static GLuint s_mask_tex = 0, s_mask_rb = 0, s_mask_fbo = 0, s_mask_prog = 0;
static int    s_mask_w = 0, s_mask_h = 0;

static int pfx_stage_on(int i) {
    switch (i) {
    case PFX_STAGE_BLOOM: return s_bloom > 0.0f;
    case PFX_STAGE_USER:  return s_pfx_user_path[0] != 0;
    case PFX_STAGE_AA:    return s_post_aa == GL_POST_AA_FXAA;
    case PFX_STAGE_HQ:    return s_post_aa == GL_POST_AA_FXAA_HQ;
    }
    return 0;
}
static int postfx_active(void) {
    for (int i = 0; i < PFX_NSTAGES; i++) if (pfx_stage_on(i)) return 1;
    return 0;
}

static void pfx_stage_unload(PfxStage *st) {
    for (int i = 0; i < PFX_MAX_PASSES; i++) {
        if (s_ctx) {
            if (st->prog[i]) p_glDeleteProgram(st->prog[i]);
            if (st->fbo[i]) p_glDeleteFramebuffers(1, &st->fbo[i]);
            if (st->tex[i]) glDeleteTextures(1, &st->tex[i]);
        }
        st->prog[i] = st->tex[i] = st->fbo[i] = 0; st->tw[i] = st->th[i] = 0;
    }
    pfx_chain_free(&st->chain);
    st->loaded = st->failed = 0;
}

/* Which stages read the mask decides whether draws keep it. */
static void pfx_mask_wanted_update(void) {
    int want = 0;
    for (int i = 0; i < PFX_NSTAGES; i++) {
        if (!pfx_stage_on(i)) continue;
        if (i == PFX_STAGE_BLOOM) want = 1;          /* the bloom preset reads it */
        else if (s_pfx[i].loaded && s_pfx[i].chain.uses_mask) want = 1;
    }
    if (want != s_bcls_on) { s_bcls_on = want; pgxp_render_wanted_update(); }
}

static int pfx_stage_load(int i) {
    PfxStage *st = &s_pfx[i];
    if (st->loaded) return 1;
    if (st->failed) return 0;
    char err[256] = "";
    int ok;
    if (i == PFX_STAGE_USER) ok = pfx_chain_load_file(s_pfx_user_path, &st->chain, err, sizeof err);
    else ok = pfx_chain_builtin(i == PFX_STAGE_BLOOM ? "bloom" : i == PFX_STAGE_AA ? "fxaa" : "fxaa_hq",
                                &st->chain, err, sizeof err);
    if (!ok) {
        fprintf(stdout, "psxrecomp: post effect '%s' not loaded: %s\n",
                i == PFX_STAGE_USER ? s_pfx_user_path : "built-in", err);
        st->failed = 1;
        return 0;
    }
    st->loaded = 1;
    if (i == PFX_STAGE_USER)
        fprintf(stdout, "psxrecomp: post shader '%s' (%d passes%s)\n", s_pfx_user_path,
                st->chain.npasses, st->chain.uses_mask ? ", mask" : "");
    return 1;
}

static GLuint pfx_build_pass(const PfxPass *ps) {
    size_t n = strlen(ps->shader) + 512 + (size_t)ps->ninputs * 64;
    char *src = (char *)malloc(n);
    if (!src) return 0;
    int k = snprintf(src, n, "#version 330\nin vec2 v_uv; out vec4 o;\n"
                             "uniform vec2 u_rcp; uniform vec2 u_size; uniform ivec2 u_org;\n");
    for (int i = 0; i < ps->ninputs; i++)
        k += snprintf(src + k, n - (size_t)k, "uniform sampler2D %s;\n", ps->in[i].sampler);
    snprintf(src + k, n - (size_t)k, "%s", ps->shader);
    GLuint prog = build_program(PRESENT_VS, src);
    free(src);
    return prog;
}

/* The presented surface: its FBO and the displayed rect, in native units of
 * a texture tex_w x tex_h native units across (as present_target_quad). */
static void postfx_note_source(GLuint fbo, GLuint tex, float tex_w, float tex_h,
                               int x, int y, int w, int h) {
    s_mask_src_valid = 0;
    if (!s_bcls_on || !fbo || !tex || tex_w <= 0.0f || tex_h <= 0.0f) return;
    GLint tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glBindTexture(GL_TEXTURE_2D, 0);
    const float sx = (float)tw / tex_w, sy = (float)th / tex_h;
    s_mask_src_fbo = fbo;
    s_mask_src[0] = (int)lrintf((float)x * sx);
    s_mask_src[1] = (int)lrintf((float)y * sy);
    s_mask_src[2] = (int)lrintf((float)(x + w) * sx);
    s_mask_src[3] = (int)lrintf((float)(y + h) * sy);
    s_mask_src_valid = 1;
}

static void pfx_tex_alloc(GLuint *t, int w, int h, int fmt, int nearest) {
    if (!*t) glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    if (fmt == PFX_FMT_RGBA16F) glTexImage2D(GL_TEXTURE_2D, 0, 0x881A /*RGBA16F*/, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    else if (fmt == -1)         glTexImage2D(GL_TEXTURE_2D, 0, 0x8229 /*R8*/, w, h, 0, 0x1903 /*RED*/, GL_UNSIGNED_BYTE, NULL);
    else                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
}
static void pfx_fbo_attach(GLuint *f, GLuint tex) {
    if (!*f) p_glGenFramebuffers(1, f);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *f);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

/* Window-sized mask from the presented surface's marks (fresh) or the last. */
static int pfx_mask_build(int fresh, int lw, int lh) {
    if (!s_mask_prog) {
        s_mask_prog = build_program(PRESENT_VS,
            "#version 330\nin vec2 v_uv; out vec4 o;\nvoid main(){ o = vec4(1.0); }\n");
        if (!s_mask_prog) return 0;
    }
    if (s_mask_w != lw || s_mask_h != lh) {
        pfx_tex_alloc(&s_mask_tex, lw, lh, -1, 1);
        pfx_fbo_attach(&s_mask_fbo, s_mask_tex);
        if (!s_mask_rb) p_glGenRenderbuffers(1, &s_mask_rb);
        p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_mask_rb);
        p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, lw, lh);
        p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                    PSXGL_RENDERBUFFER, s_mask_rb);
        s_mask_w = lw; s_mask_h = lh; s_mask_ok = 0;
    }
    if (fresh && s_mask_src_valid) {
        /* source rect (y0 = top row) -> window orientation (row 0 = bottom) */
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_mask_src_fbo);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_mask_fbo);
        p_glBlitFramebuffer(s_mask_src[0], s_mask_src[1], s_mask_src[2], s_mask_src[3],
                            0, lh, lw, 0, GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_mask_fbo);
        glViewport(0, 0, lw, lh);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0x02, 0x02);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        post_fullscreen(s_mask_prog, p_glGetUniformLocation(s_mask_prog, "u_uv_rect"));
        glDisable(GL_STENCIL_TEST);
        s_mask_ok = 1;
    }
    return s_mask_ok;
}

/* Run one stage's chain over the window rect. 0 when it could not run.
 * generated: a Smooth motion in-between frame; passes marked `generated
 * reuse` keep the target the last real frame left (when it is there at this
 * size) instead of running. */
static int pfx_stage_run(PfxStage *st, int generated, int lx, int ly, int lw, int lh) {
    PfxChain *c = &st->chain;
    post_copy_bind(lw, lh);
    post_copy_window(lx, ly, lw, lh);
    for (int i = 0; i < c->npasses; i++) {
        const PfxPass *ps = &c->pass[i];
        const int last = i == c->npasses - 1;
        if (!st->prog[i]) {
            st->prog[i] = pfx_build_pass(ps);
            if (!st->prog[i]) {
                fprintf(stdout, "psxrecomp: post effect '%s' pass '%s' did not compile; effect off\n",
                        c->name, ps->name);
                st->failed = 1;
                return 0;
            }
        }
        int w = lw, h = lh;
        if (!last) {
            w = (int)((float)lw * ps->scale); h = (int)((float)lh * ps->scale);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (generated && ps->reuse_generated && st->fbo[i] && st->tw[i] == w && st->th[i] == h) {
                s_pfx_reused++;
                continue;
            }
            if (st->tw[i] != w || st->th[i] != h) {
                pfx_tex_alloc(&st->tex[i], w, h, ps->format, ps->nearest);
                pfx_fbo_attach(&st->fbo[i], st->tex[i]);
                st->tw[i] = w; st->th[i] = h;
            }
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, st->fbo[i]);
            glViewport(0, 0, w, h);
        } else {
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
            glViewport(lx, ly, lw, lh);
        }
        const GLuint prog = st->prog[i];
        p_glUseProgram(prog);
        for (int k = 0; k < ps->ninputs; k++) {
            const int from = ps->in[k].from;
            const GLuint t = from == PFX_IN_SOURCE ? s_post_copy_tex
                           : from == PFX_IN_MASK ? s_mask_tex : st->tex[from];
            p_glActiveTexture(PSXGL_TEXTURE0 + (GLenum)k);
            glBindTexture(GL_TEXTURE_2D, t);
            p_glUniform1i(p_glGetUniformLocation(prog, ps->in[k].sampler), k);
        }
        p_glActiveTexture(PSXGL_TEXTURE0);
        p_glUniform2f(p_glGetUniformLocation(prog, "u_rcp"), 1.0f / (float)w, 1.0f / (float)h);
        p_glUniform2f(p_glGetUniformLocation(prog, "u_size"), (float)w, (float)h);
        p_glUniform2i(p_glGetUniformLocation(prog, "u_org"), last ? lx : 0, last ? ly : 0);
        for (int k = 0; k < ps->nuniforms; k++) {
            const PfxUniform *u = &ps->u[k];
            const GLint loc = p_glGetUniformLocation(prog, u->name);
            if (loc < 0) continue;
            switch (u->count) {
            case 1: p_glUniform1f(loc, u->v[0]); break;
            case 2: p_glUniform2f(loc, u->v[0], u->v[1]); break;
            case 3: p_glUniform3f(loc, u->v[0], u->v[1], u->v[2]); break;
            default: p_glUniform4f(loc, u->v[0], u->v[1], u->v[2], u->v[3]); break;
            }
        }
        post_fullscreen(prog, p_glGetUniformLocation(prog, "u_uv_rect"));
    }
    for (int k = PFX_MAX_INPUTS - 1; k >= 0; k--) {
        p_glActiveTexture(PSXGL_TEXTURE0 + (GLenum)k);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    return 1;
}

/* The GL state the executor changes, put back afterwards so whatever runs
 * next (the OSD, the next frame's draws) sees what it left. */
typedef struct {
    GLboolean blend, depth, scissor, stencil;
    GLfloat clear[4];
    GLint viewport[4], fbo, rfbo, active;
} PfxGlState;
static void pfx_state_save(PfxGlState *st) {
    st->blend = glIsEnabled(GL_BLEND); st->depth = glIsEnabled(GL_DEPTH_TEST);
    st->scissor = glIsEnabled(GL_SCISSOR_TEST); st->stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, st->clear);
    glGetIntegerv(GL_VIEWPORT, st->viewport);
    glGetIntegerv(0x8CA6 /*DRAW_FRAMEBUFFER_BINDING*/, &st->fbo);
    glGetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &st->rfbo);
    glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &st->active);
}
static void pfx_state_restore(const PfxGlState *st) {
    if (st->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (st->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (st->stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glClearColor(st->clear[0], st->clear[1], st->clear[2], st->clear[3]);
    glViewport(st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3]);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, (GLuint)st->fbo);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, (GLuint)st->rfbo);
    p_glActiveTexture((GLenum)st->active);
}

/* fresh: the noted source is this present's (0: reuse the last mask).
 * with_aa: a real frame (runs AA too); 0: a Smooth motion generated frame. */
static void postfx_present(int fresh, int with_aa, int lx, int ly, int lw, int lh) {
    const int src_fresh = fresh;
    if (!s_ctx) { s_mask_src_valid = 0; return; }
    if (!post_clip_rect(&lx, &ly, &lw, &lh, 8)) { s_mask_src_valid = 0; return; }
    PfxGlState saved;
    pfx_state_save(&saved);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    int mask_done = 0, mask_ok = 0;
    for (int i = 0; i < PFX_NSTAGES; i++) {
        if (!pfx_stage_on(i)) continue;
        if ((i == PFX_STAGE_AA || i == PFX_STAGE_HQ) && !with_aa) continue;
        if (!pfx_stage_load(i)) continue;
        PfxStage *st = &s_pfx[i];
        if (st->chain.uses_mask) {
            if (!mask_done) { mask_ok = pfx_mask_build(src_fresh, lw, lh); mask_done = 1; }
            if (!mask_ok) continue;                  /* no marks yet */
        }
        if (i == PFX_STAGE_BLOOM) {
            const float k = s_bloom, view = (float)s_bloom_view;
            pfx_chain_set_uniform(&st->chain, "u_strength", 1, &k);
            pfx_chain_set_uniform(&st->chain, "u_thr", 1, &s_bloom_thr);
            pfx_chain_set_uniform(&st->chain, "u_view", 1, &view);
        }
        int reps = 1; double *sum = NULL; uint64_t *cnt = NULL;
#ifndef PSX_NO_DEBUG_TOOLS
        if (i == PFX_STAGE_AA || i == PFX_STAGE_HQ) {
            if (s_paa_time < 0) {
                const char *e = getenv("PSX_POST_AA_TIME");
                s_paa_time = e ? atoi(e) : 0;
                if (s_paa_time < 0) s_paa_time = 0;
                if (s_paa_time > 64) s_paa_time = 64;
            }
            if (s_paa_time) { reps = s_paa_time > 1 ? s_paa_time : 1; sum = &s_paa_gpu_us_sum; cnt = &s_paa_gpu_n; }
        } else if (i == PFX_STAGE_BLOOM && s_bloom_time) {
            sum = with_aa ? &s_bloom_us_sum : &s_bloom_gus_sum;
            cnt = with_aa ? &s_bloom_us_n : &s_bloom_gus_n;
        }
        uint64_t t0 = 0;
        if (sum) { glFinish(); t0 = SDL_GetPerformanceCounter(); }
#endif
        int ran = 0;
        for (int r = 0; r < reps; r++) ran = pfx_stage_run(st, !with_aa, lx, ly, lw, lh);
#ifndef PSX_NO_DEBUG_TOOLS
        if (sum && ran) {
            glFinish();
            *sum += (double)(SDL_GetPerformanceCounter() - t0) * 1e6 /
                    (double)SDL_GetPerformanceFrequency() / (double)reps;
            (*cnt)++;
        }
#endif
        if (!ran) continue;
        if (i == PFX_STAGE_BLOOM) s_bloom_passes++;
        if (i == PFX_STAGE_AA || i == PFX_STAGE_HQ) s_paa_passes++;
    }
    s_mask_src_valid = 0;
    pfx_state_restore(&saved);
}

/* Objects died with the context. */
static void postfx_release(void) {
    for (int i = 0; i < PFX_NSTAGES; i++) {
        for (int k = 0; k < PFX_MAX_PASSES; k++) {
            s_pfx[i].prog[k] = s_pfx[i].tex[k] = s_pfx[i].fbo[k] = 0;
            s_pfx[i].tw[k] = s_pfx[i].th[k] = 0;
        }
        s_pfx[i].failed = 0;
    }
    s_mask_tex = s_mask_rb = s_mask_fbo = s_mask_prog = 0;
    s_mask_w = s_mask_h = 0; s_mask_ok = 0; s_mask_src_valid = 0;
}

/* [video] post_shader (PSX_POST_SHADER overrides): a .pfx chain, "" = none. */
void gl_renderer_set_post_shader(const char *path) {
    GL_RT_SYNC("post_shader");
    if (!s_pfx_user_env) {
        const char *e = getenv("PSX_POST_SHADER");
        if (e) { snprintf(s_pfx_user_path, sizeof s_pfx_user_path, "%s", e); s_pfx_user_env = 1; }
    }
    if (!s_pfx_user_env) {
        if (strcmp(s_pfx_user_path, path ? path : "")) pfx_stage_unload(&s_pfx[PFX_STAGE_USER]);
        snprintf(s_pfx_user_path, sizeof s_pfx_user_path, "%s", path ? path : "");
    }
    if (s_pfx_user_path[0] && s_ctx) (void)pfx_stage_load(PFX_STAGE_USER);
    else if (s_pfx_user_path[0]) {
        /* No context yet: the parse needs none, so the mask decision is made now. */
        (void)pfx_stage_load(PFX_STAGE_USER);
    }
    pfx_mask_wanted_update();
}
int gl_renderer_post_shader_loaded(void) {
    GL_RT_SYNC("post_shader");
    return s_pfx[PFX_STAGE_USER].loaded && !s_pfx[PFX_STAGE_USER].failed;
}

/* PSX_BLOOM=<0..2> overrides [video] bloom. */
void gl_renderer_set_bloom(float strength) {
    GL_RT_SYNC("set_bloom");
    const char *e = getenv("PSX_BLOOM");
    if (e && *e) strength = (float)atof(e);
    if (!(strength >= 0.0f)) strength = 0.0f;
    if (strength > 2.0f) strength = 2.0f;
    s_bloom = strength;
    pfx_mask_wanted_update();
}
float gl_renderer_get_bloom(void) { return s_bloom; }

void gl_renderer_bloom_debug(float strength, float threshold, int view, int time,
                             GlBloomInfo *out) {
    GL_RT_SYNC("bloom_debug");
    if (strength >= 0.0f && strength <= 2.0f) { s_bloom = strength; pfx_mask_wanted_update(); }
    if (threshold > 0.0f) s_bloom_thr = threshold;
    if (view >= 0 && view <= 2) s_bloom_view = view;
    if (time >= 0) s_bloom_time = time ? 1 : 0;
    if (out) {
        out->strength = s_bloom; out->threshold = s_bloom_thr; out->view = s_bloom_view;
        out->passes = s_bloom_passes;
        out->us_mean = s_bloom_us_n ? s_bloom_us_sum / (double)s_bloom_us_n : 0.0;
        out->us_n = s_bloom_us_n;
        out->gen_us_mean = s_bloom_gus_n ? s_bloom_gus_sum / (double)s_bloom_gus_n : 0.0;
        out->gen_us_n = s_bloom_gus_n;
        out->reused = s_pfx_reused;
        s_bloom_us_sum = 0.0; s_bloom_us_n = 0; s_bloom_gus_sum = 0.0; s_bloom_gus_n = 0;
    }
}
