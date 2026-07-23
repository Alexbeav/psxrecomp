#include "mod_runtime.h"

#include "mod_packages.h"
#include "psx_sha256.h"

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" uint8_t psx_read_byte(uint32_t addr);
extern "C" void psx_write_byte(uint32_t addr, uint8_t value);
extern "C" void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);

namespace PSXRecompV4 {
namespace {

struct RuntimeMods {
    ModPackageManager manager;
    ModResolution plan;
    std::string game_id;
    std::string error;
    std::string exe_sha256;
    std::string disc_sha256;
    std::filesystem::path disc_path;
    std::filesystem::path effective_disc_path;
    uint32_t entry_phys = 0;
    bool initialized = false;
    bool main_applied = false;
    bool disc_enabled = false;
    bool disc_guard_failed = false;
};

RuntimeMods& state() {
    static RuntimeMods value;
    return value;
}

const ModPackage* selected_package(const std::string& id) {
    return state().manager.selected_package(id);
}

std::string selected_value(const ModPackage& package, const ModOption& option) {
    const auto selection = state().manager.selections().find(package.id);
    if (selection != state().manager.selections().end()) {
        const auto value = selection->second.values.find(option.id);
        if (value != selection->second.values.end()) return value->second;
    }
    return option.default_value;
}

void set_error(const std::string& error) {
    state().error = error;
}

bool sha256_file(const std::filesystem::path& path, std::string& out,
                 std::string* error) {
    out.clear();
    if (path.empty()) return true;
    std::vector<std::filesystem::path> inputs{path};
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".cue") {
        std::ifstream cue(path);
        if (!cue) {
            if (error) *error = "cannot fingerprint image: " + path.string();
            return false;
        }
        inputs.clear();
        std::set<std::filesystem::path> seen;
        std::string line;
        while (std::getline(cue, line)) {
            size_t at = line.find_first_not_of(" \t");
            if (at == std::string::npos || line.size() - at < 4) continue;
            std::string keyword = line.substr(at, 4);
            std::transform(keyword.begin(), keyword.end(), keyword.begin(),
                [](unsigned char c) { return (char)std::toupper(c); });
            if (keyword != "FILE") continue;
            at += 4;
            at = line.find_first_not_of(" \t", at);
            if (at == std::string::npos) continue;
            std::string name;
            if (line[at] == '"') {
                const size_t end = line.find('"', at + 1);
                if (end == std::string::npos) continue;
                name = line.substr(at + 1, end - at - 1);
            } else {
                const size_t end = line.find_first_of(" \t", at);
                name = line.substr(at, end - at);
            }
            const std::filesystem::path input =
                (path.parent_path() / name).lexically_normal();
            if (seen.insert(input).second) inputs.push_back(input);
        }
        if (inputs.empty()) {
            if (error) *error = "CUE has no FILE entries: " + path.string();
            return false;
        }
    }
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    std::array<uint8_t, 1024 * 1024> buffer{};
    for (const std::filesystem::path& input : inputs) {
        std::ifstream file(input, std::ios::binary);
        if (!file) {
            if (error) *error = "cannot fingerprint image: " + input.string();
            return false;
        }
        while (file) {
            file.read((char*)buffer.data(), (std::streamsize)buffer.size());
            const std::streamsize got = file.gcount();
            if (got > 0) psx_sha256_update(&hash, buffer.data(), (size_t)got);
        }
        if (!file.eof()) {
            if (error) *error =
                "cannot finish fingerprinting image: " + input.string();
            return false;
        }
    }
    uint8_t digest[32];
    psx_sha256_final(&hash, digest);
    std::ostringstream text;
    for (uint8_t byte : digest)
        text << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    out = text.str();
    return true;
}

std::filesystem::path raw_image_path(const std::filesystem::path& path,
                                     std::string* error) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension != ".cue") return path;
    std::ifstream cue(path);
    if (!cue) {
        if (error) *error = "cannot open CUE for derived disc: " + path.string();
        return {};
    }
    std::string line;
    while (std::getline(cue, line)) {
        size_t at = line.find_first_not_of(" \t");
        if (at == std::string::npos || line.size() - at < 4) continue;
        std::string keyword = line.substr(at, 4);
        std::transform(keyword.begin(), keyword.end(), keyword.begin(),
            [](unsigned char c) { return (char)std::toupper(c); });
        if (keyword != "FILE") continue;
        at = line.find_first_not_of(" \t", at + 4);
        if (at == std::string::npos) continue;
        std::string name;
        if (line[at] == '"') {
            const size_t end = line.find('"', at + 1);
            if (end == std::string::npos) continue;
            name = line.substr(at + 1, end - at - 1);
        } else {
            const size_t end = line.find_first_of(" \t", at);
            name = line.substr(at, end - at);
        }
        return (path.parent_path() / name).lexically_normal();
    }
    if (error) *error = "CUE has no source file for derived disc: " + path.string();
    return {};
}

