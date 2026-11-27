#pragma once

// Directory walks, tree copies and tree removal that work on every path
// length (PS1B-703).
//
// MinGW-w64 libstdc++ walks a directory through the CRT's _wopendir. For a
// directory whose path is 260 characters or more, _wopendir lists the
// process's current directory instead, with no error, whether the path is
// spelled plainly or with "\\?\". std::filesystem::directory_iterator,
// recursive_directory_iterator, copy(recursive) and remove_all inherit that:
// a deep mod or texture-pack tree is listed wrongly, and remove_all on it
// never returns. FindFirstFileExW on the extended-length spelling lists the
// right entries.
//
// The helpers below use that on Windows and std::filesystem elsewhere. Entry
// paths keep the caller's spelling (folder / name), so callers can compare
// and relativise them as before.

#include "psx_host_long_path.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace PSXRecompV4 {

struct HostDirEntry {
    std::filesystem::path path;  // folder / name, in the caller's spelling
    bool is_directory = false;   // a real directory (not a link to one)
    bool is_regular_file = false;
    bool is_link = false;        // a symbolic link or other reparse point
    std::uintmax_t size = 0;     // bytes, regular files only
    int depth = 0;               // 0 for an entry of the walked folder itself
};

// The entries of one directory, without "." and "..". Sets ec and returns an
// empty list when the directory cannot be read.
inline std::vector<HostDirEntry> host_list_dir(const std::filesystem::path& dir,
                                               std::error_code& ec) {
    ec.clear();
    std::vector<HostDirEntry> out;
#ifdef _WIN32
    std::wstring pattern = host_long_path(dir).native();
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        ec.assign(static_cast<int>(GetLastError()), std::system_category());
        return out;
    }
    do {
        const wchar_t* n = fd.cFileName;
        if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0))
            continue;
        HostDirEntry e;
        e.path = dir / n;
        const DWORD a = fd.dwFileAttributes;
        e.is_link = (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.is_directory = !e.is_link && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.is_regular_file = !e.is_link && !(a & FILE_ATTRIBUTE_DIRECTORY);
        if (e.is_regular_file)
            e.size = (static_cast<std::uintmax_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    const DWORD last = GetLastError();
    FindClose(h);
    if (last != ERROR_NO_MORE_FILES) {
        ec.assign(static_cast<int>(last), std::system_category());
        out.clear();
    }
#else
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        HostDirEntry e;
        e.path = it->path();
        std::error_code sec;
        e.is_link = it->is_symlink(sec);
        e.is_directory = !e.is_link && it->is_directory(sec);
        e.is_regular_file = !e.is_link && it->is_regular_file(sec);
        if (e.is_regular_file) e.size = it->file_size(sec);
        out.push_back(std::move(e));
    }
    if (ec) out.clear();
#endif
    return out;
}

// What a tree visitor wants next.
enum class HostVisit {
    Continue,  // go on; for a directory, walk into it next
    Skip,      // go on, but do not walk into this directory
    Stop,      // end the walk now
};

namespace detail {
template <class Visit>
bool host_visit_level(const std::filesystem::path& dir, int depth, std::error_code& ec,
                      bool skip_unreadable, Visit& visit) {
    std::vector<HostDirEntry> entries = host_list_dir(dir, ec);
    if (ec) {
        if (depth > 0 && skip_unreadable) { ec.clear(); return true; }
        return false;
    }
    for (HostDirEntry& e : entries) {
        e.depth = depth;
        const HostVisit next = visit(static_cast<const HostDirEntry&>(e));
        if (next == HostVisit::Stop) return false;
        if (next == HostVisit::Continue && e.is_directory &&
            !host_visit_level(e.path, depth + 1, ec, skip_unreadable, visit))
            return false;
    }
    return true;
}
} // namespace detail

