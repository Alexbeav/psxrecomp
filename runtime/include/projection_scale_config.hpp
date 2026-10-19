#pragma once
#include <filesystem>

/* Host-only [video] extension. Malformed or out-of-range values warn on stderr and yield 1.0. */
double psx_projection_scale_load_config(const std::filesystem::path& path);
