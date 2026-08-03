/* Portable generate → rebuild (--no-pgo from setup) → relaunch host. */

#include "psxrecomp_codegen_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

static const PsxrecompCodegenHostConfig* g_cfg;
static char g_project_root[1024];
static char g_cli_path[1100];
static char g_game_toml[1100];
static char g_python[512];
static char g_cmake[512];
static char g_build_dir[1100];
static char g_exe_path[1100];
static char g_helper_path[1100];
static char g_cmake_target[256];
static char g_exe_basename[256];
static char g_display[128];
static char g_toolchain_bin[1400];
/* Last ensure-toolchain JSONL result path (bin/); shared-cache fallback. */
static char g_cli_toolchain_bin[1400];
static int g_ready;
static int g_relaunch_is_helper;

static const char* cfg_or(const char* v, const char* d) {
    return (v && v[0]) ? v : d;
}

static int path_is_file(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int path_is_dir(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int join_path(char* out, size_t cap, const char* a, const char* b) {
    size_t na = strlen(a);
    int need_slash = na > 0 && a[na - 1] != '/' && a[na - 1] != '\\';
    int n = snprintf(out, cap, "%s%s%s", a, need_slash ? "/" : "", b);
    return n > 0 && (size_t)n < cap;
}

#if defined(_WIN32)
/* Microsoft Store Python redirects %LOCALAPPDATA% writes into
 * Packages\PythonSoftwareFoundation.Python.*\LocalCache\Local\...
 * Map a virtual LocalAppData path to those on-disk mirrors. */
static int store_python_localcache_mirror(const char* virtual_path, char* out,
                                          size_t cap) {
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !local[0] || !virtual_path || !virtual_path[0])
        return 0;
    size_t llen = strlen(local);
    if (_strnicmp(virtual_path, local, (int)llen) != 0)
        return 0;
    const char* suffix = virtual_path + llen;
    if (*suffix != '\\' && *suffix != '/')
        return 0;
    while (*suffix == '\\' || *suffix == '/')
        ++suffix;
    if (!suffix[0])
        return 0;

    char packages[1100];
    if (!join_path(packages, sizeof(packages), local, "Packages"))
        return 0;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern),
             "%s\\PythonSoftwareFoundation.Python.*", packages);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        char mirror[1400];
        int n = snprintf(mirror, sizeof(mirror),
                         "%s\\%s\\LocalCache\\Local\\%s", packages,
                         fd.cFileName, suffix);
        if (n <= 0 || (size_t)n >= sizeof(mirror))
            continue;
        DWORD attr = GetFileAttributesA(mirror);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s", mirror);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* Prefer the real on-disk directory (handles Store Python redirection). */
static int resolve_existing_dir(const char* path, char* out, size_t cap) {
    if (path && path[0] && path_is_dir(path)) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
    char alt[1400];
    if (path && store_python_localcache_mirror(path, alt, sizeof(alt)) &&
        path_is_dir(alt)) {
        snprintf(out, cap, "%s", alt);
        return 1;
    }
    return 0;
}

static int python_path_is_store(const char* path) {
    if (!path || !path[0])
        return 0;
    char lower[1100];
    size_t n = strlen(path);
    if (n >= sizeof(lower))
        n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = path[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    return strstr(lower, "windowsapps") != NULL ||
           strstr(lower, "pythonsoftwarefoundation") != NULL;
}

/* Run a short cmdline and capture the first stdout line (trimmed). */
static int capture_cmd_first_line(const char* cmdline, char* out, size_t cap) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[1024];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    size_t got = 0;
    DWORD n = 0;
    out[0] = '\0';
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n') {
                out[got] = '\0';
                got = cap; /* mark complete */
                break;
            }
            if (got + 1 < cap)
                out[got++] = c;
        }
        if (got >= cap)
            break;
    }
    if (got > 0 && got < cap)
        out[got] = '\0';
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 8000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out[0] != '\0';
}
#else
static int resolve_existing_dir(const char* path, char* out, size_t cap) {
    if (path && path[0] && path_is_dir(path)) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
    return 0;
}

#endif

static int dirname_copy(char* out, size_t cap, const char* path) {
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\')
        --n;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    if (n == 0) {
        if (cap < 2) return 0;
        out[0] = '.';
        out[1] = '\0';
        return 1;
    }
    if (n >= cap) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

static int resolve_cli_path(const char* root, char* out, size_t cap) {
    const char* candidates[] = {
        cfg_or(g_cfg->psxrecomp_cli_relpath, "psxrecomp/psxrecomp_cli.py"),
        "psxrecomp/psxrecomp_cli.py",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i] || !candidates[i][0])
            continue;
        if (!join_path(out, cap, root, candidates[i]))
            continue;
        if (path_is_file(out))
            return 1;
    }
    return 0;
}

static int looks_like_project_root(const char* root) {
    char cli[1100], toml[1100];
    if (!join_path(toml, sizeof(toml), root,
                   cfg_or(g_cfg->seed_cfg_relpath, "game.toml")))
        return 0;
    if (!path_is_file(toml))
        return 0;
    return resolve_cli_path(root, cli, sizeof(cli));
}

static int find_on_path(const char* name, char* out, size_t cap) {
#if defined(_WIN32)
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "where %s >nul 2>nul", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#else
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#endif
    return 0;
}

static int find_python(char* out, size_t cap) {
    const char* env = getenv("PYTHON");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
#if defined(_WIN32)
    /* Prefer python.org / py-launcher installs over the Microsoft Store
     * stub: Store Python redirects LocalAppData writes into LocalCache. */
    char resolved[1100];
    if (capture_cmd_first_line(
            "py -3 -c \"import sys; print(sys.executable)\"", resolved,
            sizeof(resolved)) &&
        path_is_file(resolved) && !python_path_is_store(resolved)) {
        snprintf(out, cap, "%s", resolved);
        return 1;
    }
    if (capture_cmd_first_line(
            "python -c \"import sys; print(sys.executable)\"", resolved,
            sizeof(resolved)) &&
        path_is_file(resolved)) {
        snprintf(out, cap, "%s", resolved);
        return 1;
    }
    const char* candidates[] = {"python.exe", "python3.exe", "py.exe"};
#else
    const char* candidates[] = {"python3", "python"};
#endif
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (find_on_path(candidates[i], out, cap))
            return 1;
    }
    return 0;
}

