#pragma once

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace PSXRecompV4 {

// Host root tokens (T211, TARGET-STATE section 3). A config path may start with
// ${R} (the share root, $PSXRECOMP_SHARE_ROOT) or ${D} (the media root,
// $PSXRECOMP_MEDIA_ROOT), so one install on a share names no drive letter or
// mount point. A token whose variable is unset or empty is left as written: the
// path then fails to open instead of resolving somewhere unintended. A path
// without a leading token is returned unchanged.
inline std::filesystem::path host_expand_roots(const std::filesystem::path& path) {
    using native_t = std::filesystem::path::string_type;
    const native_t& s = path.native();
    if (s.size() < 4 || s[0] != '$' || s[1] != '{' || s[3] != '}') return path;
    const char* var = s[2] == 'R' ? "PSXRECOMP_SHARE_ROOT"
                    : s[2] == 'D' ? "PSXRECOMP_MEDIA_ROOT" : nullptr;
    if (!var) return path;
    const char* value = std::getenv(var);
    if (!value || !value[0]) return path;
    native_t rest = s.substr(4);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    const std::filesystem::path base(value);
    return rest.empty() ? base : base / std::filesystem::path(rest);
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
    const std::filesystem::path p = host_expand_roots(path);
    return host_absolute(host_path_is_absolute(p) ? p : root / p);
}

} // namespace PSXRecompV4
