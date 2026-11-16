#include "process_runner.h"

#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#endif

namespace workbench {
namespace {

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("invalid UTF-8 process argument");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring quote_windows(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(character);
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(character);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}
#endif

}  // namespace

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory,
                          ProcessControl* control,
                          OutputCallback output_callback) {
    if (arguments.empty() || arguments.front().empty()) {
        throw std::runtime_error("process argument list is empty");
    }
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        throw std::runtime_error("CreatePipe failed");
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command_line;
    for (const auto& argument : arguments) {
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }
        command_line += quote_windows(utf8_to_wide(argument));
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    const std::wstring application = utf8_to_wide(arguments.front());
    const std::wstring directory = working_directory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        CloseHandle(write_pipe);
        CloseHandle(read_pipe);
        throw std::runtime_error("CreateJobObjectW failed: " +
                                 std::to_string(GetLastError()));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
    job_info.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &job_info, sizeof(job_info))) {
        CloseHandle(job);
        CloseHandle(write_pipe);
        CloseHandle(read_pipe);
        throw std::runtime_error("SetInformationJobObject failed: " +
                                 std::to_string(GetLastError()));
    }
    const BOOL created = CreateProcessW(
        application.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(),
        &startup, &process);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(job);
        CloseHandle(read_pipe);
        throw std::runtime_error("CreateProcessW failed: " +
                                 std::to_string(GetLastError()));
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 126);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        CloseHandle(read_pipe);
        throw std::runtime_error("AssignProcessToJobObject failed: " +
                                 std::to_string(GetLastError()));
    }
    ResumeThread(process.hThread);

    ProcessResult result;
    std::thread reader([&]() {
        char buffer[4096];
        DWORD count = 0;
        while (ReadFile(read_pipe, buffer, sizeof(buffer), &count, nullptr) && count) {
            result.output.append(buffer, buffer + count);
            if (output_callback) {
                output_callback(std::string_view(buffer, count));
            }
        }
    });
    while (WaitForSingleObject(process.hProcess, 50) == WAIT_TIMEOUT) {
        if (control && control->cancel_requested.load()) {
            result.canceled = true;
            TerminateJobObject(job, 130);
            break;
        }
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    reader.join();
    CloseHandle(read_pipe);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    result.exit_code = static_cast<int>(exit_code);
    return result;
#else
    int descriptors[2];
    if (pipe(descriptors) != 0) {
        throw std::runtime_error("pipe failed");
    }
    const pid_t process = fork();
    if (process < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw std::runtime_error("fork failed");
    }
    if (process == 0) {
        setpgid(0, 0);
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO);
        dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        if (chdir(working_directory.c_str()) != 0) {
            _exit(126);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    setpgid(process, process);
    close(descriptors[1]);
    ProcessResult result;
    std::thread reader([&]() {
        std::array<char, 4096> buffer{};
        ssize_t count = 0;
        while ((count = read(descriptors[0], buffer.data(), buffer.size())) > 0) {
            const auto size = static_cast<std::size_t>(count);
            result.output.append(buffer.data(), size);
            if (output_callback) {
                output_callback(std::string_view(buffer.data(), size));
            }
        }
    });
    int status = 0;
    while (waitpid(process, &status, WNOHANG) == 0) {
        if (control && control->cancel_requested.load()) {
            result.canceled = true;
            kill(-process, SIGKILL);
            waitpid(process, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    reader.join();
    close(descriptors[0]);
    result.exit_code = result.canceled ? 130 :
        (WIFEXITED(status) ? WEXITSTATUS(status) : 128);
    return result;
#endif
}

}  // namespace workbench