static int toolchain_bin_has_cmake(const char* bin, char* out, size_t cap) {
    char cmake[1200];
#if defined(_WIN32)
    if (join_path(cmake, sizeof(cmake), bin, "cmake.exe") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#else
    if (join_path(cmake, sizeof(cmake), bin, "cmake") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#endif
    return 0;
}

static int resolve_toolchain_bin_under(const char* wrap, char* out, size_t cap) {
    char cand[1100], cmake[1200], root[1400];
    if (!wrap || !wrap[0])
        return 0;
    if (!resolve_existing_dir(wrap, root, sizeof(root)))
        return 0;
    wrap = root;
    if (join_path(cand, sizeof(cand), wrap, "bin") &&
        toolchain_bin_has_cmake(cand, out, cap))
        return 1;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%s\\*", wrap);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, fd.cFileName))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (join_path(cmake, sizeof(cmake), nbin, "cmake.exe") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR* dir = opendir(wrap);
    if (!dir)
        return 0;
    int found = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, ent->d_name))
            continue;
        if (!path_is_dir(nested))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (join_path(cmake, sizeof(cmake), nbin, "cmake") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
#endif
}

/* Prefer tagged installs (CLI writes latest/ / offline/) then any child. */
static int resolve_toolchain_cache_base(const char* base, char* out, size_t cap) {
    static const char* prefer[] = {"latest", "offline", NULL};
    char cand[1100];
    if (!base || !base[0])
        return 0;
    for (int i = 0; prefer[i]; ++i) {
        if (join_path(cand, sizeof(cand), base, prefer[i]) &&
            resolve_toolchain_bin_under(cand, out, cap))
            return 1;
    }
    return resolve_toolchain_bin_under(base, out, cap);
}

/* Same layout as psxrecomp/tools/toolchain_pack.py shared_cache_roots().
 * RetComM (`retcomm`) is preferred; legacy `psxrecomp` is a fallback. */
static int resolve_shared_toolchain_cache(char* out, size_t cap) {
    char bases[8][1400];
    int n = 0;
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (local && local[0]) {
        if (join_path(bases[n], sizeof(bases[n]), local,
                      "retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < 8 &&
            join_path(bases[n], sizeof(bases[n]), local,
                      "psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
        /* Store Python may have written only into LocalCache mirrors. */
        for (int i = 0, lim = n; i < lim && n < 8; ++i) {
            char mirror[1400];
            if (store_python_localcache_mirror(bases[i], mirror,
                                              sizeof(mirror)))
                snprintf(bases[n++], sizeof(bases[0]), "%s", mirror);
        }
    }
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0]) {
        if (join_path(bases[n], sizeof(bases[n]), xdg,
                      "retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < 8 &&
            join_path(bases[n], sizeof(bases[n]), xdg,
                      "psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
    } else if (home && home[0]) {
        if (join_path(bases[n], sizeof(bases[n]), home,
                      ".local/share/retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < 8 &&
            join_path(bases[n], sizeof(bases[n]), home,
                      ".local/share/psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
    }
#endif
    for (int i = 0; i < n; ++i) {
        if (resolve_toolchain_cache_base(bases[i], out, cap))
            return 1;
    }
    return 0;
}

/* CLI writes project_root/toolchain/.psxrecomp-bin with the bin path. */
static int resolve_toolchain_stamp(char* out, size_t cap) {
    char stamp[1200], line[1400], resolved[1400];
    FILE* f;
    if (!g_project_root[0])
        return 0;
    if (!join_path(stamp, sizeof(stamp), g_project_root,
                   "toolchain/.psxrecomp-bin"))
        return 0;
#if defined(_WIN32)
    {
        char alt[1400];
        if (!path_is_file(stamp) &&
            store_python_localcache_mirror(stamp, alt, sizeof(alt)) &&
            path_is_file(alt))
            snprintf(stamp, sizeof(stamp), "%s", alt);
    }
#endif
    if (!path_is_file(stamp))
        return 0;
    f = fopen(stamp, "r");
    if (!f)
        return 0;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                 line[n - 1] == ' '))
        line[--n] = '\0';
    if (!line[0])
        return 0;
    if (resolve_existing_dir(line, resolved, sizeof(resolved)) &&
        toolchain_bin_has_cmake(resolved, out, cap))
        return 1;
    return 0;
}

static int resolve_toolchain_bin(char* out, size_t cap) {
    /* Fresh ensure-toolchain result (bin directory). */
    if (g_cli_toolchain_bin[0]) {
        char resolved[1400];
        if (resolve_existing_dir(g_cli_toolchain_bin, resolved,
                                 sizeof(resolved)) &&
            toolchain_bin_has_cmake(resolved, out, cap))
            return 1;
    }

    const char* env_keys[] = {
        "RETCOMM_TOOLCHAIN_DIR", "PSXRECOMP_TOOLCHAIN_DIR", "TOOLCHAIN_DIR",
        "BPE_TOOLCHAIN_DIR", NULL};
    for (int i = 0; env_keys[i]; ++i) {
        const char* e = getenv(env_keys[i]);
        if (e && e[0] && resolve_toolchain_bin_under(e, out, cap))
            return 1;
    }
    if (resolve_toolchain_stamp(out, cap))
        return 1;
    if (g_project_root[0]) {
        char wrap[1100];
        if (join_path(wrap, sizeof(wrap), g_project_root, "toolchain") &&
            resolve_toolchain_bin_under(wrap, out, cap))
            return 1;
    }
    /* CLI downloads land here — must match toolchain_pack.py. */
    if (resolve_shared_toolchain_cache(out, cap))
        return 1;
    return 0;
}

static void activate_toolchain_path(void) {
    char pack_root[1400];
    g_toolchain_bin[0] = '\0';
    if (!resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return;
    const char* old = getenv("PATH");
#if defined(_WIN32)
    char neu[8192];
    snprintf(neu, sizeof(neu), "%s%s%s", g_toolchain_bin, old ? ";" : "",
             old ? old : "");
    _putenv_s("PATH", neu);
#else
    char neu[8192];
    snprintf(neu, sizeof(neu), "%s%s%s", g_toolchain_bin, old ? ":" : "",
             old ? old : "");
    setenv("PATH", neu, 1);
#endif
    /* Pack root (parent of bin/) — Windows cmake-clang-v1 ships zlib here. */
    if (dirname_copy(pack_root, sizeof(pack_root), g_toolchain_bin) &&
        pack_root[0]) {
#if defined(_WIN32)
        _putenv_s("RETCOMM_TOOLCHAIN_DIR", pack_root);
        _putenv_s("ZLIB_ROOT", pack_root);
        {
            const char* prev = getenv("CMAKE_PREFIX_PATH");
            char pref[8192];
            if (prev && prev[0] && !strstr(prev, pack_root))
                snprintf(pref, sizeof(pref), "%s;%s", pack_root, prev);
            else if (!prev || !prev[0])
                snprintf(pref, sizeof(pref), "%s", pack_root);
            else
                pref[0] = '\0';
            if (pref[0])
                _putenv_s("CMAKE_PREFIX_PATH", pref);
        }
#else
        setenv("RETCOMM_TOOLCHAIN_DIR", pack_root, 1);
        setenv("ZLIB_ROOT", pack_root, 1);
        {
            const char* prev = getenv("CMAKE_PREFIX_PATH");
            char pref[8192];
            if (prev && prev[0] && !strstr(prev, pack_root))
                snprintf(pref, sizeof(pref), "%s:%s", pack_root, prev);
            else if (!prev || !prev[0])
                snprintf(pref, sizeof(pref), "%s", pack_root);
            else
                pref[0] = '\0';
            if (pref[0])
                setenv("CMAKE_PREFIX_PATH", pref, 1);
        }
#endif
    }
}

static int find_cmake(char* out, size_t cap) {
    const char* env = getenv("CMAKE");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
    char tc[1100], cand[1200];
    if (resolve_toolchain_bin(tc, sizeof(tc))) {
#if defined(_WIN32)
        if (join_path(cand, sizeof(cand), tc, "cmake.exe") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#else
        if (join_path(cand, sizeof(cand), tc, "cmake") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#endif
    }
#if defined(_WIN32)
    return find_on_path("cmake.exe", out, cap);
#else
    return find_on_path("cmake", out, cap);
#endif
}

static int discover_project_root(char* out, size_t cap) {
    const char* env_name =
        cfg_or(g_cfg->project_root_env, "PSXRECOMP_PROJECT_ROOT");
    const char* env = getenv(env_name);
    if (env && env[0] && looks_like_project_root(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }

    char start[1024];
#if defined(_WIN32)
    if (!GetCurrentDirectoryA((DWORD)sizeof(start), start))
        start[0] = '\0';
#else
    if (!getcwd(start, sizeof(start)))
        start[0] = '\0';
#endif

    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", start[0] ? start : ".");
    for (int i = 0; i < 10; ++i) {
        if (looks_like_project_root(cur)) {
            snprintf(out, cap, "%s", cur);
            return 1;
        }
        char parent[1024];
        if (!dirname_copy(parent, sizeof(parent), cur))
            break;
        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

static int resolve_build_paths(void) {
    const char* env_name =
        cfg_or(g_cfg->build_dir_env, "PSXRECOMP_BUILD_DIR");
    const char* env = getenv(env_name);
    if (env && env[0]) {
        snprintf(g_build_dir, sizeof(g_build_dir), "%s", env);
    } else {
        const char* names[] = {
            cfg_or(g_cfg->build_dir_name, "build"),
            "build-release",
            "build",
            "build-ci",
        };
        int found = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (!names[i] || !names[i][0])
                continue;
            char cand[1100];
            if (!join_path(cand, sizeof(cand), g_project_root, names[i]))
                continue;
            if (path_is_dir(cand)) {
                snprintf(g_build_dir, sizeof(g_build_dir), "%s", cand);
                found = 1;
                break;
            }
        }
        /* Setup zips ship without a pre-made build tree. Prefer the configured
         * name so rebuild can cmake -B it on first Generate & rebuild. */
        if (!found) {
            const char* prefer = cfg_or(g_cfg->build_dir_name, "build-release");
            if (!join_path(g_build_dir, sizeof(g_build_dir), g_project_root,
                           prefer))
                return 0;
        }
    }

    char exe_name[300];
#if defined(_WIN32)
    snprintf(exe_name, sizeof(exe_name), "%s.exe", g_exe_basename);
#else
    snprintf(exe_name, sizeof(exe_name), "%s", g_exe_basename);
#endif
    return join_path(g_exe_path, sizeof(g_exe_path), g_build_dir, exe_name);
}

static int bios_backends_missing(void) {
    char openbios[1100], scph[1100];
    if (!join_path(openbios, sizeof(openbios), g_project_root,
                   "psxrecomp/generated/OpenBIOS_dispatch.c"))
        return 1;
    if (!join_path(scph, sizeof(scph), g_project_root,
                   "psxrecomp/generated/SCPH1001_dispatch.c"))
        return 1;
    return !(path_is_file(openbios) || path_is_file(scph));
}

int psxrecomp_codegen_host_sources_missing(
    const PsxrecompCodegenHostConfig* cfg) {
    if (!cfg || !cfg->cmake_target || !cfg->exe_basename)
        return 0;
    g_cfg = cfg;
    if (!g_project_root[0] &&
        !discover_project_root(g_project_root, sizeof(g_project_root)))
        return 0;
    char marker[1100];
    if (!join_path(marker, sizeof(marker), g_project_root,
                   cfg_or(cfg->gen_marker_relpath,
                          "generated/SLUS_011.89_dispatch.c")))
        return 1;
    if (!path_is_file(marker))
        return 1;
    return bios_backends_missing();
}

static int read_line_file(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                 out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return n > 0;
}

static int write_line_file(const char* path, const char* line) {
    FILE* f;
    if (!path || !path[0])
        return 0;
    if (!line || !line[0]) {
        remove(path);
        return 1;
    }
    f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", line);
    fclose(f);
    return 1;
}

/* Sidecars are loaded next to argv[0]. Setup host is often zip-root while the
 * rebuilt binary lives under build/ — write beside the game binary too. */
static void write_sidecar_near_exe(const char* near_exe, const char* name,
                                   const char* value) {
    char dir[1100], path[1200];
    if (!near_exe || !near_exe[0] || !name || !name[0])
        return;
    if (!dirname_copy(dir, sizeof(dir), near_exe))
        return;
    if (!join_path(path, sizeof(path), dir, name))
        return;
    write_line_file(path, value ? value : "");
}

/* bios_path NULL = leave bios.cfg untouched; "" = clear (OpenBIOS); else write. */
static int host_persist_setup(void* ctx, const char* rom_path,
                              const char* bios_path) {
    char path[1200];
    (void)ctx;
    if (bios_path) {
        if (g_project_root[0] &&
            join_path(path, sizeof(path), g_project_root, "bios.cfg"))
            write_line_file(path, bios_path[0] ? bios_path : "");
        write_line_file("bios.cfg", bios_path[0] ? bios_path : "");
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "bios.cfg",
                                   bios_path[0] ? bios_path : "");
    }
    if (rom_path && rom_path[0]) {
        if (g_project_root[0] &&
            join_path(path, sizeof(path), g_project_root, "disc.cfg"))
            write_line_file(path, rom_path);
        write_line_file("disc.cfg", rom_path);
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "disc.cfg", rom_path);
    }
    return 0;
}

static void persist_relaunch_sidecars(const char* near_exe,
                                      const char* disc_path) {
    char bios_line[1024];
    char project_sidecar[1200];

    if (disc_path && disc_path[0]) {
        write_sidecar_near_exe(near_exe, "disc.cfg", disc_path);
        write_line_file("disc.cfg", disc_path);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "disc.cfg"))
            write_line_file(project_sidecar, disc_path);
    }

    bios_line[0] = '\0';
    if (!read_line_file("bios.cfg", bios_line, sizeof(bios_line)) &&
        g_project_root[0] &&
        join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                  "bios.cfg"))
        read_line_file(project_sidecar, bios_line, sizeof(bios_line));
    if (bios_line[0] && !path_is_file(bios_line))
        bios_line[0] = '\0';
    if (bios_line[0]) {
        write_sidecar_near_exe(near_exe, "bios.cfg", bios_line);
        write_line_file("bios.cfg", bios_line);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            write_line_file(project_sidecar, bios_line);
    }
}

static int resolve_bios_arg(char* out, size_t cap) {
    char cand[1100];
    char exe_dir[1100];
    if (join_path(cand, sizeof(cand), g_project_root, "bios.cfg") &&
        read_line_file(cand, out, cap) && path_is_file(out))
        return 1;
    if (g_exe_path[0] && dirname_copy(exe_dir, sizeof(exe_dir), g_exe_path) &&
        join_path(cand, sizeof(cand), exe_dir, "bios.cfg") &&
        read_line_file(cand, out, cap) && path_is_file(out))
        return 1;
    if (read_line_file("bios.cfg", out, cap) && path_is_file(out))
        return 1;
    out[0] = '\0';
    return 0;
}

static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static int json_get_number(const char* line, const char* key, double* out) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') ++p;
    char* end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static void handle_progress_line(const char* line,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!line || line[0] != '{')
        return;
    char event[64] = "";
    json_get_string(line, "event", event, sizeof(event));
    /* Capture ensure-toolchain result even when UI progress is absent. */
    if (strcmp(event, "result") == 0) {
        char tb[1400], resolved[1400];
        if (json_get_string(line, "toolchain_bin", tb, sizeof(tb)) && tb[0] &&
            resolve_existing_dir(tb, resolved, sizeof(resolved))) {
            snprintf(g_cli_toolchain_bin, sizeof(g_cli_toolchain_bin), "%s",
                     resolved);
            /* Env expects pack root (parent of bin/), not bin/ itself. */
            char pack_root[1400];
            if (dirname_copy(pack_root, sizeof(pack_root), resolved) &&
                pack_root[0]) {
#if defined(_WIN32)
                _putenv_s("PSXRECOMP_TOOLCHAIN_DIR", pack_root);
#else
                setenv("PSXRECOMP_TOOLCHAIN_DIR", pack_root, 1);
#endif
            }
        }
    }
    if (!on_progress)
        return;
    if (strcmp(event, "phase") == 0) {
        char message[240] = "";
        char phase[64] = "";
        double pct = -1.0;
        json_get_string(line, "message", message, sizeof(message));
        json_get_string(line, "phase", phase, sizeof(phase));
        if (!json_get_number(line, "pct", &pct))
            pct = -1.0;
        if (!message[0] && phase[0])
            snprintf(message, sizeof(message), "%s", phase);
        on_progress(progress_ctx, (float)pct, message[0] ? message : NULL);
    } else if (strcmp(event, "log") == 0 || strcmp(event, "error") == 0) {
        char message[240] = "";
        if (json_get_string(line, "message", message, sizeof(message)))
            on_progress(progress_ctx, -1.0f, message);
    }
}

#if defined(_WIN32)
static int run_cli_win(const char* cmdline,
                       RecompLauncherCPrepareProgressFn on_progress,
                       void* progress_ctx, char* err_msg, size_t err_cap,
                       const char* fail_label) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err_msg, err_cap, "CreatePipe failed.");
        return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[4096];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, 0, NULL,
                        g_project_root, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        snprintf(err_msg, err_cap, "Failed to spawn %s.", fail_label);
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    /* Long JSONL rows (toolchain paths under LocalAppData) need headroom. */
    char line[16384];
    size_t line_len = 0;
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = '\0';
                handle_progress_line(line, on_progress, progress_ctx);
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line))
                line[line_len++] = c;
        }
    }
    if (line_len) {
        line[line_len] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0) return 1;
    if (code == 3)
        snprintf(err_msg, err_cap, "Disc verification failed (wrong dump).");
    else
        snprintf(err_msg, err_cap, "%s failed (exit %lu).", fail_label,
                 (unsigned long)code);
    return 0;
}
#else
static int run_cli_posix(char* const argv[],
                         RecompLauncherCPrepareProgressFn on_progress,
                         void* progress_ctx, char* err_msg, size_t err_cap,
                         const char* fail_label) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
        return 0;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        snprintf(err_msg, err_cap, "Failed to spawn %s: %s", fail_label,
                 strerror(rc));
        return 0;
    }

    FILE* out = fdopen(pipefd[0], "r");
    if (!out) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        snprintf(err_msg, err_cap, "fdopen failed.");
        return 0;
    }
    char line[16384];
    while (fgets(line, sizeof(line), out)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
    }
    fclose(out);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
        return 0;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0) return 1;
    if (code == 3)
        snprintf(err_msg, err_cap, "Disc verification failed (wrong dump).");
    else
        snprintf(err_msg, err_cap, "%s failed (exit %d).", fail_label, code);
    return 0;
}
#endif

