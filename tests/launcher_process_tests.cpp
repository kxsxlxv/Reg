#include "launcher/ProcessManager.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--test-child") {
        std::cout << "launcher-stdout" << std::endl;
        std::cerr << "launcher-stderr" << std::endl;
        return 0;
    }

    namespace fs = std::filesystem;
    using reg::launcher::ProcessManager;
    const auto directory = fs::temp_directory_path() / "reg-launcher-process-test";
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    fs::create_directories(directory);

#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (count == 0 || count >= path.size()) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    path.resize(count);
    const fs::path executable(path);
#else
    const auto executable = fs::read_symlink("/proc/self/exe");
#endif

    ProcessManager process;
    process.start(executable, {"--test-child"}, directory);
    assert(process.running());
    bool finished = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const auto code = process.poll();
        if (code) {
            if (*code != 0) {
                std::ifstream error(directory / "stderr.log");
                std::string details((std::istreambuf_iterator<char>(error)),
                                    std::istreambuf_iterator<char>());
                throw std::runtime_error("Child exited with code " +
                                         std::to_string(*code) + ": " + details);
            }
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!finished) {
        process.requestStop();
        throw std::runtime_error("Child did not finish within test timeout");
    }
    std::string stdoutText;
    std::string stderrText;
    {
        std::ifstream file(directory / "stdout.log");
        stdoutText.assign(std::istreambuf_iterator<char>(file),
                          std::istreambuf_iterator<char>());
    }
    {
        std::ifstream file(directory / "stderr.log");
        stderrText.assign(std::istreambuf_iterator<char>(file),
                          std::istreambuf_iterator<char>());
    }
    assert(stdoutText.find("launcher-stdout") != std::string::npos);
    assert(stderrText.find("launcher-stderr") != std::string::npos);
    fs::remove_all(directory);
}
