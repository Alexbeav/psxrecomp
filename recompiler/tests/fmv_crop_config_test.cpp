/* fmv_crop_config_test - game.toml [video] fmv_crop_to_picture.
 *
 *   1. absent means off: a 24-bit movie frame keeps the active-video canvas;
 *   2. true turns the crop on, false spells the default;
 *   3. it sits beside depth24_trailing_margin without changing it. */
#include "config_loader.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
    const auto root = fs::temp_directory_path() /
        ("fmv-crop-config-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto config = root / "game.toml";
    auto load = [&](const std::string& video) {
        {
            std::ofstream f(config);
            f << "[game]\nname=\"Test\"\nid=\"SLUS-TEST\"\nexe=\"stock.exe\"\n"
                 "load_address=\"0x80010000\"\nentry_pc=\"0x80010000\"\ntext_size=\"0x1000\"\n"
                 "stack_base=\"0x801ffff0\"\n[recompiler]\nseeds=\"seeds.txt\"\n"
              << video;
        }
        return PSXRecompV4::load_game_config(config);
    };

    check(!PSXRecompV4::RuntimeConfig{}.video_fmv_crop_to_picture,
          "a default RuntimeConfig: crop off");
    check(!load("").runtime.video_fmv_crop_to_picture, "absent: crop off");
    check(!load("[video]\nfmv_chroma_smoothing=true\n").runtime.video_fmv_crop_to_picture,
          "a [video] block without the key: off");
    check(load("[video]\nfmv_crop_to_picture=true\n").runtime.video_fmv_crop_to_picture,
          "true turns the crop on");
    check(!load("[video]\nfmv_crop_to_picture=false\n").runtime.video_fmv_crop_to_picture,
          "false is the default spelled out");
    const auto both = load("[video]\nfmv_crop_to_picture=true\n"
                           "depth24_trailing_margin=0\n");
    check(both.runtime.video_fmv_crop_to_picture &&
          both.runtime.video_depth24_trailing_margin == 0 &&
          both.runtime.video_depth24_trailing_margin_set,
          "depth24_trailing_margin is kept beside it");

    fs::remove(config);
    fs::remove(root);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("fmv_crop_config_test: ok");
    return 0;
}