/* ---- Host-native toolchain install (no Store Python AppData redirect) ---- */

static const char* k_tc_repo = "TechnicallyComputers/retcomm-toolchains";
/* Default floor for Windows zlib-in-pack; override with RETCOMM_TOOLCHAIN_MIN_VERSION. */
static const char* k_tc_min_version_default = "1.0.3";

static const char* toolchain_zip_asset_name(void) {
#if defined(_WIN32)
    return "cmake-clang-v1-windows-x64.zip";
#elif defined(__APPLE__)
    return "cmake-clang-v1-macos-universal.zip";
#else
    return "cmake-clang-v1-linux-x64.zip";
#endif
}

static const char* toolchain_min_version(void) {
    const char* env = getenv("RETCOMM_TOOLCHAIN_MIN_VERSION");
    if (env && env[0])
        return env;
#if defined(_WIN32)
    /* Windows packs from 1.0.3 ship static zlib for find_package(ZLIB). */
    return k_tc_min_version_default;
#else
    (void)k_tc_min_version_default;
    return "";
#endif
}

/* Parse leading dotted integers from a version / tag (optional leading 'v'). */
static int version_cmp(const char* a, const char* b) {
    const char* pa = a ? a : "";
    const char* pb = b ? b : "";
    if ((pa[0] == 'v' || pa[0] == 'V') && pa[1] >= '0' && pa[1] <= '9')
        ++pa;
    if ((pb[0] == 'v' || pb[0] == 'V') && pb[1] >= '0' && pb[1] <= '9')
        ++pb;
    for (;;) {
        long va = 0, vb = 0;
        int ha = 0, hb = 0;
        while (*pa >= '0' && *pa <= '9') {
            va = va * 10 + (*pa - '0');
            ++pa;
            ha = 1;
        }
        while (*pb >= '0' && *pb <= '9') {
            vb = vb * 10 + (*pb - '0');
            ++pb;
            hb = 1;
        }
        if (!ha && !hb)
            return 0;
        if (va != vb)
            return va < vb ? -1 : 1;
        if (*pa == '.')
            ++pa;
        if (*pb == '.')
            ++pb;
        if (!ha || !hb)
            return ha ? 1 : (hb ? -1 : 0);
    }
}

