// Exercise the CLI's private selector without loading a disc or BIOS image.
#define main unused_cli_main
#include "../src/main_cli.cpp"
#undef main

int main() {
    try {
        for (const char* filename : {
                 "SCPH101.BIN", "scph101.bin", "US-PSOne-SCPH101.BIN"}) {
            bool refused = false;
            try {
                select_bios_profile(filename);
            } catch (const std::runtime_error& error) {
                const std::string message = error.what();
                refused = message.find("SCPH-101 support has been retired") != std::string::npos &&
                          message.find("OpenBIOS, SCPH1001, or SCPH5552") != std::string::npos;
            }
            if (!refused)
                throw std::runtime_error("CLI did not clearly refuse " + std::string(filename));
        }

        struct Case {
            const char* filename;
            const char* profile;
            const char* stem;
        };
        for (const Case& item : {
                 Case{"openbios.bin", "OpenBIOS.toml", "OpenBIOS"},
                 Case{"SCPH1001.BIN", "SCPH1001.toml", "SCPH1001"},
                 Case{"us-psx-scph1001.bin", "SCPH1001.toml", "SCPH1001"},
                 Case{"SCPH5552.BIN", "SCPH5552.toml", "SCPH5552"},
                 Case{"EUR-PSX-SCPH5552.bin", "SCPH5552.toml", "SCPH5552"}}) {
            const BiosProfile& profile = select_bios_profile(item.filename);
            if (profile.filename != item.profile || profile.out_stem != item.stem)
                throw std::runtime_error("CLI changed the profile for " + std::string(item.filename));
        }
        fmt::print("PASS: retired BIOS refused; remaining CLI profiles preserved\n");
        return 0;
    } catch (const std::exception& error) {
        fmt::print("FAIL: {}\n", error.what());
        return 1;
    }
}
