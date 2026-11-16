/* settings_boot_keys_probe — PS1B-394.
 *
 * Prints what the settings loader makes of the two boot keys of one
 * settings.toml, so that a tool written in another language can check the
 * file it writes against the real loader:
 *
 *   settings_boot_keys_probe <settings.toml>
 *   format=<n> echoes=<0|1> has_fast_boot=<0|1> fast_boot=<0|1> has_bios_hle=<0|1> bios_hle=<0|1>
 *
 * Exit 0 when the file parses, 1 when it does not, 2 on a usage error.
 * tools/tasreplays/test_run_native_settings.py is the caller.
 */
#include "config_loader.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: settings_boot_keys_probe <settings.toml>\n");
        return 2;
    }
    const PSXRecompV4::UserSettings s = PSXRecompV4::load_user_settings(argv[1]);
    if (s.parse_error) {
        std::fprintf(stderr, "settings_boot_keys_probe: %s does not parse\n", argv[1]);
        return 1;
    }
    std::printf("format=%d echoes=%d has_fast_boot=%d fast_boot=%d has_bios_hle=%d bios_hle=%d\n",
                s.settings_format, s.boot_keys_were_echoes ? 1 : 0,
                s.has_fast_boot ? 1 : 0, s.fast_boot ? 1 : 0,
                s.has_bios_hle ? 1 : 0, s.bios_hle ? 1 : 0);
    return 0;
}