#if defined(_WIN32)
std::wstring quote_windows_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(c);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
#endif

bool run_xdelta_decode(const std::filesystem::path& executable,
                       const std::filesystem::path& source,
                       const std::filesystem::path& patch,
                       const std::filesystem::path& output,
                       std::string* error) {
#if defined(_WIN32)
    std::wstring command =
        quote_windows_argument(executable.wstring()) + L" -f -n -d -s " +
        quote_windows_argument(source.wstring()) + L" " +
        quote_windows_argument(patch.wstring()) + L" " +
        quote_windows_argument(output.wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.wstring().c_str(), mutable_command.data(),
                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        if (error) *error = "cannot start trusted xdelta3 decoder (Windows error " +
            std::to_string((unsigned long)GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        if (error) *error =
            "trusted xdelta3 decoder failed with exit code " + std::to_string(exit_code);
        return false;
    }
    return true;
#else
    const pid_t child = fork();
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), "-f", "-n", "-d", "-s",
              source.c_str(), patch.c_str(), output.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (child < 0) {
        if (error) *error = "cannot start trusted xdelta3 decoder";
        return false;
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        if (error) *error = "trusted xdelta3 decoder failed";
        return false;
    }
    return true;
#endif
}

bool valid_cached_disc(const std::filesystem::path& path,
                       const ModResolution::DerivedDisc& derived) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) &&
           std::filesystem::file_size(path, ec) == derived.output_size && !ec;
}

bool materialize_derived_disc(RuntimeMods& s, const ModResolution& plan,
                              std::filesystem::path& out, std::string* error) {
    out.clear();
    if (plan.derived_discs.empty()) return true;
    const ModResolution::DerivedDisc& derived = plan.derived_discs.front();
    std::string digest;
    if (!sha256_file(derived.patch, digest, error) ||
        digest != derived.patch_sha256) {
        if (error && error->empty())
            *error = derived.package_id + ": derived-disc patch checksum failed";
        else if (error && digest != derived.patch_sha256)
            *error = derived.package_id + ": derived-disc patch checksum failed";
        return false;
    }
    const std::filesystem::path cache_root = s.manager.root() / "cache";
    const std::filesystem::path cached = cache_root / (plan.fingerprint + ".bin");
    if (valid_cached_disc(cached, derived)) {
        out = cached;
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_root, ec);
    if (ec) {
        if (error) *error = "cannot create derived-disc cache: " + ec.message();
        return false;
    }
    const char* override_tool = std::getenv("PSXRECOMP_XDELTA3");
    const std::filesystem::path decoder =
        override_tool && override_tool[0]
            ? std::filesystem::path(override_tool)
#if defined(_WIN32)
            : s.manager.root().parent_path() / "xdelta3.exe";
#else
            : s.manager.root().parent_path() / "xdelta3";
#endif
    if (!std::filesystem::is_regular_file(decoder, ec)) {
        if (error) *error =
            "this mod needs the trusted xdelta3 decoder, but it is missing: " +
            decoder.string();
        return false;
    }
    const std::filesystem::path source = raw_image_path(s.disc_path, error);
    if (source.empty()) return false;
#if defined(_WIN32)
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = (unsigned long)getpid();
#endif
    const std::filesystem::path temporary =
        cache_root / (plan.fingerprint + ".tmp." + std::to_string(process_id));
    std::filesystem::remove(temporary, ec);
    std::fprintf(stdout, "psxrecomp: building derived disc for %s...\n",
                 derived.package_id.c_str());
    if (!run_xdelta_decode(decoder, source, derived.patch, temporary, error)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    if (!valid_cached_disc(temporary, derived)) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = derived.package_id +
            ": derived disc has the wrong output size";
        return false;
    }
    if (!sha256_file(temporary, digest, error) || digest != derived.output_sha256) {
        std::filesystem::remove(temporary, ec);
        if (error && digest != derived.output_sha256)
            *error = derived.package_id + ": derived disc checksum failed";
        return false;
    }
    std::filesystem::rename(temporary, cached, ec);
    if (ec) {
        std::filesystem::remove(cached, ec);
        ec.clear();
        std::filesystem::rename(temporary, cached, ec);
    }
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = "cannot publish derived-disc cache: " + ec.message();
        return false;
    }
    std::fprintf(stdout, "psxrecomp: cached derived disc %s\n", cached.string().c_str());
    out = cached;
    return true;
}