static int read_pack_version(const char* pack_root, char* out, size_t cap) {
    char meta[1400], buf[4096];
    FILE* f;
    const char* p;
    size_t n;
    if (!pack_root || !pack_root[0] || !out || cap < 2)
        return 0;
    out[0] = '\0';
    if (!join_path(meta, sizeof(meta), pack_root, "retcomm-toolchain.json"))
        return 0;
    f = fopen(meta, "rb");
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strstr(buf, "\"version\"");
    if (!p)
        return 0;
    p = strchr(p + 9, '"');
    if (!p)
        return 0;
    ++p;
    {
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < cap) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return i > 0;
    }
}

static int pack_meets_min_version(const char* pack_root) {
    char ver[64];
    const char* need = toolchain_min_version();
    if (!need || !need[0])
        return 1;
    if (!read_pack_version(pack_root, ver, sizeof(ver)))
        return 0;
    return version_cmp(ver, need) >= 0;
}

static int mkdir_p(const char* path) {
    char tmp[1400];
    size_t n;
    if (!path || !path[0])
        return 0;
    n = strlen(path);
    if (n >= sizeof(tmp))
        return 0;
    memcpy(tmp, path, n + 1);
#if defined(_WIN32)
    {
        char* p = tmp;
        if (n >= 2 && tmp[1] == ':')
            p = tmp + 2;
        while (*p == '\\' || *p == '/')
            ++p;
        for (; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                char save = *p;
                *p = '\0';
                if (tmp[0])
                    CreateDirectoryA(tmp, NULL);
                *p = save;
            }
        }
        return CreateDirectoryA(tmp, NULL) ||
               GetLastError() == ERROR_ALREADY_EXISTS || path_is_dir(tmp);
    }
#else
    {
        char* p = tmp;
        if (*p == '/')
            ++p;
        for (; *p; ++p) {
            if (*p == '/') {
                *p = '\0';
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST && !path_is_dir(tmp))
                    return 0;
                *p = '/';
            }
        }
        return mkdir(tmp, 0755) == 0 || errno == EEXIST || path_is_dir(tmp);
    }
#endif
}

static int rmtree_path(const char* path) {
    char cmd[1600];
    if (!path || !path[0] || !path_is_dir(path))
        return 1;
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", path);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
#endif
    return system(cmd) == 0 || !path_is_dir(path);
}

#if defined(_WIN32)
static int run_cmdline_wait(const char* cmdline, DWORD* out_code) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char mutable_cmd[8192];
    DWORD code = 1;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (out_code)
        *out_code = code;
    return 1;
}
#endif

