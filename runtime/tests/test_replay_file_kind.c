/* PS1B-675: a bare argument that is a replay plays; anything else stays the
 * positional BIOS alias.
 *
 * psx_file_is_replay() decides by the file's own first eight bytes. The real
 * replay files the runtime writes start with "PSXRTI3\0"; older routes with
 * "PSXRTI1\0" or "PSXRTI2\0". A BIOS image starts with MIPS code (SCPH-1001's
 * first word is 0x3C080013, lui t0,0x13). The wiring check reads main.cpp: the
 * positional branch must ask before it takes the argument as the BIOS. */
#include "replay_file_kind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void write_file(const char *path, const void *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("FAIL: cannot write %s\n", path);
        exit(1);
    }
    if (n) fwrite(bytes, 1, n, f);
    fclose(f);
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *s;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s = (char *)malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    static const unsigned char v3[16] = {'P','S','X','R','T','I','3',0, 3,0,0,0, 12,0,0,0};
    static const unsigned char v1[8] = {'P','S','X','R','T','I','1',0};
    static const unsigned char v2[8] = {'P','S','X','R','T','I','2',0};
    static const unsigned char v9[8] = {'P','S','X','R','T','I','9',0};
    static const unsigned char bios[8] = {0x13,0x00,0x08,0x3C, 0x3F,0x24,0x08,0x35};
    static const unsigned char shortf[6] = {'P','S','X','R','T','I'};
    const char *main_cpp = argc > 1 ? argv[1] : NULL;

    write_file("replay_kind_v3.psxrpl", v3, sizeof v3);
    write_file("replay_kind_v1.bin", v1, sizeof v1);
    write_file("replay_kind_v2.dat", v2, sizeof v2);
    write_file("replay_kind_v9.psxrpl", v9, sizeof v9);
    write_file("replay_kind_bios.psxrpl", bios, sizeof bios);
    write_file("replay_kind_short.psxrpl", shortf, sizeof shortf);
    write_file("replay_kind_empty.psxrpl", "", 0);

    check(psx_file_is_replay("replay_kind_v3.psxrpl") == 1, "a PSXRTI3 replay is a replay");
    check(psx_file_is_replay("replay_kind_v1.bin") == 1, "a PSXRTI1 route is a replay, whatever its name");
    check(psx_file_is_replay("replay_kind_v2.dat") == 1, "a PSXRTI2 route is a replay, whatever its name");
    check(psx_file_is_replay("replay_kind_v9.psxrpl") == 0, "an unknown PSXRTI version is not");
    check(psx_file_is_replay("replay_kind_bios.psxrpl") == 0, "BIOS bytes named .psxrpl are not a replay");
    check(psx_file_is_replay("replay_kind_short.psxrpl") == 0, "a file shorter than the magic is not");
    check(psx_file_is_replay("replay_kind_empty.psxrpl") == 0, "an empty file is not");
    check(psx_file_is_replay("replay_kind_missing.psxrpl") == 0, "a missing file is not");
    check(psx_file_is_replay("") == 0 && psx_file_is_replay(NULL) == 0, "no path is not");

    remove("replay_kind_v3.psxrpl");
    remove("replay_kind_v1.bin");
    remove("replay_kind_v2.dat");
    remove("replay_kind_v9.psxrpl");
    remove("replay_kind_bios.psxrpl");
    remove("replay_kind_short.psxrpl");
    remove("replay_kind_empty.psxrpl");

    if (main_cpp) {
        char *src = read_text(main_cpp);
        const char *bare, *ask, *alias;
        check(src != NULL, "main.cpp is readable");
        if (src) {
            bare = strstr(src, "} else if (argv[i][0] != '-') {");
            ask = bare ? strstr(bare, "psx_file_is_replay(argv[i])") : NULL;
            alias = bare ? strstr(bare, "bios_path = argv[i];") : NULL;
            check(bare != NULL, "main.cpp has the bare-argument branch");
            check(ask && alias && ask < alias,
                  "the bare-argument branch asks for a replay before it takes the BIOS alias");
            check(ask && strstr(ask, "g_replay_cli_path = argv[i];") &&
                      strstr(ask, "g_replay_cli_path = argv[i];") < alias,
                  "a bare replay becomes the --replay path");
            free(src);
        }
    } else {
        check(0, "pass main.cpp as the first argument");
    }

    if (failures) return 1;
    printf("replay_file_kind_test: all checks passed\n");
    return 0;
}
