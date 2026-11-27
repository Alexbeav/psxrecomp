/* replay_file_kind.h — is a file a replay (PSXRTI route), by its own bytes?
 *
 * PS1B-675: Windows hands a file dropped on a game, or opened with it, to the
 * product as its only argument. main() read every bare argument as the old
 * positional BIOS alias, so a dropped .psxrpl was checked as a BIOS image and
 * refused with "BIOS Mismatch". The file's first eight bytes decide instead:
 * every replay and input route the runtime writes starts with "PSXRTI1\0",
 * "PSXRTI2\0" or "PSXRTI3\0" (input_route_file.h, input_dualshock_route_file.h,
 * input_route_v3_file.h), and a BIOS image starts with MIPS code. The name and
 * extension are not trusted: a renamed file is still what its bytes say. */
#ifndef PSX_REPLAY_FILE_KIND_H
#define PSX_REPLAY_FILE_KIND_H

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the file opens and starts with a PSXRTI magic of version 1, 2 or 3;
 * 0 otherwise (missing, unreadable, shorter than the magic, anything else). */
static inline int psx_file_is_replay(const char *path) {
    unsigned char m[8];
    FILE *f;
    size_t n;
    if (!path || !path[0] || !(f = fopen(path, "rb"))) return 0;
    n = fread(m, 1, sizeof m, f);
    fclose(f);
    return n == sizeof m && !memcmp(m, "PSXRTI", 6) && m[6] >= '1' && m[6] <= '3' && m[7] == 0;
}

#ifdef __cplusplus
}
#endif

#endif
