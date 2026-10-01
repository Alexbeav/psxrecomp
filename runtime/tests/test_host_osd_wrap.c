/* PS1B-316: line breaking of the host toast (host_osd_wrap.h).
 *
 *   host_osd_wrap_test                 the unit checks
 *   host_osd_wrap_test --fit <file>    one message per line of <file>; prints
 *       "<lines>\t<widest>\t<length>\t<message>" for each and exits 1 when a
 *       message does not fit (test_replay_osd_text.py feeds it every replay
 *       message). */
#include "host_osd_wrap.h"
#include <stdio.h>
#include <stdlib.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char L[HOST_OSD_LINES][HOST_OSD_COLS + 1];
static int cut;
static int wrap(const char *m) { return host_osd_wrap(m, L, &cut); }
static int widest(int n) {
    int w = 0;
    for (int i = 0; i < n; ++i) if ((int)strlen(L[i]) > w) w = (int)strlen(L[i]);
    return w;
}

static int fit(const char *path) {
    FILE *f = fopen(path, "rb");
    char line[1024];
    int bad = 0, count = 0;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        const int lines = wrap(line);
        printf("%d\t%d\t%u\t%s%s\n", lines, widest(lines), (unsigned)n, line, cut ? "\tDOES NOT FIT" : "");
        bad += cut;
        count++;
    }
    fclose(f);
    if (!count) { fprintf(stderr, "no messages in %s\n", path); return 2; }
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--fit")) return fit(argv[2]);

    /* The numbers the toast is sized by: 8 pixels of margin, 2+2 of padding
     * at 2x, 16 pixels a character: 38 columns end at pixel 624 of 640. */
    CHECK(8 + (2 * 2 + HOST_OSD_COLS * 8) * 2 <= 640, "a full line fits the 640-pixel window");
    CHECK(8 + (2 * 2 + (HOST_OSD_COLS + 2) * 8) * 2 > 640, "and two more characters would not");

    CHECK(wrap("") == 0 && wrap(NULL) == 0 && !cut, "empty message: no lines");
    CHECK(wrap("Replay playing") == 1 && !strcmp(L[0], "Replay playing") && !cut, "a short message is one line");
    char full[HOST_OSD_COLS + 1];
    memset(full, 'x', HOST_OSD_COLS); full[HOST_OSD_COLS] = 0;
    CHECK(wrap(full) == 1 && !strcmp(L[0], full) && !cut, "exactly one line wide stays one line");

    /* The message that did not fit (PS1B-316 hands-on), as it reads now. */
    CHECK(wrap("Replay not recorded: port 1 needs a pad or the keyboard") == 2 &&
          !strcmp(L[0], "Replay not recorded:") && !strcmp(L[1], "port 1 needs a pad or the keyboard") && !cut,
          "prefix on its own line, reason below: [%s] [%s]", L[0], L[1]);
    /* A reason too long for one line keeps the prefix line and wraps. */
    CHECK(wrap("Replay not played: it was recorded on a different game or disc image") == 3 &&
          !strcmp(L[0], "Replay not played:") && !strcmp(L[1], "it was recorded on a different game or") &&
          !strcmp(L[2], "disc image") && !cut, "three lines: [%s] [%s] [%s]", L[0], L[1], L[2]);
    /* A reason that does not fit under its prefix falls back to plain filling. */
    const char *long_reason = "Note: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
                              "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb cccccccccccccccccccccccccccccccccccccc";
    int n = wrap(long_reason);
    CHECK(n == 3 && !cut && !strncmp(L[0], "Note: aaaa", 10) && L[1][0] == 'b' && L[2][0] == 'c',
          "fallback fill keeps the whole text: [%s] [%s] [%s] cut=%d", L[0], L[1], L[2], cut);
    /* No prefix: break at spaces, never inside a word. */
    CHECK(wrap("Save states are off while a replay records or plays") == 2 &&
          !strcmp(L[0], "Save states are off while a replay") && !strcmp(L[1], "records or plays"),
          "plain fill: [%s] [%s]", L[0], L[1]);
    /* A saved replay's name (shown without folder or extension). */
    CHECK(wrap("Replay saved: Time_Crisis-boot-20261001T111417Z") == 2 &&
          !strcmp(L[0], "Replay saved:") && !strcmp(L[1], "Time_Crisis-boot-20261001T111417Z") && !cut,
          "saved name: [%s] [%s]", L[0], L[1]);
    /* The longest boot name: 48 title characters, "-boot-", the stamp, "-99". */
    n = wrap("Replay saved: Tttttttttttttttttttttttttttttttttttttttttttttttt-boot-20261001T111417Z-99");
    CHECK(n == 3 && !cut && !strcmp(L[0], "Replay saved:") && strlen(L[1]) == HOST_OSD_COLS &&
          !strcmp(L[2] + strlen(L[2]) - 3, "-99"), "longest saved name fits: [%s] [%s] [%s]", L[0], L[1], L[2]);
    /* A word longer than a line is split across lines, nothing lost. */
    char word[HOST_OSD_COLS * 2 + 6];
    memset(word, 'w', sizeof word - 1); word[sizeof word - 1] = 0;
    n = wrap(word);
    CHECK(n == 3 && (int)(strlen(L[0]) + strlen(L[1]) + strlen(L[2])) == (int)strlen(word) && !cut,
          "long word split over %d lines", n);
    /* Too long: three lines, the last ends in "...", and the caller is told. */
    char huge[400];
    memset(huge, 0, sizeof huge);
    for (int i = 0; i < 60; ++i) strcat(huge, "word ");
    n = wrap(huge);
    CHECK(n == HOST_OSD_LINES && cut && strlen(L[2]) <= HOST_OSD_COLS &&
          !strcmp(L[2] + strlen(L[2]) - 3, "..."), "over-long message is cut with dots: [%s]", L[2]);
    /* Every line always fits. */
    for (int i = 0; i < HOST_OSD_LINES; ++i) CHECK(strlen(L[i]) <= HOST_OSD_COLS, "line %d width", i);
    /* Runs of spaces do not make empty lines or leading blanks. */
    CHECK(wrap("  spaced    out   text that goes past the width of one line   ") == 2 && L[0][0] == 's' && L[1][0] != ' ',
          "spaces collapse: [%s] [%s]", L[0], L[1]);

    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: host toast wrapping, %d checks\n", checks);
    return 0;
}
