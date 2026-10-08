#include "launcher/ProcessManager.hpp"

#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <string_view>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace reg::launcher {
namespace {
#ifdef _WIN32
std::wstring toWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length == 0) throw std::runtime_error("Invalid UTF-8 process argument");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}

// Matches CommandLineToArgvW escaping rules, including runs of backslashes.
std::wstring quote(const std::wstring& arg) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result += c;
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

BOOL CALLBACK closeWindow(HWND window, LPARAM data) {
    DWORD windowPid = 0;
    GetWindowThreadProcessId(window, &windowPid);
    if (windowPid == static_cast<DWORD>(data)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

HANDLE outputFile(const std::filesystem::path& path) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "Cannot create output log");
    }
    return file;
}
#else
int outputFile(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor == -1) {
        throw std::system_error(errno, std::generic_category(), "Cannot create output log");
    }
    return descriptor;
}
#endif
} // namespace

ProcessManager::~ProcessManager() {
    if (!running()) return;
    requestStop();
    // Never leave a child without supervision when launcher exits.
#ifdef _WIN32
    WaitForSingleObject(process_, 2000);
    if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 2000);
    }
    CloseHandle(process_);
    process_ = nullptr;
#else
    int status = 0;
    for (int i = 0; i < 20; ++i) {
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            return;
        }
        usleep(100000);
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, &status, 0);
    pid_ = -1;
#endif
}

bool ProcessManager::running() const noexcept {
#ifdef _WIN32
    return process_ != nullptr;
#else
    return pid_ > 0;
#endif
}

void ProcessManager::start(
    const std::filesystem::path& executable,
    const std::vector<std::string>& args,
    const std::filesystem::path& sessionDirectory) {
    if (running()) throw std::runtime_error("A child is already running");
    if (!std::filesystem::is_regular_file(executable)) {
        throw std::runtime_error("Executable not found: " + executable.string());
    }
    const auto stdoutPath = sessionDirectory / "stdout.log";
    const auto stderrPath = sessionDirectory / "stderr.log";
#ifdef _WIN32
    HANDLE stdoutHandle = outputFile(stdoutPath);
    HANDLE stderrHandle = nullptr;
    try {
        stderrHandle = outputFile(stderrPath);
        std::wstring command = quote(executable.wstring());
        for (const auto& arg : args) {
            command += L" ";
            command += quote(toWide(arg));
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = stdoutHandle;
        startup.hStdError = stderrHandle;
        PROCESS_INFORMATION result{};
        auto workingDir = sessionDirectory.wstring();
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                TRUE, CREATE_NO_WINDOW, nullptr, workingDir.c_str(),
                &startup, &result)) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "CreateProcessW");
        }
        CloseHandle(result.hThread);
        process_ = result.hProcess;
        pid_ = result.dwProcessId;
    } catch (...) {
        if (stderrHandle) CloseHandle(stderrHandle);
        CloseHandle(stdoutHandle);
        throw;
    }
    CloseHandle(stderrHandle);
    CloseHandle(stdoutHandle);
#else
    const int stdoutFd = outputFile(stdoutPath);
    int stderrFd;
    try {
        stderrFd = outputFile(stderrPath);
    } catch (...) {
        close(stdoutFd);
        throw;
    }
    const pid_t child = fork();
    if (child == -1) {
        const int error = errno;
        close(stdoutFd);
        close(stderrFd);
        throw std::system_error(error, std::generic_category(), "fork");
    }
    if (child == 0) {
        if (chdir(sessionDirectory.c_str()) != 0 ||
            dup2(stdoutFd, STDOUT_FILENO) < 0 ||
            dup2(stderrFd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(stdoutFd);
        close(stderrFd);
        std::vector<std::string> argvStrings{executable.string()};
        argvStrings.insert(argvStrings.end(), args.begin(), args.end());
        std::vector<char*> argv;
        argv.reserve(argvStrings.size() + 1);
        for (auto& value : argvStrings) argv.push_back(value.data());
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    close(stdoutFd);
    close(stderrFd);
    pid_ = child;
#endif
    stopRequestedAt_ = {};
}

void ProcessManager::requestStop() {
    if (!running() || stopRequestedAt_ != std::chrono::steady_clock::time_point{}) {
        return;
    }
    stopRequestedAt_ = std::chrono::steady_clock::now();
#ifdef _WIN32
    EnumWindows(closeWindow, static_cast<LPARAM>(pid_));
#else
    kill(pid_, SIGTERM);
#endif
}

std::optional<int> ProcessManager::poll() {
    if (!running()) return std::nullopt;
    constexpr auto grace = std::chrono::seconds(5);
    const bool expired = stopRequestedAt_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - stopRequestedAt_ > grace;
#ifdef _WIN32
    const DWORD waiting = WaitForSingleObject(process_, 0);
    if (waiting == WAIT_TIMEOUT && expired) {
        TerminateProcess(process_, 1);
        return std::nullopt;
    }
    if (waiting == WAIT_TIMEOUT) return std::nullopt;
    if (waiting != WAIT_OBJECT_0) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), "WaitForSingleObject");
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process_, &exitCode);
    CloseHandle(process_);
    process_ = nullptr;
    return static_cast<int>(exitCode);
#else
    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == 0) {
        if (expired) kill(pid_, SIGKILL);
        return std::nullopt;
    }
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), "waitpid");
    }
    pid_ = -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

} // namespace reg::launcher
