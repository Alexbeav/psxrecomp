#ifndef PSX_SOURCE_RAM_PAGE_PROBE_H
#define PSX_SOURCE_RAM_PAGE_PROBE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Passive diagnostic index. A hash mismatch locates bytes to inspect; hashes
 * are not a substitute for comparing the selected raw RAM snapshots. */
static uint64_t source_ram_page_hash(const uint8_t *bytes, unsigned count) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < count; ++i)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static void source_ram_page_probe(unsigned frame, uint64_t cycle) {
    static int initialized, enabled;
    static FILE *stream;
    static unsigned snapshots[32], snapshot_count;
    if (!initialized) {
        initialized = 1;
        const char *setting = getenv("PSX_SOURCE_RAM_PAGE_PROBE");
        enabled = setting && strcmp(setting, "1") == 0;
        const char *cursor = getenv("PSX_SOURCE_RAM_SNAPSHOT_FRAMES");
        while (enabled && cursor && *cursor) {
            if (*cursor < '0' || *cursor > '9') abort();
            char *end;
            unsigned long value = strtoul(cursor, &end, 10);
            if (end == cursor || value < 1 || value > 20000 || snapshot_count == 32 ||
                (*end && *end != ',')) abort();
            for (unsigned i = 0; i < snapshot_count; ++i)
                if (snapshots[i] == value) abort();
            snapshots[snapshot_count++] = (unsigned)value;
            cursor = *end ? end + 1 : end;
            if (*end && !*cursor) abort();
        }
    }
    if (!enabled) return;
    if (frame < 1 || frame > 20000) abort();
    extern uint8_t *memory_get_ram_ptr(void);
    const uint8_t *ram = memory_get_ram_ptr();
    char path[4096];
    const char *directory = getenv("PSX_INPUT_ROUTE_CAPTURE_DIR");
    if (!ram || !directory) abort();
    if (!stream) {
        if (snprintf(path, sizeof(path), "%s/ram-pages.tsv", directory) >= (int)sizeof(path)) abort();
        stream = fopen(path, "wx");
        if (!stream) abort();
        fputs("# psx-ram-pages-v1 page_bytes=4096 ram_bytes=2097152 hash=fnv1a64\nframe\tcycle", stream);
        for (unsigned page = 0; page < 512; ++page) fprintf(stream, "\t%06X", page * 4096);
        fputc('\n', stream);
    }
    fprintf(stream, "%u\t%llu", frame, (unsigned long long)cycle);
    for (unsigned page = 0; page < 512; ++page)
        fprintf(stream, "\t%016llX", (unsigned long long)source_ram_page_hash(ram + page * 4096, 4096));
    fputc('\n', stream);
    if (fflush(stream) != 0 || ferror(stream)) abort();
    for (unsigned i = 0; i < snapshot_count; ++i) if (snapshots[i] == frame) {
        if (snprintf(path, sizeof(path), "%s/ram-frame-%06u.bin", directory, frame) >= (int)sizeof(path)) abort();
        FILE *snapshot = fopen(path, "wbx");
        if (!snapshot) abort();
        if (fwrite(ram, 1, 2097152, snapshot) != 2097152 || fclose(snapshot) != 0) abort();
    }
}
#endif