#if defined(RECOMP_LAUNCHER)
void copy_text(char* out, size_t capacity, const std::string& value) {
    if (!out || capacity == 0) return;
    std::snprintf(out, capacity, "%s", value.c_str());
}

int provider_package_count(void*) {
    return (int)state().manager.packages().size();
}

int provider_package_get(void*, int index, RecompLauncherCModPackage* out) {
    if (!out || index < 0) return 0;
    const auto& packages = state().manager.packages();
    if ((size_t)index >= packages.size()) return 0;
    auto item = packages.begin();
    std::advance(item, index);
    const ModPackage* package = selected_package(item->first);
    if (!package) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), package->id);
    copy_text(out->version, sizeof(out->version), package->version);
    copy_text(out->name, sizeof(out->name), package->name);
    copy_text(out->author, sizeof(out->author), package->author);
    copy_text(out->description, sizeof(out->description), package->description);
    copy_text(out->license, sizeof(out->license), package->license);
    const auto selection = state().manager.selections().find(package->id);
    out->enabled = selection != state().manager.selections().end() &&
                   selection->second.enabled;
    out->option_count = (int)package->options.size();
    out->removable = !out->enabled;
    return 1;
}

int provider_option_get(void*, const char* package_id, int index,
                        RecompLauncherCModOption* out) {
    if (!package_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package || (size_t)index >= package->options.size()) return 0;
    const ModOption& option = package->options[(size_t)index];
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), option.id);
    copy_text(out->label, sizeof(out->label), option.label);
    copy_text(out->description, sizeof(out->description), option.description);
    copy_text(out->group, sizeof(out->group), option.group);
    copy_text(out->value, sizeof(out->value), selected_value(*package, option));
    copy_text(out->default_value, sizeof(out->default_value), option.default_value);
    out->type = option.type == ModOptionType::Boolean ? RECOMP_MOD_OPTION_BOOLEAN :
                option.type == ModOptionType::Choice ? RECOMP_MOD_OPTION_CHOICE :
                                                       RECOMP_MOD_OPTION_INTEGER;
    out->min_value = option.min_value;
    out->max_value = option.max_value;
    out->step = option.step;
    out->choice_count = (int)option.choices.size();
    return 1;
}

int provider_choice_get(void*, const char* package_id, const char* option_id,
                        int index, RecompLauncherCModChoice* out) {
    if (!package_id || !option_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    const auto option = std::find_if(package->options.begin(), package->options.end(),
        [&](const ModOption& value) { return value.id == option_id; });
    if (option == package->options.end() || (size_t)index >= option->choices.size()) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->value, sizeof(out->value), option->choices[(size_t)index].value);
    copy_text(out->label, sizeof(out->label), option->choices[(size_t)index].label);
    return 1;
}

