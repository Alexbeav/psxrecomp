/* test_host_overlay.c -- unit tests for host_overlay.c hooks. */
#include "host_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int refuse_on = 0;
static int refuse(void) { return refuse_on; }

static void test_pause(void) {
    CHECK(psx_host_pause_depth() == 0 && psx_host_pause_reason() == NULL);
    CHECK(psx_host_pause_push("overlay"));
    CHECK(psx_host_pause_push("other"));
    CHECK(psx_host_pause_depth() == 2 && strcmp(psx_host_pause_reason(), "other") == 0);
    psx_host_pause_pop();
    CHECK(strcmp(psx_host_pause_reason(), "overlay") == 0);
    psx_host_pause_pop();
    psx_host_pause_pop(); /* extra pop is harmless */
    CHECK(psx_host_pause_depth() == 0);
    psx_host_pause_set_refuse_probe(refuse);
    refuse_on = 1;
    CHECK(!psx_host_pause_push("netplay") && psx_host_pause_depth() == 0);
    refuse_on = 0;
    CHECK(psx_host_pause_push(NULL) && strcmp(psx_host_pause_reason(), "host") == 0);
    psx_host_pause_pop();
    for (int i = 0; i < 8; i++) CHECK(psx_host_pause_push("n"));
    CHECK(!psx_host_pause_push("overflow"));
    for (int i = 0; i < 8; i++) psx_host_pause_pop();
}

static int draws = 0, last_w = 0, last_h = 0;
static void *last_ctx = NULL;
static void on_draw(int w, int h, void *ctx) { draws++; last_w = w; last_h = h; last_ctx = ctx; }

static void test_draw(void) {
    int tag = 7;
    CHECK(!psx_host_overlay_has_draw());
    psx_host_overlay_draw(640, 480); /* no callback: nothing happens */
    psx_host_overlay_set_draw_cb(on_draw, &tag);
    CHECK(psx_host_overlay_has_draw());
    psx_host_overlay_draw(1280, 800);
    CHECK(draws == 1 && last_w == 1280 && last_h == 800 && last_ctx == &tag);
    psx_host_overlay_draw(0, 800); /* minimised window: skipped */
    CHECK(draws == 1);
    psx_host_overlay_set_draw_cb(NULL, NULL);
    psx_host_overlay_draw(1280, 800);
    CHECK(draws == 1 && !psx_host_overlay_has_draw());
}

/* Lifetime: once set_draw_cb(NULL) returns, the old callback never runs
 * again, so its context can be freed. A reader thread draws continuously;
 * the main thread unregisters and immediately marks the context dead. */
#if !defined(_WIN32) || defined(__MINGW32__)
#include <pthread.h>
#include <stdatomic.h>
typedef struct { atomic_int alive; atomic_int calls; atomic_int used_dead; } LifeCtx;
static atomic_int g_stop;
static void life_draw(int w, int h, void* p) {
    LifeCtx* c = (LifeCtx*)p;
    (void)w; (void)h;
    for (volatile int i = 0; i < 2000; ++i) {}  /* a frame's worth of work */
    if (!atomic_load(&c->alive)) atomic_fetch_add(&c->used_dead, 1);
    atomic_fetch_add(&c->calls, 1);
}
static void* life_reader(void* arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) psx_host_overlay_draw(64, 64);
    return NULL;
}
static void test_draw_lifetime(void) {
    pthread_t t;
    atomic_store(&g_stop, 0);
    pthread_create(&t, NULL, life_reader, NULL);
    int dead_uses = 0, total = 0;
    for (int round = 0; round < 300; ++round) {
        LifeCtx* c = (LifeCtx*)calloc(1, sizeof(*c));
        atomic_store(&c->alive, 1);
        psx_host_overlay_set_draw_cb(life_draw, c);
        while (atomic_load(&c->calls) < 2) {}
        psx_host_overlay_set_draw_cb(NULL, NULL);  /* must wait for the reader */
        atomic_store(&c->alive, 0);                 /* the owner tears down */
        for (volatile int i = 0; i < 20000; ++i) {}
        dead_uses += atomic_load(&c->used_dead);
        total += atomic_load(&c->calls);
        free(c);
    }
    atomic_store(&g_stop, 1);
    pthread_join(t, NULL);
    CHECK(total > 0);
    CHECK(dead_uses == 0);
}
#else
static void test_draw_lifetime(void) {}
#endif

/* Changing the callback from inside it must not deadlock. */
static void self_unregister(int w, int h, void* p) {
    (void)w; (void)h;
    ++*(int*)p;
    psx_host_overlay_set_draw_cb(NULL, NULL);
}
static void test_draw_reentrant(void) {
    int n = 0;
    psx_host_overlay_set_draw_cb(self_unregister, &n);
    psx_host_overlay_draw(64, 64);
    psx_host_overlay_draw(64, 64);
    CHECK(n == 1 && !psx_host_overlay_has_draw());
}

int main(void) {
    test_draw_lifetime();
    test_draw_reentrant();
    test_draw();
    test_pause();
    printf("host_overlay_test: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
