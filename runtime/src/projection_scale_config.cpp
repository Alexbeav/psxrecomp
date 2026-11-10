#include "projection_scale_config.hpp"
#include "projection_scale.hpp"
#include "toml.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>

/* A bad [video] fov_scale must never stop the game starting (this runs inside
 * main()'s --game try block): warn and fall back to faithful, like the
 * PSX_GTE_FOV_SCALE path. Integers (fov_scale = 2) are accepted. */
double psx_projection_scale_load_config(const std::filesystem::path& path) {
    try {
        const auto config = toml::parse(path.string());
        if (!config.contains("video")) return 1.0;
        const auto& video = toml::find(config, "video");
        if (!video.contains("fov_scale")) return 1.0;
        const auto& node = toml::find(video, "fov_scale");
        double value;
        if (node.is_floating())     value = node.as_floating();
        else if (node.is_integer()) value = static_cast<double>(node.as_integer());
        else throw std::runtime_error("must be a number");
        if (!psx_projection_scale_valid(value))
            throw std::runtime_error("must be > 0 and <= 8");
        return value;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "psxrecomp: [video] fov_scale ignored (%s); using 1.0 (faithful)\n", e.what());
        return 1.0;
    }
}