int provider_version_count(void*, const char* package_id) {
    if (!package_id) return 0;
    const auto package = state().manager.packages().find(package_id);
    return package == state().manager.packages().end() ? 0 : (int)package->second.size();
}

int provider_version_get(void*, const char* package_id, int index,
                         RecompLauncherCModVersion* out) {
    if (!package_id || !out || index < 0) return 0;
    const auto package = state().manager.packages().find(package_id);
    if (package == state().manager.packages().end() ||
        (size_t)index >= package->second.size()) return 0;
    auto version = package->second.begin();
    std::advance(version, index);
    std::memset(out, 0, sizeof(*out));
    copy_text(out->version, sizeof(out->version), version->first);
    const ModPackage* selected = selected_package(package_id);
    out->selected = selected && selected->version == version->first;
    const auto selection = state().manager.selections().find(package_id);
    out->removable = selection == state().manager.selections().end() ||
                     !selection->second.enabled || !out->selected;
    return 1;
}

template <typename Callback>
int mutate(Callback callback) {
    std::string error;
    if (!callback(error)) {
        set_error(error);
        return 0;
    }
    state().error.clear();
    return 1;
}

int provider_install(void*, const char* path) {
    if (!path) return 0;
    return mutate([&](std::string& error) {
        std::string id, version;
        if (!state().manager.install_archive(path, &id, &version, &error)) return false;
        if (!state().manager.scan(&error)) return false;
        return state().manager.select_version(id, version, &error);
    });
}

int provider_remove(void*, const char* id, const char* version) {
    if (!id || !version) return 0;
    return mutate([&](std::string& error) {
        return state().manager.remove_version(id, version, &error);
    });
}

int provider_enable(void*, const char* id, int enabled) {
    if (!id) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_enabled(id, enabled != 0, &error);
    });
}

int provider_select(void*, const char* id, const char* version) {
    if (!id || !version) return 0;
    return mutate([&](std::string& error) {
        return state().manager.select_version(id, version, &error);
    });
}

int provider_set_option(void*, const char* id, const char* option, const char* value) {
    if (!id || !option || !value) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_option(id, option, value, &error);
    });
}

int provider_commit(void*, const char* image_path) {
    std::string error;
    if (!mod_runtime_commit(image_path ? std::filesystem::path(image_path) :
                                      std::filesystem::path(), &error)) {
        set_error(error);
        return 0;
    }
    state().error.clear();
    return 1;
}

const char* provider_error(void*) {
    return state().error.c_str();
}

RecompLauncherCModProvider provider = {
    nullptr,
    provider_package_count,
    provider_package_get,
    provider_option_get,
    provider_choice_get,
    provider_version_count,
    provider_version_get,
    provider_install,
    provider_remove,
    provider_enable,
    provider_select,
    provider_set_option,
    provider_commit,
    provider_error,
};
#endif

} // namespace

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path,
                            std::string* error) {
    RuntimeMods& s = state();
    s.manager.set_root({});
    s.plan = {};
    s.game_id.clear();
    s.error.clear();
    s.exe_sha256.clear();
    s.disc_sha256.clear();
    s.disc_path.clear();
    s.effective_disc_path.clear();
    s.entry_phys = 0;
    s.initialized = false;
    s.main_applied = false;
    s.disc_enabled = false;
    s.disc_guard_failed = false;
    s.manager.set_root(root);
    s.game_id = game_id;
    s.entry_phys = game_entry_pc & 0x1FFFFFFFu;
    if (!s.manager.scan(&s.error) || !s.manager.load_state(&s.error)) {
        if (error) *error = s.error;
        return false;
    }
    if (!sha256_file(exe_path, s.exe_sha256, &s.error)) {
        /* Release installs commonly do not carry a loose PS-X EXE; game-id and
         * expected-byte guards remain available in that case. */
        s.exe_sha256.clear();
        s.error.clear();
    }
    s.initialized = true;
    return true;
}

