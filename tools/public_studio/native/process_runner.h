#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench {

struct ProcessResult {
    int exit_code = -1;
    std::string output;
    bool canceled = false;
};

struct ProcessControl {
    std::atomic<bool> cancel_requested{false};

    void request_cancel() { cancel_requested.store(true); }
};

using OutputCallback = std::function<void(std::string_view)>;

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory,
                          ProcessControl* control = nullptr,
                          OutputCallback output_callback = {});

}  // namespace workbench
