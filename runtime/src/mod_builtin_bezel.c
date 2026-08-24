/*
 * Framework-owned bezel presentation mod.
 *
 * The renderer primitive is generic, but artwork is data. A trusted plugin
 * resolves package-local bezel.png and asks the runtime to draw it behind the
 * game image; package archives can ship different artwork without executing
 * native code.
 */
#include "mod_plugins.h"

#include <stdio.h>

static void builtin_bezel_activate(void) {
    char path[1024] = "";
    if (!psx_mod_current_package_file("bezel.png", path, sizeof path)) {
        fprintf(stderr, "psxrecomp: bezel package has no resolvable bezel.png\n");
        return;
    }
    (void)psx_mod_set_bezel_artwork(path);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_bezel_plugin) {
    (void)psx_mod_register_activation_plugin("psx.bezel",
                                             builtin_bezel_activate);
}
