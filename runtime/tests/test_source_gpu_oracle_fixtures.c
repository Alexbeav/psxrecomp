/* PS1B-182: authored oracle micro-fixtures replayed through the timing table.
 *
 * source_gpu_oracle_micro_fixtures.tsv holds dispatch traces of authored
 * CPU/MMIO programs run on the oracle core (no BIOS, no retail data). For
 * every case, the environment commands in its trace (E1h-E6h) and its GP1
 * writes (from source_gpu_oracle_micro_fixtures_gp1.tsv) rebuild the draw
 * state; the case's measured command must then cost exactly its logged
 * charge_from_full. Textured cases are skipped: their logged charge includes
 * the renderer's texture-cache work, which the fixture cannot separate.
 * Usage: test <fixtures.tsv> <gp1.tsv> */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCASES 128
typedef struct { char name[64]; uint32_t gp1[8]; int ngp1; } Gp1;
static Gp1 gp1s[MAXCASES];
static int ngp1s;

static void load_gp1(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        Gp1 *g = &gp1s[ngp1s++];
        char *tab = strchr(line, '\t');
        if (!tab) { --ngp1s; continue; }
        *tab = 0;
        snprintf(g->name, sizeof g->name, "%.63s", line);
        char *p = tab + 1;
        while (*p && *p != '\n' && g->ngp1 < 8) {
            g->gp1[g->ngp1++] = (uint32_t)strtoul(p, &p, 16);
            while (*p == ' ') ++p;
        }
    }
    fclose(f);
}

static const Gp1 *gp1_for(const char *name)
{
    for (int i = 0; i < ngp1s; ++i) if (!strcmp(gp1s[i].name, name)) return &gp1s[i];
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s fixtures.tsv gp1.tsv\n", argv[0]); return 2; }
    load_gp1(argv[2]);
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    char line[4096], current[64] = "";
    SourceGPUCommandProjection s;
    unsigned checked = 0, skipped = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'D') continue;
        char *col[16];
        int n = 0;
        char *p = line;
        col[n++] = p;
        while ((p = strchr(p, '\t')) && n < 16) { *p++ = 0; col[n++] = p; }
        if (n < 11) continue;
        col[10][strcspn(col[10], "\r\n")] = 0;
        /* D case cycle kind budget_after phase queued budget_delta cycles_since_prev charge_from_full words */
        if (strcmp(col[1], current)) {
            snprintf(current, sizeof current, "%.63s", col[1]);
            source_gpu_command_cold(&s);
            s.field_valid = 1;
            const Gp1 *g = gp1_for(current);
            for (int i = 0; g && i < g->ngp1; ++i) source_gpu_command_gp1(&s, g->gp1[i]);
        }
        uint32_t w[16];
        int nw = 0;
        char *q = col[10];
        while (*q && nw < 16) { w[nw++] = (uint32_t)strtoul(q, &q, 16); while (*q == ' ') ++q; }
        unsigned op = w[0] >> 24;
        if (!strcmp(col[9], "NA")) {
            if (op >= 0xE1u && op <= 0xE6u) source_gpu_command_environment(&s, w[0]);
            continue;
        }
        int kind = atoi(col[3]);
        long logged = atol(col[9]), model;
        if (source_gpu_polygon_supported(op)) {
            if (op & 0x04u) { ++skipped; continue; }
            model = source_gpu_command_polygon_cost(&s, w, kind == 3);
        } else if (source_gpu_sprite_opcode(op)) {
            if (op & 0x04u) { ++skipped; continue; }
            model = SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_block_cost(&s, w);
        } else if (op == 0x02u || op == 0x80u) {
            model = SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_block_cost(&s, w);
        } else if (source_gpu_line_supported(op)) {
            model = SOURCE_GPU_T_COMMAND_OVERHEAD + source_gpu_command_line_cost(&s, w);
        } else {
            model = source_gpu_command_immediate(op) ? 0 : SOURCE_GPU_T_COMMAND_OVERHEAD;
        }
        ++checked;
        int ok = model == logged;
        if (!ok) ++bad;
        printf("%-32s %-8s logged %7ld model %7ld%s\n", current, col[10], logged, model, ok ? "" : "  MISMATCH");
    }
    fclose(f);
    printf("%u cases checked, %u textured cases skipped, %u mismatched\n", checked, skipped, bad);
    if (!checked) return 2;
    return bad ? 1 : 0;
}
