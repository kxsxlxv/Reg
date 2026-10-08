#include "launcher/ProcessManager.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    using reg::launcher::ProcessManager;
    const auto directory =
        fs::temp_directory_path() / "reg-launcher-process-test";
    fs::remove_all(directory);
    fs::create_directories(directory);
#ifdef _WIN32
    const char* systemRoot = std::getenv("SystemRoot");
    if (!systemRoot) throw std::runtime_error("SystemRoot not set");
    const fs::path executable =
        fs::path(systemRoot) / "System32" / "cmd.exe";
    const std::vector<std::string> args{
        "/C", "echo launcher-stdout & echo launcher-stderr 1>&2"};
#else
    const fs::path executable = "/bin/sh";
    const std::vector<std::string> args{
        "-c", "echo launcher-stdout; echo launcher-stderr 1>&2"};
#endif
    ProcessManager process;
    process.start(executable, args, directory);
    assert(process.running());
    bool finished = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const auto code = process.poll();
        if (code) {
            assert(*code == 0);
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!finished) {
        process.requestStop();
        throw std::runtime_error("Child did not finish within test timeout");
    }
    std::ifstream stdoutFile(directory / "stdout.log");
    std::ifstream stderrFile(directory / "stderr.log");
    std::string stdoutText((std::istreambuf_iterator<char>(stdoutFile)),
                           std::istreambuf_iterator<char>());
    std::string stderrText((std::istreambuf_iterator<char>(stderrFile)),
                           std::istreambuf_iterator<char>());
    assert(stdoutText.find("launcher-stdout") != std::string::npos);
    assert(stderrText.find("launcher-stderr") != std::string::npos);
    fs::remove_all(directory);
}
