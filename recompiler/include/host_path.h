#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace PSXRecompV4 {

// generic_string() can collapse a MinGW UNC root from "//server" to
// "/server". Persist the native spelling, changing separators only on
// Windows; a backslash in a POSIX filename is an ordinary character.
inline std::string host_path_forward_slashes(const std::filesystem::path& path) {
    std::string result = path.string();
#ifdef _WIN32
    for (char& c : result) if (c == '\\') c = '/';
#endif
    return result;
}

// Some MinGW libstdc++ builds treat a readable UNC path as drive-relative:
// absolute("\\\\server\\share\\file") becomes "D:\\server\\share\\file".
// Windows already considers UNC and device namespace paths fully qualified.
// Preserve their spelling before asking std::filesystem to anchor a path.
inline bool host_path_is_absolute(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& s = path.native();
    const auto separator = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if (s.size() > 2 && separator(s[0]) && separator(s[1]) && !separator(s[2]))
        return true;
#endif
    return path.is_absolute();
}

inline std::filesystem::path host_absolute(const std::filesystem::path& path,
                                          std::error_code& ec) {
    if (host_path_is_absolute(path)) {
        ec.clear();
        return path;
    }
    return std::filesystem::absolute(path, ec);
}

inline std::filesystem::path host_absolute(const std::filesystem::path& path) {
    if (host_path_is_absolute(path)) return path;
    return std::filesystem::absolute(path);
}

// A fully qualified config value must not be prefixed with its config root.
inline std::filesystem::path host_resolve(const std::filesystem::path& root,
                                         const std::filesystem::path& path) {
    return host_absolute(host_path_is_absolute(path) ? path : root / path);
}

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