static int shared_toolchain_latest_dir(char* out, size_t cap) {
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !local[0])
        return 0;
    return join_path(out, cap, local, "retcomm/toolchains/cmake-clang-v1/latest");
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    char base[1100];
    if (xdg && xdg[0]) {
        if (!join_path(base, sizeof(base), xdg,
                       "retcomm/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else if (home && home[0]) {
        if (!join_path(base, sizeof(base), home,
                       ".local/share/retcomm/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else {
        return 0;
    }
    snprintf(out, cap, "%s", base);
    return 1;
#endif
}

static int pack_root_has_cmake(const char* root) {
    char bin[1400], cmake[1400];
    if (!root || !root[0])
        return 0;
    if (!join_path(bin, sizeof(bin), root, "bin"))
        return 0;
#if defined(_WIN32)
    return join_path(cmake, sizeof(cmake), bin, "cmake.exe") && path_is_file(cmake);
#else
    return join_path(cmake, sizeof(cmake), bin, "cmake") && path_is_file(cmake);
#endif
}

static int write_project_toolchain_stamp(const char* bin_dir) {
    char tc_dir[1200], stamp[1200];
    FILE* f;
    if (!g_project_root[0] || !bin_dir || !bin_dir[0])
        return 0;
    if (!join_path(tc_dir, sizeof(tc_dir), g_project_root, "toolchain"))
        return 0;
    mkdir_p(tc_dir);
    if (!join_path(stamp, sizeof(stamp), tc_dir, ".psxrecomp-bin"))
        return 0;
    f = fopen(stamp, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", bin_dir);
    fclose(f);
    return 1;
}

static int activate_installed_pack_root(const char* pack_root) {
    char bin[1400];
    if (!pack_root_has_cmake(pack_root))
        return 0;
    if (!join_path(bin, sizeof(bin), pack_root, "bin"))
        return 0;
    snprintf(g_cli_toolchain_bin, sizeof(g_cli_toolchain_bin), "%s", bin);
#if defined(_WIN32)
    _putenv_s("RETCOMM_TOOLCHAIN_DIR", pack_root);
    _putenv_s("PSXRECOMP_TOOLCHAIN_DIR", pack_root);
#else
    setenv("RETCOMM_TOOLCHAIN_DIR", pack_root, 1);
    setenv("PSXRECOMP_TOOLCHAIN_DIR", pack_root, 1);
#endif
    write_project_toolchain_stamp(bin);
    activate_toolchain_path();
    return find_cmake(g_cmake, sizeof(g_cmake)) ? 1 : 0;
}

#if defined(_WIN32)
/* Point a real LocalAppData path at a Store-Python LocalCache pack (no copy). */
static int junction_dir(const char* link_path, const char* target_path) {
    char cmd[3200];
    DWORD code = 1;
    char parent[1400];
    if (!link_path || !target_path || !path_is_dir(target_path))
        return 0;
    if (path_is_dir(link_path) || path_is_file(link_path)) {
        /* Replace broken/empty dir; keep a usable pack. */
        if (pack_root_has_cmake(link_path))
            return 1;
        rmtree_path(link_path);
    }
    if (!dirname_copy(parent, sizeof(parent), link_path))
        return 0;
    mkdir_p(parent);
    snprintf(cmd, sizeof(cmd), "cmd.exe /c mklink /J \"%s\" \"%s\"", link_path,
             target_path);
    if (!run_cmdline_wait(cmd, &code))
        return 0;
    return code == 0 && path_is_dir(link_path);
}

static int find_store_localcache_pack_root(char* out, size_t cap) {
    const char* local = getenv("LOCALAPPDATA");
    char packages[1100], pattern[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!local || !local[0])
        return 0;
    if (!join_path(packages, sizeof(packages), local, "Packages"))
        return 0;
    snprintf(pattern, sizeof(pattern),
             "%s\\PythonSoftwareFoundation.Python.*", packages);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char* suffixes[] = {
            "LocalCache\\Local\\retcomm\\toolchains\\cmake-clang-v1\\latest",
            "LocalCache\\Local\\retcomm\\toolchains\\cmake-clang-v1\\offline",
            "LocalCache\\Local\\psxrecomp\\toolchains\\cmake-clang-v1\\latest",
            "LocalCache\\Local\\psxrecomp\\toolchains\\cmake-clang-v1\\offline",
            NULL};
        int i;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        for (i = 0; suffixes[i]; ++i) {
            char cand[1400], nested[1400];
            snprintf(cand, sizeof(cand), "%s\\%s\\%s", packages, fd.cFileName,
                     suffixes[i]);
            if (pack_root_has_cmake(cand)) {
                snprintf(out, cap, "%s", cand);
                FindClose(h);
                return 1;
            }
            /* Nested single child with bin/cmake.exe */
            if (path_is_dir(cand)) {
                WIN32_FIND_DATAA kd;
                char kp[1400];
                HANDLE hk;
                snprintf(kp, sizeof(kp), "%s\\*", cand);
                hk = FindFirstFileA(kp, &kd);
                if (hk == INVALID_HANDLE_VALUE)
                    continue;
                do {
                    if (!(kd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        continue;
                    if (kd.cFileName[0] == '.')
                        continue;
                    if (!join_path(nested, sizeof(nested), cand, kd.cFileName))
                        continue;
                    if (pack_root_has_cmake(nested)) {
                        snprintf(out, cap, "%s", nested);
                        FindClose(hk);
                        FindClose(h);
                        return 1;
                    }
                } while (FindNextFileA(hk, &kd));
                FindClose(hk);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* allow_copy=0: point this process at LocalCache (cheap).
 * allow_copy=1: also junction/robocopy into real %LOCALAPPDATA% (ensure). */
static int harvest_store_python_toolchain(int allow_copy) {
    char cache_root[1400], real_latest[1400], proj_tc[1200];
    if (!find_store_localcache_pack_root(cache_root, sizeof(cache_root)))
        return 0;
    if (!allow_copy)
        return activate_installed_pack_root(cache_root);
    if (!shared_toolchain_latest_dir(real_latest, sizeof(real_latest)))
        return activate_installed_pack_root(cache_root);
    if (!junction_dir(real_latest, cache_root) &&
        !pack_root_has_cmake(real_latest)) {
        /* Junction failed — robocopy into the real tree. */
        char parent[1400], cmd[3200];
        DWORD code = 1;
        if (!dirname_copy(parent, sizeof(parent), real_latest))
            return activate_installed_pack_root(cache_root);
        mkdir_p(parent);
        rmtree_path(real_latest);
        mkdir_p(real_latest);
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                 "/ns /np",
                 cache_root, real_latest);
        if (!run_cmdline_wait(cmd, &code) || code > 7 ||
            !pack_root_has_cmake(real_latest))
            return activate_installed_pack_root(cache_root);
    }
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc))
            junction_dir(proj_tc, pack_root_has_cmake(real_latest)
                                      ? real_latest
                                      : cache_root);
    }
    return activate_installed_pack_root(
        pack_root_has_cmake(real_latest) ? real_latest : cache_root);
}

static int host_download_url_to_file(const char* url, const char* dest,
                                     char* err_msg, size_t err_cap) {
    char cmd[4096];
    DWORD code = 1;
    char parent[1400];
    if (!dirname_copy(parent, sizeof(parent), dest)) {
        snprintf(err_msg, err_cap, "Bad download destination.");
        return 0;
    }
    mkdir_p(parent);
    DeleteFileA(dest);
    /* Windows 10+ ships curl.exe. -L follows GitHub release redirects. */
    snprintf(cmd, sizeof(cmd),
             "curl.exe -fsSL --retry 3 --retry-delay 2 -o \"%s\" \"%s\"", dest,
             url);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        snprintf(err_msg, err_cap,
                 "Toolchain download failed (curl exit %lu). Check network / "
                 "curl.exe.",
                 (unsigned long)code);
        return 0;
    }
    return path_is_file(dest);
}

static int host_extract_zip(const char* zip_path, const char* dest_dir,
                            char* err_msg, size_t err_cap) {
    char cmd[3200];
    DWORD code = 1;
    char parent[1400];
    if (!path_is_file(zip_path)) {
        snprintf(err_msg, err_cap, "Toolchain zip not found: %s", zip_path);
        return 0;
    }
    if (!dirname_copy(parent, sizeof(parent), dest_dir)) {
        snprintf(err_msg, err_cap, "Bad extract destination.");
        return 0;
    }
    mkdir_p(parent);
    rmtree_path(dest_dir);
    mkdir_p(dest_dir);
    /* tar.exe on Windows 10+ extracts .zip */
    snprintf(cmd, sizeof(cmd), "tar.exe -xf \"%s\" -C \"%s\"", zip_path,
             dest_dir);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        snprintf(err_msg, err_cap, "Failed to extract toolchain zip (tar exit %lu).",
                 (unsigned long)code);
        return 0;
    }
    if (pack_root_has_cmake(dest_dir))
        return 1;
    /* Single nested directory layout — resolve_toolchain_bin_under handles it. */
    {
        WIN32_FIND_DATAA fd;
        char pattern[1400], child[1400];
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", dest_dir);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (fd.cFileName[0] == '.')
                    continue;
                if (!join_path(child, sizeof(child), dest_dir, fd.cFileName))
                    continue;
                if (pack_root_has_cmake(child)) {
                    FindClose(h);
                    return 1;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.exe.");
    return 0;
}
#else
static int host_download_url_to_file(const char* url, const char* dest,
                                     char* err_msg, size_t err_cap) {
    char cmd[4096];
    char parent[1400];
    if (!dirname_copy(parent, sizeof(parent), dest)) {
        snprintf(err_msg, err_cap, "Bad download destination.");
        return 0;
    }
    mkdir_p(parent);
    unlink(dest);
    snprintf(cmd, sizeof(cmd),
             "curl -fsSL --retry 3 --retry-delay 2 -o \"%s\" \"%s\"", dest, url);
    if (system(cmd) != 0) {
        snprintf(err_msg, err_cap, "Toolchain download failed (curl).");
        return 0;
    }
    return path_is_file(dest);
}

static int host_extract_zip(const char* zip_path, const char* dest_dir,
                            char* err_msg, size_t err_cap) {
    char cmd[3200];
    char parent[1400];
    if (!path_is_file(zip_path)) {
        snprintf(err_msg, err_cap, "Toolchain zip not found: %s", zip_path);
        return 0;
    }
    if (!dirname_copy(parent, sizeof(parent), dest_dir)) {
        snprintf(err_msg, err_cap, "Bad extract destination.");
        return 0;
    }
    mkdir_p(parent);
    rmtree_path(dest_dir);
    mkdir_p(dest_dir);
    snprintf(cmd, sizeof(cmd), "unzip -q \"%s\" -d \"%s\"", zip_path, dest_dir);
    if (system(cmd) != 0) {
        snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\"", zip_path,
                 dest_dir);
        if (system(cmd) != 0) {
            snprintf(err_msg, err_cap, "Failed to extract toolchain zip.");
            return 0;
        }
    }
    if (pack_root_has_cmake(dest_dir))
        return 1;
    /* Nested child with bin/ is fine — resolve_toolchain_bin_under handles it. */
    {
        DIR* dir = opendir(dest_dir);
        struct dirent* ent;
        if (!dir) {
            snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.");
            return 0;
        }
        while ((ent = readdir(dir)) != NULL) {
            char child[1400];
            if (ent->d_name[0] == '.')
                continue;
            if (!join_path(child, sizeof(child), dest_dir, ent->d_name))
                continue;
            if (path_is_dir(child) && pack_root_has_cmake(child)) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }
    snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.");
    return 0;
}
#endif

static int link_or_stamp_project_toolchain(const char* pack_root) {
    char proj_tc[1200], bin[1400];
    if (!pack_root_has_cmake(pack_root))
        return 0;
    if (!join_path(bin, sizeof(bin), pack_root, "bin"))
        return 0;
#if defined(_WIN32)
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc))
            junction_dir(proj_tc, pack_root);
    }
#else
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc) && !path_is_dir(proj_tc)) {
            char cmd[2800];
            snprintf(cmd, sizeof(cmd), "ln -s \"%s\" \"%s\"", pack_root,
                     proj_tc);
            (void)system(cmd);
        }
    }
