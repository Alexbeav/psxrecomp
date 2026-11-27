#pragma once

// Kept out of recompiler/include on purpose: every header there is an input
// of the BIOS emitter fingerprint, and this one is runtime-only.
#include "host_path.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace PSXRecompV4 {

// Windows refuses an ordinary path of MAX_PATH (260) characters or more unless
// the process and the system both opted into long paths; the extended-length
// "\\?\" spelling has no such limit (PS1B-620: a shared Clang store under a long
// LOCALAPPDATA was refused as corrupt). Returns that spelling for an absolute
// drive or UNC path on Windows, and the path unchanged otherwise. The prefix
// turns off Win32 name parsing, so the path is normalised first. MinGW
// libstdc++ sees no root name in "\\server\share" and its lexically_normal()
// collapses the leading pair, so a UNC path keeps its "\\" and only the part
// after it is normalised.
inline std::filesystem::path host_long_path(const std::filesystem::path& path) {
#ifdef _WIN32
    std::error_code ec;
    std::wstring s = host_absolute(path, ec).native();
    if (ec) return path;
    for (wchar_t& c : s) if (c == L'/') c = L'\\';
    if (s.rfind(L"\\\\?\\", 0) == 0 || s.rfind(L"\\\\.\\", 0) == 0) return s;
    if (s.size() > 2 && s[0] == L'\\' && s[1] == L'\\')
        return L"\\\\?\\UNC\\" + std::filesystem::path(s.substr(2)).lexically_normal().native();
    s = std::filesystem::path(s).lexically_normal().native();
    if (s.size() > 2 && s[1] == L':' && s[2] == L'\\') return L"\\\\?\\" + s;
#endif
    return path;
}

} // namespace PSXRecompV4
