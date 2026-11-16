/* Overwrite/refusal behaviour of the atomic state-file replace (Windows
 * MoveFileExW REPLACE_EXISTING). The overwrite case is the one that matters:
 * after each fix the same checkpoint path is regenerated. */
#include "boot_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(text, 1, strlen(text), f) != strlen(text)) { fclose(f); return 0; }
    return fclose(f) == 0;
}

static int read_is(const char *path, const char *text) {
    char buf[64];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    char target[512], tmp[512];
    if (argc != 2) return 2;
    snprintf(target, sizeof target, "%s/state.pst", argv[1]);
    snprintf(tmp, sizeof tmp, "%s/state.pst.tmp", argv[1]);

    remove(target);
    remove(tmp);

    /* 1. replace with no existing target */
    check(write_text(tmp, "NEW"), "stage tmp 1");
    check(boot_state_replace_file(tmp, target) == 1, "replace onto absent target");
    check(read_is(target, "NEW"), "target content after first replace");
    check(file_size(tmp) == -1, "tmp consumed");

    /* 2. OVERWRITE: target already exists (the case that breaks on a bare
     *    Windows rename and that happens on every re-checkpoint) */
    check(write_text(tmp, "NEWER"), "stage tmp 2");
    check(boot_state_replace_file(tmp, target) == 1, "replace onto EXISTING target");
    check(read_is(target, "NEWER"), "target content after overwrite");
    check(file_size(tmp) == -1, "tmp consumed on overwrite");

    /* 3. failed replace refuses (missing source) and leaves the target intact */
    check(boot_state_replace_file(tmp, target) == 0, "refuse missing source");
    check(read_is(target, "NEWER"), "target intact after refused replace");

    /* 4. null arguments refuse */
    check(boot_state_replace_file(NULL, target) == 0, "refuse null from");
    check(boot_state_replace_file(target, NULL) == 0, "refuse null to");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: atomic replace onto absent and EXISTING target; failure refuses");
    return 0;
}