#endif
    write_project_toolchain_stamp(bin);
    return 1;
}

static int host_install_toolchain_from_zip(
    const char* zip_path, RecompLauncherCPrepareProgressFn on_progress,
    void* progress_ctx, char* err_msg, size_t err_cap) {
    char dest[1400];
    if (!shared_toolchain_latest_dir(dest, sizeof(dest))) {
        snprintf(err_msg, err_cap, "Cannot resolve shared toolchain directory.");
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.45f, "Extracting portable cmake/clang…");
    if (!host_extract_zip(zip_path, dest, err_msg, err_cap))
        return 0;
    if (on_progress)
        on_progress(progress_ctx, 0.85f, "Activating toolchain…");
    link_or_stamp_project_toolchain(dest);
    if (activate_installed_pack_root(dest))
        return 1;
    /* Nested layout under dest/ */
    if (resolve_toolchain_bin_under(dest, g_cli_toolchain_bin,
                                    sizeof(g_cli_toolchain_bin))) {
        char pack[1400];
        if (dirname_copy(pack, sizeof(pack), g_cli_toolchain_bin))
            return activate_installed_pack_root(pack);
    }
    snprintf(err_msg, err_cap, "Extracted toolchain but cmake was not found.");
    return 0;
}

static int host_download_and_install_toolchain(
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx,
    char* err_msg, size_t err_cap) {
    char url[512], zip_path[1400], tmp_dir[1100];
    const char* asset = toolchain_zip_asset_name();
    snprintf(url, sizeof(url),
             "https://github.com/%s/releases/latest/download/%s", k_tc_repo,
             asset);
#if defined(_WIN32)
    {
        char tmp[512];
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        if (n == 0 || n >= sizeof(tmp)) {
            snprintf(err_msg, err_cap, "GetTempPath failed.");
            return 0;
        }
        snprintf(tmp_dir, sizeof(tmp_dir), "%spsxrecomp-tc-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/psxrecomp-tc-%d", (int)getpid());
#endif
    mkdir_p(tmp_dir);
    if (!join_path(zip_path, sizeof(zip_path), tmp_dir, asset)) {
        snprintf(err_msg, err_cap, "Temp path too long.");
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.1f, "Downloading portable cmake/clang…");
    if (!host_download_url_to_file(url, zip_path, err_msg, err_cap)) {
        rmtree_path(tmp_dir);
        return 0;
    }
    if (!host_install_toolchain_from_zip(zip_path, on_progress, progress_ctx,
                                         err_msg, err_cap)) {
        rmtree_path(tmp_dir);
        return 0;
    }
    rmtree_path(tmp_dir);
    return 1;
}

/* Promote a usable legacy psxrecomp cache into the shared retcomm tree. */
static int migrate_legacy_psxrecomp_toolchain(void) {
    char legacy[1400], dest[1400], parent[1400];
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !local[0])
        return 0;
    if (!join_path(legacy, sizeof(legacy), local,
                   "psxrecomp/toolchains/cmake-clang-v1/latest"))
        return 0;
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0]) {
        if (!join_path(legacy, sizeof(legacy), xdg,
                       "psxrecomp/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else if (home && home[0]) {
        if (!join_path(legacy, sizeof(legacy), home,
                       ".local/share/psxrecomp/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else {
        return 0;
    }
#endif
    if (!pack_root_has_cmake(legacy))
        return 0;
    if (!shared_toolchain_latest_dir(dest, sizeof(dest)))
        return 0;
    if (pack_root_has_cmake(dest))
        return 1;
    if (!dirname_copy(parent, sizeof(parent), dest))
        return 0;
    mkdir_p(parent);
#if defined(_WIN32)
    if (junction_dir(dest, legacy))
        return 1;
    {
        char cmd[3200];
        DWORD code = 1;
        mkdir_p(dest);
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                 "/ns /np",
                 legacy, dest);
        if (run_cmdline_wait(cmd, &code) && code <= 7 && pack_root_has_cmake(dest))
            return 1;
    }
#else
    {
        char cmd[2800];
        snprintf(cmd, sizeof(cmd), "ln -sfn \"%s\" \"%s\"", legacy, dest);
        if (system(cmd) == 0 && pack_root_has_cmake(dest))
            return 1;
        snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", legacy, dest);
        if (system(cmd) == 0 && pack_root_has_cmake(dest))
            return 1;
    }
#endif
    return 0;
}

static int pack_root_from_bin(const char* bin, char* out, size_t cap) {
    return bin && bin[0] && dirname_copy(out, cap, bin);
}

static int active_toolchain_meets_min(void) {
    char pack[1400];
    if (!g_toolchain_bin[0] && !resolve_toolchain_bin(g_toolchain_bin,
                                                      sizeof(g_toolchain_bin)))
        return 0;
    if (!pack_root_from_bin(g_toolchain_bin, pack, sizeof(pack)))
        return 0;
    return pack_meets_min_version(pack);
}

static int host_toolchain_is_ready(void) {
    if (!g_project_root[0])
        return 0;
    migrate_legacy_psxrecomp_toolchain();
    activate_toolchain_path();
    if (find_cmake(g_cmake, sizeof(g_cmake)) && active_toolchain_meets_min())
        return 1;
#if defined(_WIN32)
    /* Reuse a Store-Python LocalCache install without copying. */
    if (harvest_store_python_toolchain(0)) {
        char pack[1400];
        if (pack_root_from_bin(g_cli_toolchain_bin[0] ? g_cli_toolchain_bin
                                                     : g_toolchain_bin,
                               pack, sizeof(pack)) &&
            pack_meets_min_version(pack))
            return 1;
    }
#endif
    return 0;
}

/* Download or offline-install cmake-clang-v1 (wizard page 0 / rebuild fallback).
 * Prefer host-native curl/tar so Microsoft Store Python cannot redirect the
 * unpack into Packages\\...\\LocalCache. Installs into the shared RetComM
 * cache: %LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/… */
static int host_ensure_toolchain_with_progress(
    int download, const char* zip_path, char* err_msg, size_t err_cap,
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx) {
    if (!g_project_root[0]) {
        snprintf(err_msg, err_cap, "Project root is not available.");
        return 0;
    }
    migrate_legacy_psxrecomp_toolchain();
    activate_toolchain_path();
    if (find_cmake(g_cmake, sizeof(g_cmake)) && active_toolchain_meets_min())
        return 1;

#if defined(_WIN32)
    if (on_progress)
        on_progress(progress_ctx, 0.02f,
                    "Checking for an existing portable toolchain…");
    if (harvest_store_python_toolchain(1)) {
        char pack[1400];
        if (pack_root_from_bin(g_cli_toolchain_bin[0] ? g_cli_toolchain_bin
                                                     : g_toolchain_bin,
                               pack, sizeof(pack)) &&
            pack_meets_min_version(pack))
            return 1;
    }
#endif

    if (zip_path && zip_path[0]) {
        if (on_progress)
            on_progress(progress_ctx, 0.05f, "Installing toolchain from zip…");
        if (host_install_toolchain_from_zip(zip_path, on_progress, progress_ctx,
                                            err_msg, err_cap)) {
            if (active_toolchain_meets_min())
                return 1;
            snprintf(err_msg, err_cap,
                     "Toolchain zip does not meet min_version %s "
                     "(set RETCOMM_TOOLCHAIN_MIN_VERSION or use a newer pack).",
                     toolchain_min_version());
            return 0;
        }
        return 0;
    }

    if (download) {
        if (on_progress && find_cmake(g_cmake, sizeof(g_cmake)) &&
            !active_toolchain_meets_min())
            on_progress(progress_ctx, 0.05f,
                        "Updating portable toolchain to required version…");
        if (host_download_and_install_toolchain(on_progress, progress_ctx,
                                                err_msg, err_cap)) {
            if (active_toolchain_meets_min())
                return 1;
            snprintf(err_msg, err_cap,
                     "Downloaded toolchain does not meet min_version %s.",
                     toolchain_min_version());
            return 0;
        }
        return 0;
    }

    snprintf(err_msg, err_cap,
             "No portable toolchain found. Enable automatic download, pick a "
             "cmake-clang-v1 zip, or set RETCOMM_TOOLCHAIN_DIR.");
    return 0;
}

/* Rebuild-time fallback if the wizard step was skipped / cache pruned. */
static int host_ensure_toolchain(RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx, char* err_msg,
                                 size_t err_cap) {
    return host_ensure_toolchain_with_progress(1, NULL, err_msg, err_cap,
                                               on_progress, progress_ctx);
}

static int host_prepare_generate(const char* source_path, char* out_path,
                                 size_t out_cap, char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!g_ready) {
        snprintf(err_msg, err_cap, "Local codegen tools are not available.");
        return 0;
    }
    if (!source_path || !source_path[0]) {
        snprintf(err_msg, err_cap, "No disc selected.");
        return 0;
    }
    activate_toolchain_path();
    if (on_progress)
        on_progress(progress_ctx, 0.02f, "Starting psxrecomp generate…");

    /* Sync wizard disc/BIOS sidecars into project root + build/ before resolve. */
    {
        char bios_line[1100];
        host_persist_setup(NULL, source_path, NULL); /* disc only */
        if (resolve_bios_arg(bios_line, sizeof(bios_line)))
            host_persist_setup(NULL, source_path, bios_line);
    }

    char bios_path[1100];
    const int have_bios = resolve_bios_arg(bios_path, sizeof(bios_path));

#if defined(_WIN32)
    char cmdline[4096];
    if (have_bios) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --config \"%s\" "
                 "--disc \"%s\" --bios \"%s\" --json-progress",
                 g_python, g_cli_path, g_project_root, g_game_toml, source_path,
                 bios_path);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --config \"%s\" "
                 "--disc \"%s\" --json-progress",
                 g_python, g_cli_path, g_project_root, g_game_toml, source_path);
    }
    if (!run_cli_win(cmdline, on_progress, progress_ctx, err_msg, err_cap,
                     "psxrecomp generate"))
        return 0;
