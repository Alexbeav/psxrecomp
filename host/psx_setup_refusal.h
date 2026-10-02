/* psx_setup_refusal.h -- the disc file that setup refused last (PS1B-415).
 *
 * Two checks judge the disc a player selects in the setup window. The disc
 * panel asks the game program, which looks at the ISO header and the serial.
 * "Generate & rebuild" asks the CLI, which compares the data track with the
 * kit and can refuse (exit code 3). Nothing connected them: a first pressing
 * with the kit's serial showed "Disc verified" in the panel and "Disc
 * verification failed: ..." under it.
 *
 * The setup host remembers the one file the CLI refused, and the disc panel's
 * verdict follows it. These are the rules, kept apart from the host so that a
 * test can run them:
 *
 *   - only exit code 3 remembers a file, and only when the CLI says that the
 *     refused file is the selected one (a set can refuse another of its
 *     discs); a build failure, a toolchain failure or a prepare that never
 *     started leave what is remembered as it is;
 *   - a prepare that succeeds forgets it (the player replaced the file in
 *     place and pressed Generate again);
 *   - a disc check of another file forgets it.
 *
 * The caller owns the buffer. It is memory of the running program only: the
 * host writes it to no file, and a game the host hands over to is another
 * process and starts with none.
 */
#ifndef PSX_SETUP_REFUSAL_H
#define PSX_SETUP_REFUSAL_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define PSX_SETUP_EXIT_VERIFY 3L   /* psxrecomp_cli.py EXIT_VERIFY */

/* 1 when the two paths name the same file as the player selected it: equal
 * but for the direction of a slash and, on Windows, the case of a letter. */
static int psx_setup_refusal_same_file(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
#if defined(_WIN32)
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
#endif
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* 1 when `line` of the CLI's JSON progress is a refusal of the very file that
 * was given as `--disc`. A game whose discs are separate programs checks
 * every disc of the set, and can refuse one that is not the selected file:
 * that refusal must not mark the selected one. The CLI writes compact JSON
 * with sorted keys (tools/sdk_progress.py). The same words inside a message
 * do not match: a quote inside a JSON string is written with a backslash. */
static int psx_setup_refusal_line_names_given(const char* line) {
    if (!line || line[0] != '{') return 0;
    if (!strstr(line, "\"event\":\"error\"")) return 0;
    return strstr(line, "\"refused_given_disc\":true") != NULL;
}

/* A prepare of `path` ended with the CLI's exit code. `given_refused` is 1
 * when the CLI said that the refused file is `path` itself. */
static void psx_setup_refusal_after_prepare(char* remembered, size_t cap,
                                            const char* path, long exit_code,
                                            int given_refused) {
    if (!remembered || !cap) return;
    if (exit_code == 0) {
        remembered[0] = '\0';
    } else if (exit_code == PSX_SETUP_EXIT_VERIFY && given_refused && path && path[0]) {
        snprintf(remembered, cap, "%s", path);
    }
}

/* The disc panel checks `path`. Returns 1 when it is the file setup refused.
 * Another file forgets the refusal. */
static int psx_setup_refusal_at_check(char* remembered, const char* path) {
    if (!remembered || !remembered[0]) return 0;
    if (psx_setup_refusal_same_file(remembered, path)) return 1;
    remembered[0] = '\0';
    return 0;
}

#endif /* PSX_SETUP_REFUSAL_H */