// Calls visit(const HostDirEntry&) for every entry below root, depth first,
// each directory before its contents, and returns a HostVisit. Links are
// reported and never followed. With skip_unreadable, a subdirectory that
// cannot be read is passed over (like directory_options::
// skip_permission_denied); otherwise the walk ends there with ec set. An
// exception thrown by visit ends the walk and propagates.
template <class Visit>
void host_visit_tree(const std::filesystem::path& root, std::error_code& ec, Visit visit,
                     bool skip_unreadable = false) {
    ec.clear();
    detail::host_visit_level(root, 0, ec, skip_unreadable, visit);
}

// Every entry below root, in host_visit_tree's order.
inline std::vector<HostDirEntry> host_walk_tree(const std::filesystem::path& root,
                                                std::error_code& ec,
                                                bool skip_unreadable = false) {
    std::vector<HostDirEntry> out;
    host_visit_tree(root, ec, [&](const HostDirEntry& e) {
        out.push_back(e);
        return HostVisit::Continue;
    }, skip_unreadable);
    return out;
}

// Copies the directory from, with everything below it, to the directory to,
// like std::filesystem::copy with copy_options::recursive: folders are
// created as needed, an existing file is an error, links are not followed.
inline void host_copy_tree(const std::filesystem::path& from,
                           const std::filesystem::path& to, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    auto make_dir = [&](const std::filesystem::path& p) -> bool {
        const std::wstring w = host_long_path(p).native();
        if (CreateDirectoryW(w.c_str(), nullptr)) return true;
        const DWORD err = GetLastError();
        const DWORD a = GetFileAttributesW(w.c_str());
        if (err == ERROR_ALREADY_EXISTS && a != INVALID_FILE_ATTRIBUTES &&
            (a & FILE_ATTRIBUTE_DIRECTORY))
            return true;
        ec.assign(static_cast<int>(err), std::system_category());
        return false;
    };
    if (!make_dir(to)) return;
    std::error_code walk_ec;
    host_visit_tree(from, walk_ec, [&](const HostDirEntry& e) {
        const std::filesystem::path dest = to / e.path.lexically_relative(from);
        if (e.is_directory) return make_dir(dest) ? HostVisit::Continue : HostVisit::Stop;
        if (e.is_link) return HostVisit::Skip;
        if (!CopyFileW(host_long_path(e.path).c_str(), host_long_path(dest).c_str(), TRUE)) {
            ec.assign(static_cast<int>(GetLastError()), std::system_category());
            return HostVisit::Stop;
        }
        return HostVisit::Continue;
    });
    if (!ec && walk_ec) ec = walk_ec;
#else
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ec);
#endif
}

// Removes path and, for a directory, everything below it, like
// std::filesystem::remove_all: a missing path is not an error, a link is
// removed and not followed. Returns the number of entries removed, or
// static_cast<std::uintmax_t>(-1) with ec set.
inline std::uintmax_t host_remove_tree(const std::filesystem::path& path,
                                       std::error_code& ec) {
    ec.clear();
    constexpr std::uintmax_t failed = static_cast<std::uintmax_t>(-1);
#ifdef _WIN32
    const std::wstring top = host_long_path(path).native();
    const DWORD attr = GetFileAttributesW(top.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return 0;
        ec.assign(static_cast<int>(err), std::system_category());
        return failed;
    }
    auto remove_one = [&](const std::filesystem::path& p) -> bool {
        const std::wstring w = host_long_path(p).native();
        const DWORD a = GetFileAttributesW(w.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(w.c_str(), a & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
        // A directory, or a link to one, goes with RemoveDirectoryW, which
        // removes the link itself and never its target.
        const bool dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        if (dir ? RemoveDirectoryW(w.c_str()) : DeleteFileW(w.c_str())) return true;
        ec.assign(static_cast<int>(GetLastError()), std::system_category());
        return false;
    };
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        return remove_one(path) ? 1 : failed;
    std::vector<HostDirEntry> all = host_walk_tree(path, ec);
    if (ec) return failed;
    std::uintmax_t count = 0;
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (!remove_one(it->path)) return failed;
        ++count;
    }
    if (!remove_one(path)) return failed;
    return count + 1;
#else
    return std::filesystem::remove_all(path, ec);
#endif
}

} // namespace PSXRecompV4
