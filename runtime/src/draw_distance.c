/* draw_distance.c - the [[draw_distance.clamp]] site store and switch
 * (draw_distance.h). */
#include "draw_distance.h"
#include "mod_plugins.h"

#include <stdio.h>
#include <stdlib.h>

uint32_t g_psx_draw_distance_clamp = 0;
int g_psx_draw_distance_clamp_live = 0;

static PSXDrawDistanceClampSite s_sites[PSX_DRAW_DISTANCE_CLAMP_SITES_MAX];
static int s_site_count = 0;

static void update_live(void) {
    g_psx_draw_distance_clamp_live =
        (g_psx_draw_distance_clamp != 0u && s_site_count > 0) ? 1 : 0;
}

static int site_cmp(const void *a, const void *b) {
    const uint32_t x = ((const PSXDrawDistanceClampSite *)a)->address;
    const uint32_t y = ((const PSXDrawDistanceClampSite *)b)->address;
    return (x > y) - (x < y);
}

int psx_draw_distance_set_clamp_sites(const PSXDrawDistanceClampSite *sites,
                                      int count) {
    if (!sites || count < 0) count = 0;
    if (count > PSX_DRAW_DISTANCE_CLAMP_SITES_MAX) {
        fprintf(stderr,
                "psxrecomp: [[draw_distance.clamp]] lists %d sites; keeping "
                "the first %d\n",
                count, PSX_DRAW_DISTANCE_CLAMP_SITES_MAX);
        count = PSX_DRAW_DISTANCE_CLAMP_SITES_MAX;
    }
    for (int i = 0; i < count; i++) {
        s_sites[i] = sites[i];
        s_sites[i].address &= 0x1FFFFFFFu;
    }
    if (count > 1) qsort(s_sites, (size_t)count, sizeof s_sites[0], site_cmp);
    s_site_count = count;
    update_live();
    return count;
}

int psx_draw_distance_clamp_site_count(void) { return s_site_count; }

const PSXDrawDistanceClampSite *psx_draw_distance_clamp_find(uint32_t pc,
                                                             uint32_t insn) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    int lo = 0, hi = s_site_count;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (s_sites[mid].address < phys) lo = mid + 1;
        else hi = mid;
    }
    if (lo < s_site_count && s_sites[lo].address == phys &&
        s_sites[lo].expected == insn)
        return &s_sites[lo];
    return NULL;
}

/* mod_plugins.h. Process-wide host state: the session start resets it to off
 * (main.cpp reset_mod_owned_presentation), so only a session whose plan
 * activates a plugin that asks for it, never netplay, runs with it on. */
int psx_mod_set_draw_distance_clamp(int enabled) {
    g_psx_draw_distance_clamp = enabled ? 1u : 0u;
    update_live();
    return s_site_count > 0;
}

int psx_mod_draw_distance_clamp_enabled(void) {
    return g_psx_draw_distance_clamp != 0u;
}
