/* v7 identity dimensions: file hash, runtime-binary hash, config digest. */
#include "source_stateio_identity.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main(int argc, char **argv) {
    char a[65], b[65], cfg[17], cfg2[17];
    FILE *f;

    if (argc != 2) return 2;

    /* self-hash of this test executable resolves and is stable */
    check(source_stateio_exe_sha256(a) == 1 && strlen(a) == 64, "exe hash resolves");
    check(source_stateio_exe_sha256(b) == 1 && strcmp(a, b) == 0, "exe hash stable");
    check(source_stateio_file_sha256(argv[1], b) == 1 && strcmp(a, b) != 0,
          "distinct file hashes differently");

    /* content sensitivity */
    f = fopen(argv[1], "wb");
    if (!f) return 2;
    fputs("one", f); fclose(f);
    check(source_stateio_file_sha256(argv[1], a) == 1, "hash written file");
    f = fopen(argv[1], "wb");
    if (!f) return 2;
    fputs("two", f); fclose(f);
    check(source_stateio_file_sha256(argv[1], b) == 1 && strcmp(a, b) != 0,
          "file hash content sensitive");

    /* refusal cases */
    check(source_stateio_file_sha256("does-not-exist-xyz", b) == 0 && b[0] == '\0',
          "missing file refuses");
    check(source_stateio_file_sha256(NULL, b) == 0, "null path refuses");

    /* config digest is present; the value itself is environment dependent, so
     * only shape and stability are asserted here. */
    check(source_stateio_config_digest_hex(cfg) == 1 && strlen(cfg) == 16, "config digest shape");
    check(source_stateio_config_digest_hex(cfg2) == 1 && strcmp(cfg, cfg2) == 0,
          "config digest stable");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("config=%s\n", cfg);
    puts("PASS: v7 identity dimensions");
    return 0;
}
