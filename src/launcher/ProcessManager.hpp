#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace reg::launcher {

class ProcessManager {
public:
    ProcessManager() = default;
    ~ProcessManager();
    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    void start(
        const std::filesystem::path& executable,
        const std::vector<std::string>& args,
        const std::filesystem::path& sessionDirectory);
    void requestStop();
    std::optional<int> poll();
    bool running() const noexcept;
private:
#ifdef _WIN32
    HANDLE process_{nullptr};
    DWORD pid_{0};
#else
    pid_t pid_{-1};
#endif
    std::chrono::steady_clock::time_point stopRequestedAt_{};
};

} // namespace reg::launcher