#else
    char* argv[16];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "generate";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--config";
    argv[argc++] = g_game_toml;
    argv[argc++] = "--disc";
    argv[argc++] = (char*)source_path;
    if (have_bios) {
        argv[argc++] = "--bios";
        argv[argc++] = bios_path;
    }
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;
    if (!run_cli_posix(argv, on_progress, progress_ctx, err_msg, err_cap,
                       "psxrecomp generate"))
        return 0;
#endif

    snprintf(out_path, out_cap, "%s", source_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Generate complete");
    return 1;
}

#if defined(_WIN32)
static void bat_write_set(FILE* f, const char* name, const char* value) {
    fprintf(f, "set \"%s=", name);
    for (const char* p = value; *p; ++p) {
        if (*p == '%')
            fputc('%', f);
        fputc(*p, f);
    }
    fprintf(f, "\"\r\n");
}

static int write_windows_deferred_rebuild_helper(char* err_msg, size_t err_cap) {
    if (!join_path(g_helper_path, sizeof(g_helper_path), g_build_dir,
                   "recomp_deferred_rebuild.cmd")) {
        snprintf(err_msg, err_cap, "Failed to form helper path.");
        return 0;
    }
    /* Setup zips omit build-release/; create it before writing the .cmd. */
    if (!mkdir_p(g_build_dir)) {
        snprintf(err_msg, err_cap, "Failed to create build dir: %s",
                 g_build_dir);
        return 0;
    }
    FILE* f = fopen(g_helper_path, "wb");
    if (!f) {
        snprintf(err_msg, err_cap, "Failed to write rebuild helper: %s",
                 g_helper_path);
        return 0;
    }
    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "%lu",
             (unsigned long)GetCurrentProcessId());
    fprintf(f, "@echo off\r\n");
    fprintf(f, "setlocal EnableExtensions\r\n");
    fprintf(f, "title %s - rebuilding\r\n", g_display);
    bat_write_set(f, "PARENT_PID", pid_buf);
    bat_write_set(f, "PYTHON", g_python);
    bat_write_set(f, "CLI", g_cli_path);
    bat_write_set(f, "ROOT", g_project_root);
    bat_write_set(f, "CONFIG", g_game_toml);
    bat_write_set(f, "BUILD_DIR", g_build_dir);
    bat_write_set(f, "TARGET", g_cmake_target);
    bat_write_set(f, "EXE_BASE", g_exe_basename);
    bat_write_set(f, "EXE", g_exe_path);
    bat_write_set(f, "DISPLAY", g_display);
    if (g_toolchain_bin[0])
        bat_write_set(f, "TC_BIN", g_toolchain_bin);
    fprintf(f,
            "echo Waiting for %%DISPLAY%% to exit...\r\n"
            ":waitloop\r\n"
            "tasklist /FI \"PID eq %%PARENT_PID%%\" 2>NUL | "
            "findstr /I \"%%PARENT_PID%%\" >NUL\r\n"
            "if not errorlevel 1 (\r\n"
            "  ping -n 2 127.0.0.1 >NUL\r\n"
            "  goto waitloop\r\n"
            ")\r\n"
            "echo Ensuring toolchain...\r\n"
            "cd /d \"%%ROOT%%\"\r\n"
            "if defined TC_BIN set \"PATH=%%TC_BIN%%;%%PATH%%\"\r\n"
            "\"%%PYTHON%%\" \"%%CLI%%\" ensure-toolchain --project-root \"%%ROOT%%\"\r\n"
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Toolchain missing. Download cmake-clang-v1 or set\r\n"
            "  echo RETCOMM_TOOLCHAIN_DIR / pass --toolchain-zip on rebuild.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n"
            "echo Building...\r\n"
            "\"%%PYTHON%%\" \"%%CLI%%\" rebuild --project-root \"%%ROOT%%\" "
            "--config \"%%CONFIG%%\" --build-dir \"%%BUILD_DIR%%\" "
            "--target \"%%TARGET%%\" --exe-basename \"%%EXE_BASE%%\" "
            "--no-pgo --prune-after build-intermediates\r\n"
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Build failed. Fix the errors above, then rebuild manually.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n"
            "echo Starting %%DISPLAY%%...\r\n"
            "start \"\" /D \"%%ROOT%%\" \"%%EXE%%\" --launcher\r\n"
            "endlocal\r\n");
    fclose(f);
    return 1;
}
#endif

