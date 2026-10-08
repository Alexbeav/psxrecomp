#include "replay_marks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static char replay[1000], path[1100];
static ReplayMarks marks;
static void raw(const char *text) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { CHECK(fputs(text, f) >= 0); CHECK(fclose(f) == 0); }
}
static void invalid(const char *text) {
    raw(text);
    CHECK(replay_marks_read(replay, 120, &marks) != NULL);
    CHECK(marks.count == 0);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    snprintf(replay, sizeof replay, "%s/marks.psxrpl", argv[1]);
    CHECK(replay_marks_path(replay, path, sizeof path));
    remove(path);
    CHECK(!replay_marks_read(replay, 120, &marks) && marks.count == 0);
    CHECK(!replay_marks_path(replay, path, 4));
    CHECK(replay_marks_path(replay, path, sizeof path));
    raw("{\"marks\":[{\"label\":\"title\",\"frame\":0},{\"estimated\":true,\"frame\":60,\"label\":\"gameplay\"}],\"schema\":\"psxrecomp-replay-marks/1\"}");
    CHECK(!replay_marks_read(replay, 120, &marks) && marks.count == 2);
    CHECK(marks.items[1].frame == 60 && marks.items[1].estimated && marks.items[1].in_sync == -1);
    uint32_t frame = 999;
    CHECK(replay_marks_resolve(&marks, "title", 120, &frame) && frame == 0);
    CHECK(replay_marks_resolve(&marks, "gameplay+60", 120, &frame) && frame == 120);
    CHECK(!replay_marks_resolve(&marks, "gameplay+61", 120, &frame));
    CHECK(!replay_marks_resolve(&marks, "missing", 120, &frame));
    CHECK(!replay_marks_resolve(&marks, "gameplay+-1", 120, &frame));
    CHECK(!replay_marks_resolve(&marks, "gameplay+4294967296", 120, &frame));
    CHECK(!replay_marks_resolve(&marks, "gameplay+1.5", 120, &frame));
    CHECK(!replay_marks_resolve(&marks, "gameplay+01", 120, &frame));
    CHECK(replay_marks_add(&marks, 80, "gameplay", 0) && marks.count == 2 && marks.items[1].frame == 80);
    CHECK(replay_marks_add(&marks, 80, "Caf\xc3\xa9 \\\" path", 0));
    remove(path);
    CHECK(!replay_marks_write(replay, &marks));
    CHECK(!replay_marks_read(replay, 120, &marks) && marks.count == 3 && !strcmp(marks.items[2].label, "Caf\xc3\xa9 \\\" path"));
    raw("{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":120,\"label\":\"\\u03b1\\ud83d\\ude00\"}]}");
    CHECK(!replay_marks_read(replay, 120, &marks) && !strcmp(marks.items[0].label, "\xce\xb1\xf0\x9f\x98\x80"));
    const char *bad[] = {
        "{}", "[]", "{\"schema\":\"other\",\"marks\":[]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[],\"marks\":[]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[],\"unknown\":1}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[],}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[]}x",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":121,\"label\":\"x\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":-1,\"label\":\"x\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1.0,\"label\":\"x\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":01,\"label\":\"x\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":4294967296,\"label\":\"x\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"frame\":1,\"label\":\"x\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"x\",\"estimated\":1}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"x\",\"extra\":true}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"\\u0000\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"\\ud800\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"\\udc00\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"\\ud800 \\udc00\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"\xc0\x80\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"\xe0\xa0\"}]}",
        "{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[{\"frame\":1,\"label\":\"x\"},{\"frame\":2,\"label\":\"x\"}]}",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) invalid(bad[i]);
    memset(&marks, 0, sizeof marks);
    for (unsigned i = 0; i < REPLAY_MARKS_MAX; ++i) {
        char label[32]; snprintf(label, sizeof label, "label%u", i);
        CHECK(replay_marks_add(&marks, 0, label, 0));
    }
    CHECK(!replay_marks_add(&marks, 0, "overflow", 0));
    remove(path);
    CHECK(!replay_marks_write(replay, &marks));
    CHECK(!replay_marks_read(replay, 120, &marks) && marks.count == REPLAY_MARKS_MAX);
    FILE *f = fopen(path, "ab"); CHECK(f != NULL);
    if (f) { for (unsigned i = 0; i < REPLAY_MARKS_BYTES_MAX; ++i) fputc(' ', f); fclose(f); }
    CHECK(replay_marks_read(replay, 120, &marks) != NULL && marks.count == 0);
    raw("{\"schema\":\"psxrecomp-replay-marks/1\",\"marks\":[]}");
    f = fopen(path, "ab"); CHECK(f != NULL); if (f) { fputc(0, f); fclose(f); }
    CHECK(replay_marks_read(replay, 120, &marks) != NULL);
    raw("FOREIGN-MARK-SENTINEL");
    CHECK(replay_marks_write(replay, &marks) != NULL);
    char sentinel[64] = {0};
    f = fopen(path, "rb"); CHECK(f != NULL);
    if (f) { fread(sentinel, 1, sizeof sentinel - 1, f); fclose(f); }
    CHECK(!strcmp(sentinel, "FOREIGN-MARK-SENTINEL"));
    remove(path);
    memset(&marks, 0, sizeof marks);
    CHECK(replay_marks_add(&marks, 0, "title", 0));
    FILE *owner = NULL;
    CHECK(!replay_marks_save(replay, &marks, &owner) && owner != NULL);
    CHECK(replay_marks_add(&marks, 60, "gameplay", 0));
    CHECK(!replay_marks_save(replay, &marks, &owner));
    CHECK(!replay_marks_read(replay, 120, &marks) && marks.count == 2);
#ifndef _WIN32
    char moved[1200]; snprintf(moved, sizeof moved, "%s.owned", path);
    CHECK(rename(path, moved) == 0);
    raw("REPLACED-FOREIGN-SENTINEL");
    CHECK(replay_marks_save(replay, &marks, &owner) != NULL);
    memset(sentinel, 0, sizeof sentinel);
    f = fopen(path, "rb"); CHECK(f != NULL);
    if (f) { fread(sentinel, 1, sizeof sentinel - 1, f); fclose(f); }
    CHECK(!strcmp(sentinel, "REPLACED-FOREIGN-SENTINEL"));
    remove(moved);
#endif
    if (owner) fclose(owner);
    remove(path);
    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: replay mark parser and selectors, %d checks\n", checks);
    return 0;
}
