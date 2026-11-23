/* psx_shared_bios.h -- the one per-user folder where every PS1 port keeps the
 * player's retail BIOS dump (PS1B-677, amendment A1).
 *
 * bios.cfg is per title, so a player who owns fifty ports picked the same
 * BIOS fifty times. A port now looks here before it asks, and a pick in any
 * port stores a copy here, so one pick covers them all. A per-title bios.cfg
 * and --bios still win over this folder.
 *
 * The folder holds the dump itself under its usual name (SCPH5552.BIN), not a
 * path to it, so moving or deleting the original does not break every port.
 * Ports pinned to different images find their own file by name and CRC.
 *
 *   PSXRECOMP_BIOS_DIR, when set        (tests, portable installs)
 *   Windows: %LOCALAPPDATA%\psxrecomp\bios
 *   elsewhere: $XDG_DATA_HOME/psxrecomp/bios, else ~/.local/share/psxrecomp/bios
 *
 * The same base folders the toolchain cache uses (psxrecomp_codegen_host.c,
 * tools/toolchain_pack.py).
 */
#ifndef PSX_SHARED_BIOS_H
#define PSX_SHARED_BIOS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define PSX_SHARED_BIOS_SEP "\\"
#else
#define PSX_SHARED_BIOS_SEP "/"
#endif

/* Writes the folder's path into out. Returns 1 when one could be formed (the
 * folder need not exist yet), 0 when no base folder is known. */
static inline int psx_shared_bios_dir(char* out, size_t cap) {
    const char* env = getenv("PSXRECOMP_BIOS_DIR");
    int n;
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (env && env[0]) {
        n = snprintf(out, cap, "%s", env);
        return n > 0 && (size_t)n < cap;
    }
#if defined(_WIN32)
    env = getenv("LOCALAPPDATA");
    if (!env || !env[0])
        return 0;
    n = snprintf(out, cap, "%s\\psxrecomp\\bios", env);
#else
    env = getenv("XDG_DATA_HOME");
    if (env && env[0]) {
        n = snprintf(out, cap, "%s/psxrecomp/bios", env);
    } else {
        env = getenv("HOME");
        if (!env || !env[0])
            return 0;
        n = snprintf(out, cap, "%s/.local/share/psxrecomp/bios", env);
    }
#endif
    if (n <= 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

#endif /* PSX_SHARED_BIOS_H */