static int host_rebuild_game(const char* disc_path, char* out_exe_path,
                             size_t out_cap, char* err_msg, size_t err_cap,
                             RecompLauncherCPrepareProgressFn on_progress,
                             void* progress_ctx) {
    g_relaunch_is_helper = 0;
    if (!g_ready || !g_build_dir[0]) {
        snprintf(err_msg, err_cap, "CMake build environment is not available.");
        return 0;
    }

    if (!host_ensure_toolchain(on_progress, progress_ctx, err_msg, err_cap))
        return 0;

#if defined(_WIN32)
    (void)disc_path;
    activate_toolchain_path();
    if (on_progress)
        on_progress(progress_ctx, 0.4f,
                    "Scheduling Windows rebuild after exit…");
    if (!write_windows_deferred_rebuild_helper(err_msg, err_cap))
        return 0;
    g_relaunch_is_helper = 1;
    snprintf(out_exe_path, out_cap, "%s", g_helper_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f,
                    "Exiting so Windows can rebuild safely…");
    return 1;
#else
    if (on_progress)
        on_progress(progress_ctx, 0.05f, "Starting rebuild (cmake)…");

    activate_toolchain_path();

    char disc_arg_storage[1100];
    char* argv[36];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "rebuild";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--config";
    argv[argc++] = g_game_toml;
    argv[argc++] = "--build-dir";
    argv[argc++] = g_build_dir;
    argv[argc++] = "--target";
    argv[argc++] = g_cmake_target;
    argv[argc++] = "--exe-basename";
    argv[argc++] = g_exe_basename;
    if (disc_path && disc_path[0]) {
        snprintf(disc_arg_storage, sizeof(disc_arg_storage), "%s", disc_path);
        argv[argc++] = "--disc";
        argv[argc++] = disc_arg_storage;
    }
    argv[argc++] = "--no-pgo";
    argv[argc++] = "--prune-after";
    argv[argc++] = "build-intermediates";
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;

    if (!run_cli_posix(argv, on_progress, progress_ctx, err_msg, err_cap,
                       "psxrecomp rebuild"))
        return 0;
    if (!path_is_file(g_exe_path)) {
        snprintf(err_msg, err_cap, "Build succeeded but binary missing: %s",
                 g_exe_path);
        return 0;
    }
    snprintf(out_exe_path, out_cap, "%s", g_exe_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Build complete");
    return 1;
#endif
}

void psxrecomp_codegen_host_relaunch_or_exit(const char* disc_path) {
    char exe[512];
    const char* near_exe;
    if (!recomp_launcher_relaunch_exe(exe, sizeof(exe)) || !exe[0]) {
        fprintf(stderr, "psxrecomp-codegen: relaunch requested but no path\n");
        exit(1);
    }
    /* Prefer the final game binary (build/<exe>) over a Windows helper bat. */
    near_exe = g_exe_path[0] ? g_exe_path : exe;
    persist_relaunch_sidecars(near_exe, disc_path);

#if defined(_WIN32)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[1536];
        DWORD flags = 0;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        if (g_relaunch_is_helper) {
            fprintf(stderr,
                    "psxrecomp-codegen: starting deferred rebuild helper\n");
            snprintf(cmd, sizeof(cmd), "cmd.exe /C \"%s\"", exe);
            flags = CREATE_NEW_CONSOLE;
        } else {
            fprintf(stderr, "psxrecomp-codegen: relaunching %s\n", exe);
            snprintf(cmd, sizeof(cmd), "\"%s\" --launcher", exe);
        }
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL,
                            g_project_root, &si, &pi)) {
            fprintf(stderr, "psxrecomp-codegen: CreateProcess failed\n");
            exit(1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    {
        if (g_project_root[0] && chdir(g_project_root) != 0) {
            fprintf(stderr, "psxrecomp-codegen: chdir(%s) failed: %s\n",
                    g_project_root, strerror(errno));
        }
        fprintf(stderr, "psxrecomp-codegen: relaunching %s\n", exe);
        char* args[] = {exe, "--launcher", NULL};
        execv(exe, args);
        perror("psxrecomp-codegen: execv failed");
        exit(1);
    }
#endif
}

void psxrecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                  const PsxrecompCodegenHostConfig* cfg) {
    if (!gi || !cfg || !cfg->cmake_target || !cfg->exe_basename)
        return;

    g_cfg = cfg;
    g_ready = 0;
    g_relaunch_is_helper = 0;
    g_project_root[0] = '\0';
    g_cli_path[0] = '\0';
    g_game_toml[0] = '\0';
    g_python[0] = '\0';
    g_cmake[0] = '\0';
    g_build_dir[0] = '\0';
    g_exe_path[0] = '\0';
    g_helper_path[0] = '\0';
    g_toolchain_bin[0] = '\0';
    g_cli_toolchain_bin[0] = '\0';

    snprintf(g_display, sizeof(g_display), "%s",
             cfg_or(cfg->display_name, "Game"));
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%s", cfg->cmake_target);
    snprintf(g_exe_basename, sizeof(g_exe_basename), "%s", cfg->exe_basename);

    const char* force_env =
        cfg_or(cfg->force_setup_env, "PSXRECOMP_FORCE_SETUP");
    const char* force = getenv(force_env);
    const int force_setup = force && force[0] && force[0] != '0';

    if (!discover_project_root(g_project_root, sizeof(g_project_root))) {
        /* Still force the wizard when generated/ is missing — discover may
         * fail if the process cwd is unrelated to the project tree. */
        if (force_setup) {
            gi->needs_setup = 1;
            gi->prepare_required_before_continue = 1;
        }
        return;
    }
    if (!resolve_cli_path(g_project_root, g_cli_path, sizeof(g_cli_path))) {
        if (psxrecomp_codegen_host_sources_missing(cfg) || force_setup) {
            gi->needs_setup = 1;
            gi->prepare_required_before_continue = 1;
        }
        return;
    }
    if (!join_path(g_game_toml, sizeof(g_game_toml), g_project_root,
                   cfg_or(cfg->game_toml_relpath, "game.toml")))
        return;
    if (!path_is_file(g_game_toml))
        return;
    if (!find_python(g_python, sizeof(g_python))) {
        if (psxrecomp_codegen_host_sources_missing(cfg) || force_setup) {
            gi->needs_setup = 1;
            gi->prepare_required_before_continue = 1;
        }
        return;
    }

    g_ready = 1;
    activate_toolchain_path();
    gi->prepare_with_progress = host_prepare_generate;
    gi->prepare_use_selected_rom = 1;
    /* Number prefix is applied in the setup UI (BIOS adds a step). */
    gi->prepare_section_title = "Generate BIOS + game C & rebuild";
    gi->prepare_busy_status = "Generating BIOS + game sources…";
    gi->prepare_success_status = "Sources ready — building…";

    /* Rebuild is offered whenever the build tree can be formed; the wizard
     * installs cmake-clang-v1 on page 0 before Generate & rebuild. */
    const int can_rebuild = resolve_build_paths();
    if (can_rebuild) {
        gi->prepare_disc_label = "Generate & rebuild…";
#if defined(_WIN32)
        gi->prepare_disc_note =
            cfg->prepare_note_windows
                ? cfg->prepare_note_windows
                : "Uses your disc with the local psxrecomp SDK to regenerate "
                  "generated/, then quits and rebuilds via a helper so the "
                  "running .exe is not locked.";
        gi->rebuild_busy_status = "Scheduling rebuild…";
        gi->rebuild_success_status =
            "Exiting for Windows rebuild — a console will finish the build…";
#else
        gi->prepare_disc_note =
            cfg->prepare_note
                ? cfg->prepare_note
                : "Uses your disc with the local psxrecomp SDK to regenerate "
                  "generated/, then runs cmake --build and restarts.";
        gi->rebuild_busy_status = "Building…";
        gi->rebuild_success_status = "Build complete — restarting…";
#endif
        gi->rebuild_with_progress = host_rebuild_game;
        gi->rebuild_after_prepare = 1;
        gi->relaunch_after_rebuild = 1;
        gi->setup_needs_toolchain = 1;
        gi->toolchain_is_ready = host_toolchain_is_ready;
        gi->ensure_toolchain_with_progress = host_ensure_toolchain_with_progress;
    } else {
        gi->prepare_disc_label = "Generate sources…";
        gi->prepare_disc_note =
            cfg->prepare_note_no_cmake
                ? cfg->prepare_note_no_cmake
                : "Regenerates generated/ with the local psxrecomp SDK. "
                  "Build dir could not be resolved — rebuild manually, then "
                  "relaunch.";
        gi->prepare_success_status =
            "Sources generated. Rebuild manually, then relaunch.";
    }
    gi->persist_setup = host_persist_setup;
    gi->persist_setup_ctx = NULL;

    if (psxrecomp_codegen_host_sources_missing(cfg) || force_setup) {
        gi->needs_setup = 1;
        gi->prepare_required_before_continue = 1;
    }
}