bool mod_runtime_commit(const std::filesystem::path& disc_path, std::string* error) {
    RuntimeMods& s = state();
    if (!s.initialized) return true;
    if (disc_path != s.disc_path) {
        std::string hash_error;
        std::string digest;
        if (!sha256_file(disc_path, digest, &hash_error)) digest.clear();
        s.disc_path = disc_path;
        s.disc_sha256 = std::move(digest);
    }
    ModResolution plan =
        s.manager.resolve(s.game_id, s.exe_sha256, s.disc_sha256);
    if (!plan.ok) {
        s.error.clear();
        for (const std::string& item : plan.errors) {
            if (!s.error.empty()) s.error += "\n";
            s.error += item;
        }
        if (error) *error = s.error;
        return false;
    }
    std::filesystem::path effective_disc;
    if (!materialize_derived_disc(s, plan, effective_disc, &s.error)) {
        if (error) *error = s.error;
        return false;
    }
    if (!s.manager.save_state(&s.error)) {
        if (error) *error = s.error;
        return false;
    }
    s.plan = std::move(plan);
    s.effective_disc_path = std::move(effective_disc);
    s.main_applied = false;
    s.error.clear();
    return true;
}

const std::string& mod_runtime_fingerprint() {
    return state().plan.fingerprint;
}

const std::filesystem::path& mod_runtime_effective_disc_path() {
    return state().effective_disc_path;
}

#if defined(RECOMP_LAUNCHER)
const RecompLauncherCModProvider* mod_runtime_launcher_provider() {
    return &provider;
}
#endif

} // namespace PSXRecompV4

extern "C" void mod_runtime_on_dispatch(uint32_t target) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || s.main_applied ||
        (target & 0x1FFFFFFFu) != s.entry_phys) return;

    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        for (size_t i = 0; i < write.expected.size(); ++i) {
            if (psx_read_byte((uint32_t)write.location + (uint32_t)i) !=
                write.expected[i]) {
                std::fprintf(stderr,
                    "psxrecomp: mod plan %s rejected at 0x%08X "
                    "(expected-byte guard failed; booting unmodified)\n",
                    s.plan.fingerprint.c_str(),
                    (unsigned)((uint32_t)write.location + (uint32_t)i));
                s.main_applied = true;
                return;
            }
        }
    }
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        for (size_t i = 0; i < write.replacement.size(); ++i)
            psx_write_byte((uint32_t)write.location + (uint32_t)i,
                           write.replacement[i]);
        dirty_ram_mark_executable_range(
            (uint32_t)write.location & 0x1FFFFFFFu,
            (uint32_t)write.replacement.size());
    }
    s.main_applied = true;
    if (!s.plan.writes.empty())
        std::fprintf(stdout, "psxrecomp: applied mod plan %s\n",
                     s.plan.fingerprint.c_str());
}

extern "C" void mod_runtime_enable_disc_patches(void) {
    PSXRecompV4::state().disc_enabled = true;
}

extern "C" void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                               uint8_t* bytes, uint32_t size) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.disc_enabled || s.disc_guard_failed ||
        !bytes || size == 0) return;
    const ModPatchTarget target =
        raw_sector ? ModPatchTarget::DiscRaw : ModPatchTarget::DiscUser;
    const uint64_t base = (uint64_t)lba * size;
    const uint64_t end = base + size;
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != target || write.location < base ||
            write.location + write.replacement.size() > end) continue;
        const size_t offset = (size_t)(write.location - base);
        if (std::memcmp(bytes + offset, write.expected.data(),
                        write.expected.size()) != 0) {
            std::fprintf(stderr,
                "psxrecomp: disc mod plan %s rejected at LBA %u+%zu "
                "(expected-byte guard failed; disc overlay disabled)\n",
                s.plan.fingerprint.c_str(), lba, offset);
            s.disc_guard_failed = true;
            return;
        }
    }
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != target || write.location < base ||
            write.location + write.replacement.size() > end) continue;
        const size_t offset = (size_t)(write.location - base);
        std::memcpy(bytes + offset, write.replacement.data(),
                    write.replacement.size());
    }
}
