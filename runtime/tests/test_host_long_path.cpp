// PS1B-620: the shared overlay Clang store is verified file by file. Its
// deepest header sits about 170 characters below LOCALAPPDATA, so a long
// profile path put members past MAX_PATH, and the narrow fopen the check used
// also fails for a folder name outside the ANSI code page. The start then
// refused with "shared overlay compiler unavailable or corrupt".
//
// This test builds both cases under TEMP and reads them the way the store
// check now does: is_regular_file and psx_sha256_file on host_long_path().
// It also records whether the old narrow spelling could open them; on a host
// without long-path support it cannot, which is the fault.
#include "host_path.h"
#include "psx_sha256.h"
#include "psx_sha256_file.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using PSXRecompV4::host_long_path;

static int failures;
static void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}

static bool same_digest(const fs::path& file, const std::string& bytes) {
    uint8_t want[32], got[32];
    psx_sha256_ctx ctx;
    psx_sha256_init(&ctx);
    psx_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    psx_sha256_final(&ctx, want);
    if (psx_sha256_file(host_long_path(file), got) != PsxFileHashResult::Ok) return false;
    return std::equal(want, want + 32, got);
}

// Remove the member and each folder above it up to root, one entry at a time.
// Not remove_all: with this MinGW libstdc++ a directory iterator below MAX_PATH
// depth lists the current directory instead, and remove_all never returns.
static void remove_member(const fs::path& root, const fs::path& file) {
    std::error_code ec;
    fs::remove(host_long_path(file), ec);
    for (fs::path dir = file.parent_path(); dir != root && dir.has_relative_path(); dir = dir.parent_path())
        fs::remove(host_long_path(dir), ec);
    fs::remove(host_long_path(root), ec);
}

static void member_case(const fs::path& file, const char* name) {
    const std::string bytes = std::string("member ") + name + '\n';
    std::error_code ec;
    fs::create_directories(host_long_path(file.parent_path()), ec);
    check(!ec, "create the member's folders through the long spelling");
    { std::ofstream(host_long_path(file), std::ios::binary) << bytes; }
    check(fs::is_regular_file(host_long_path(file), ec), "is_regular_file on host_long_path");
    check(same_digest(file, bytes), "psx_sha256_file on host_long_path reads the exact bytes");
    std::FILE* narrow = std::fopen(file.string().c_str(), "rb");
    std::cout << name << ": " << file.native().size() << " characters; the narrow fopen the old check used "
              << (narrow ? "opens it" : "cannot open it") << '\n';
    if (narrow) std::fclose(narrow);
}

int main() {
#ifdef _WIN32
    const fs::path root = fs::temp_directory_path() / "psx-long-path-test";

    // The real store layout, below a LOCALAPPDATA long enough to pass 260.
    const fs::path deep = root / std::string(120, 'L') / "PSXRecomp" / "toolchains" /
                          ("overlay-clang-" + std::string(64, 'a')) / "lib" / "clang" / "22" / "include" /
                          "openmp_wrappers" / "__clang_openmp_device_functions.h";
    // A profile folder name outside every ANSI code page.
    const fs::path wide = root / fs::u8path(u8"\u6771\u4eac") / "SHA256SUMS.txt";
    remove_member(root, deep);
    remove_member(root, wide);

    check(deep.native().size() >= 260, "precondition: the member path reaches MAX_PATH");
    member_case(deep, "long store member");
    member_case(wide, "non-ANSI store member");

    // Spellings: drive, UNC and an already extended path.
    check(host_long_path(fs::path(L"C:/a/./b/../c")).native() == L"\\\\?\\C:\\a\\c", "drive path");
    check(host_long_path(fs::path(L"\\\\srv\\share\\x\\..\\y")).native() == L"\\\\?\\UNC\\srv\\share\\y", "UNC path");
    check(host_long_path(fs::path(L"\\\\?\\C:\\x")).native() == L"\\\\?\\C:\\x", "extended path kept");

    remove_member(root, deep);
    remove_member(root, wide);
#else
    check(host_long_path("/a/b") == fs::path("/a/b"), "POSIX paths are unchanged");
#endif
    if (failures) return 1;
    std::cout << "host_long_path_test: all checks passed\n";
    return 0;
}
