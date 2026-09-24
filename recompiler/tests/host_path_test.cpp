#include "host_path.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using namespace PSXRecompV4;
static int checks = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    for (const char* spelling : {
            R"(\\server\share\BIOS files\SCPH5500.BIN)",
            "//server/share/Game (Japan)/game.cue",
            R"(\\?\UNC\server\share\game.cue)",
            R"(\\?\C:\Game files\game.cue)",
            R"(Z:\Game files\game.cue)"}) {
        const fs::path path(spelling);
        check(host_path_is_absolute(path), "fully qualified Windows path");
        std::error_code ec = std::make_error_code(std::errc::invalid_argument);
        check(host_absolute(path, ec).native() == path.native() && !ec,
              "nonthrowing absolute preserves exact Windows path and clears error");
        check(host_absolute(path).native() == path.native(),
              "throwing absolute preserves exact Windows path");
        check(host_resolve(R"(D:\unrelated\config)", path).native() == path.native(),
              "config root never replaces an absolute Windows path");
    }
    check(!host_path_is_absolute(R"(\current-drive\file)"), "drive-rooted path needs current drive");
    check(!host_path_is_absolute(R"(C:drive-relative.bin)"), "drive-relative path needs drive directory");
#else
    check(host_absolute("/tmp/PSX files/game.cue") == fs::path("/tmp/PSX files/game.cue"),
          "POSIX absolute path unchanged");
    check(!host_path_is_absolute(R"(\\server\share\file)"), "backslashes remain ordinary on POSIX");
#endif
    const fs::path relative("relative directory/game.cue");
    check(!host_path_is_absolute(relative), "ordinary relative path");
    check(host_absolute(relative) == fs::absolute(relative), "relative path anchors to cwd");
    const auto root = fs::current_path() / "config directory";
    check(host_resolve(root, relative) == root / relative, "relative config value anchors to root");
    // T211 host root tokens.
    {
#ifdef _WIN32
        _putenv_s("PSXRECOMP_SHARE_ROOT", "");
        _putenv_s("PSXRECOMP_MEDIA_ROOT", "");
#else
        unsetenv("PSXRECOMP_SHARE_ROOT");
        unsetenv("PSXRECOMP_MEDIA_ROOT");
#endif
        check(host_expand_roots("${R}/bios/SCPH1001.BIN") == fs::path("${R}/bios/SCPH1001.BIN"),
              "unset root token stays as written");
        check(host_expand_roots("plain/disc.cue") == fs::path("plain/disc.cue"), "no token: unchanged");
        check(host_expand_roots("${X}/disc.cue") == fs::path("${X}/disc.cue"), "unknown token: unchanged");
        const fs::path share = fs::current_path() / "share root";
        const fs::path media = fs::current_path() / "PS1 Games";
#ifdef _WIN32
        _putenv_s("PSXRECOMP_SHARE_ROOT", share.string().c_str());
        _putenv_s("PSXRECOMP_MEDIA_ROOT", media.string().c_str());
#else
        setenv("PSXRECOMP_SHARE_ROOT", share.string().c_str(), 1);
        setenv("PSXRECOMP_MEDIA_ROOT", media.string().c_str(), 1);
#endif
        check(host_expand_roots("${R}/inputs/bios/SCPH1001.BIN") == share / "inputs/bios/SCPH1001.BIN",
              "${R} expands to the share root");
        check(host_expand_roots(R"(${D}\Syphon Filter 2 (USA) (Disc 1).chd)") ==
                  media / fs::path("Syphon Filter 2 (USA) (Disc 1).chd"),
              "${D} expands with a backslash separator");
        check(host_expand_roots("${R}") == share, "bare token is the root itself");
        check(host_resolve(fs::current_path() / "unrelated", "${D}/game.cue") == media / "game.cue",
              "host_resolve expands a token before anchoring");
        check(host_expand_roots("disc/${R}.cue") == fs::path("disc/${R}.cue"), "token only at the start");
    }
    // Optional private integration input: inspect only, never change the asset.
    if (argc == 2) {
        const fs::path path(argv[1]);
        check(bool(std::ifstream(path, std::ios::binary)), "original private path readable");
        check(bool(std::ifstream(host_absolute(path), std::ios::binary)), "resolved private path readable");
        check(host_absolute(path).native() == path.native(), "private path remains exact");
    }
    std::printf("host_path: %d checks passed\n", checks);
}
