// bios_runtime_exports_test.cpp — PS1B-454 part A row 3: the BIOS profiles
// that carry a verified boot shell entry export it, and the runtime's boot
// plan built from that export skips the BIOS boot.
//
// The fix is c4ec298eb (Future Cop on SCPH-5552). bios/SCPH5552.toml exported
// no shell_entry_phys, so the generated psx_bios_image carried 0, the boot
// plan refused the boot-skip (boot_skip_denied), and a player who chose
// SCPH-5552 sat through the BIOS boot animation while the same build on
// SCPH-1001 went straight to the game. LoadRunShell (ROM 0xBFC06FF0) jumps to
// 0x80030000 on every image listed below.
//
// Rows:
//   E1  Each profile below loads through the recompiler's own loader and
//       exports shell_entry_phys = 0x00030000.
//   E2  A boot plan built from that export, as main.cpp builds it
//       (have_shell_entry = shell_entry_phys != 0), skips the boot for
//       bios_hle and for the fast_boot alias, and denies nothing.
//   E3  SCPH-5552 also exports deliver_event_ret = 0x80001720, as its profile
//       says (kernel RAM 0x1720, verified identical to SCPH-1001).
//
// Usage: bios_runtime_exports_test (run from the framework root)

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "../src/config_loader.h"
#include "bios_hle_plan.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", (m).c_str()); ++failures; } } while (0)

static PsxBiosHlePlan plan_for(const PSXRecompV4::BiosConfig &cfg, int bios_hle, int fast_boot) {
    PsxBiosHleRequest req{};
    req.bios_hle = bios_hle;
    req.keep_intro = 0;
    req.fast_boot = fast_boot;
    req.have_deliver_event_ret = cfg.deliver_event_ret != 0;
    req.have_shell_entry = cfg.shell_entry_phys != 0;
    req.have_game_entry = 1;
    return psx_bios_hle_plan(req);
}

int main() {
    static const char *const profiles[] = {
        "bios/SCPH5552.toml",  // the fix under guard
        "bios/SCPH1001.toml",
        "bios/SCPH5500.toml",
        "bios/SCPH5501.toml",
        "bios/OpenBIOS.toml",
    };
    for (const char *path : profiles) {
        const std::string name = path;
        PSXRecompV4::BiosConfig cfg;
        try {
            cfg = PSXRecompV4::load_bios_config(path);
        } catch (const std::exception &e) {
            CHECK(false, name + ": the profile loads (" + e.what() + ")");
            continue;
        }
        char got[64];
        std::snprintf(got, sizeof got, "0x%08X", (unsigned)cfg.shell_entry_phys);
        CHECK(cfg.shell_entry_phys == 0x00030000u,
              "E1 " + name + ": exports shell_entry_phys = 0x00030000 (got " + got + ")");

        const PsxBiosHlePlan hle = plan_for(cfg, 1, 0);
        CHECK(hle.boot_skip == 1 && hle.boot_skip_denied == 0,
              "E2 " + name + ": bios_hle skips the BIOS boot and denies nothing");
        const PsxBiosHlePlan fast = plan_for(cfg, 0, 1);
        CHECK(fast.boot_skip == 1 && fast.boot_skip_denied == 0 && fast.call_hle == 0,
              "E2 " + name + ": the fast_boot alias skips the BIOS boot only");

        if (name == "bios/SCPH5552.toml") {
            std::snprintf(got, sizeof got, "0x%08X", (unsigned)cfg.deliver_event_ret);
            CHECK(cfg.deliver_event_ret == 0x80001720u,
                  "E3 " + name + ": exports deliver_event_ret = 0x80001720 (got " + got + ")");
        }
    }
    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::puts("PASS: the profiles export the boot shell entry and the boot plan skips the boot");
    return 0;
}
