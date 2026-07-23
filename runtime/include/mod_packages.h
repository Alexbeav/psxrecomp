#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace PSXRecompV4 {

enum class ModOptionType {
    Boolean,
    Choice,
    Integer,
};

struct ModChoice {
    std::string value;
    std::string label;
};

struct ModOption {
    std::string id;
    std::string label;
    std::string description;
    std::string group;
    ModOptionType type = ModOptionType::Boolean;
    std::string default_value;
    int64_t min_value = 0;
    int64_t max_value = 0;
    int64_t step = 1;
    std::vector<ModChoice> choices;
};

struct ModRequirement {
    std::string id;
    std::string version;
};

struct ModTarget {
    std::string game_id;
    std::string exe_sha256;
    std::string disc_sha256;
};

enum class ModPatchTarget {
    MainExe,
    DiscRaw,
    DiscUser,
};

struct ModPatch {
    ModPatchTarget target = ModPatchTarget::MainExe;
    uint64_t location = 0; /* guest address or canonical disc-stream byte offset */
    std::vector<uint8_t> expected;
    std::vector<uint8_t> replacement;
    std::string when_option;
    std::string when_value;
    int64_t order = 0;
};

struct ModPackage {
    uint32_t format_version = 0;
    std::string id;
    std::string version;
    std::string name;
    std::string author;
    std::string description;
    std::string license;
    std::string resolver = "declarative";
    std::string save_compatibility = "shared";
    std::filesystem::path root;
    std::vector<ModTarget> targets;
    std::vector<ModRequirement> dependencies;
    std::vector<std::string> conflicts;
    std::vector<ModOption> options;
    std::vector<ModPatch> patches;
};

struct ModSelection {
    bool enabled = false;
    std::string version;
    std::map<std::string, std::string> values;
};

struct ModResolution {
    bool ok = false;
    std::string fingerprint;
    std::vector<const ModPackage*> ordered;
    struct Write {
        ModPatchTarget target = ModPatchTarget::MainExe;
        uint64_t location = 0;
        std::vector<uint8_t> expected;
        std::vector<uint8_t> replacement;
        std::string package_id;
    };
    std::vector<Write> writes;
    std::vector<std::string> errors;
};

using ModBuiltinResolver = std::function<bool(
    const ModPackage& package,
    const ModSelection& selection,
    std::vector<ModResolution::Write>& writes,
    std::vector<std::string>& errors)>;

class ModPackageManager {
public:
    explicit ModPackageManager(std::filesystem::path mods_root = {});

    void set_root(std::filesystem::path mods_root);
    const std::filesystem::path& root() const { return root_; }

    bool scan(std::string* error = nullptr);
    bool load_state(std::string* error = nullptr);
    bool save_state(std::string* error = nullptr) const;

    bool install_archive(const std::filesystem::path& archive,
                         std::string* installed_id = nullptr,
                         std::string* installed_version = nullptr,
                         std::string* error = nullptr);
    bool remove_version(const std::string& id, const std::string& version,
                        std::string* error = nullptr);

    bool set_enabled(const std::string& id, bool enabled, std::string* error = nullptr);
    bool select_version(const std::string& id, const std::string& version,
                        std::string* error = nullptr);
    bool set_option(const std::string& id, const std::string& option,
                    const std::string& value, std::string* error = nullptr);

    const std::map<std::string, std::map<std::string, ModPackage>>& packages() const {
        return packages_;
    }
    const std::map<std::string, ModSelection>& selections() const { return selections_; }
    const ModPackage* selected_package(const std::string& id) const;

    ModResolution resolve(const std::string& game_id,
                          const std::string& exe_sha256 = {},
                          const std::string& disc_sha256 = {}) const;

    static bool read_manifest(const std::filesystem::path& path, ModPackage& out,
                              std::string* error = nullptr);

private:
    std::filesystem::path root_;
    std::map<std::string, std::map<std::string, ModPackage>> packages_;
    std::map<std::string, ModSelection> selections_;
};

bool mod_register_builtin_resolver(const std::string& id, ModBuiltinResolver resolver);
void mod_clear_builtin_resolvers_for_tests();

} // namespace PSXRecompV4
