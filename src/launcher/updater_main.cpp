#include "launcher/UpdateShared.hpp"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else {
            result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
            if (c == L'"') result += L'\\';
            result += c;
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct Change {
    std::string path;
    std::string sha256;
    bool existed{false};
};

void atomicCopy(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::path temporary = to;
    temporary += L".update-pending";
    std::error_code ignored;
    fs::remove(temporary, ignored);
    if (!CopyFileW(from.c_str(), temporary.c_str(), FALSE)) {
        throw std::runtime_error("Cannot write update file");
    }
    if (!MoveFileExW(temporary.c_str(), to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary, ignored);
        throw std::runtime_error("Cannot replace locked update file");
    }
}

void restart(const fs::path& root) {
    const auto exe = root / "Launcher.exe";
    std::wstring command = quote(exe.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION result{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, root.c_str(), &startup, &result)) {
        throw std::runtime_error("Cannot restart launcher");
    }
    CloseHandle(result.hThread);
    CloseHandle(result.hProcess);
}

void waitForParent(DWORD pid) {
    if (pid == 0) throw std::runtime_error("Missing parent process ID");
    const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!parent) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return; // already exited
        throw std::runtime_error("Cannot watch parent process");
    }
    const DWORD waited = WaitForSingleObject(parent, 120000);
    CloseHandle(parent);
    if (waited != WAIT_OBJECT_0) {
        throw std::runtime_error("Launcher did not exit before update");
    }
}

std::vector<Change> readPlan(const fs::path& stage) {
    std::ifstream input(stage / "plan.json");
    if (!input) throw std::runtime_error("Missing update plan");
    const auto plan = json::parse(input);
    if (plan.value("schema_version", 0) != 1 ||
        !plan.contains("files") || !plan["files"].is_array() ||
        plan["files"].empty() || plan["files"].size() > 512) {
        throw std::runtime_error("Invalid update plan");
    }
    std::vector<Change> result;
    for (const auto& entry : plan["files"]) {
        Change file{
            .path = entry.at("path").get<std::string>(),
            .sha256 = entry.at("sha256").get<std::string>()
        };
        if (!reg::launcher::validManagedPath(file.path) ||
            file.sha256.size() != 64) {
            throw std::runtime_error("Unsafe update plan path/hash");
        }
        for (const auto& previous : result) {
            if (previous.path == file.path) {
                throw std::runtime_error("Duplicate update path");
            }
        }
        result.push_back(std::move(file));
    }
    return result;
}

void install(const fs::path& root, const fs::path& stage) {
    if (!fs::is_regular_file(root / "Launcher.exe") ||
        !fs::is_regular_file(stage / "manifest.json")) {
        throw std::runtime_error("Invalid update destination/stage");
    }
    auto files = readPlan(stage);
    const auto backup = stage / "backup";

    // Verify *all* files before modifying the installation.
    for (auto& file : files) {
        const auto downloaded = reg::launcher::managedPath(stage / "new", file.path);
        if (!fs::is_regular_file(downloaded) ||
            reg::launcher::sha256File(downloaded) != file.sha256) {
            throw std::runtime_error("Update verification failed: " + file.path);
        }
        const auto destination = reg::launcher::managedPath(root, file.path);
        file.existed = fs::is_regular_file(destination);
        if (file.existed) {
            const auto previous = reg::launcher::managedPath(backup, file.path);
            fs::create_directories(previous.parent_path());
            fs::copy_file(destination, previous, fs::copy_options::overwrite_existing);
        }
    }
    const auto oldManifest = backup / "manifest.json";
    if (fs::is_regular_file(root / "manifest.json")) {
        fs::create_directories(backup);
        fs::copy_file(root / "manifest.json", oldManifest,
                      fs::copy_options::overwrite_existing);
    }

    std::vector<Change> installed;
    try {
        for (const auto& file : files) {
            atomicCopy(reg::launcher::managedPath(stage / "new", file.path),
                       reg::launcher::managedPath(root, file.path));
            installed.push_back(file);
        }
        atomicCopy(stage / "manifest.json", root / "manifest.json");
    } catch (...) {
        // Best effort rollback: app is closed and all originals were copied.
        for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
            try {
                const auto destination = reg::launcher::managedPath(root, it->path);
                if (it->existed) {
                    atomicCopy(reg::launcher::managedPath(backup, it->path), destination);
                } else {
                    fs::remove(destination);
                }
            } catch (...) { /* Preserve the original error; backups remain on disk. */ }
        }
        try {
            if (fs::is_regular_file(oldManifest)) {
                atomicCopy(oldManifest, root / "manifest.json");
            }
        } catch (...) {}
        throw;
    }
}

int selfTest() {
    const auto dir = fs::temp_directory_path() /
        ("VideoConsole-updater-selftest-" + std::to_string(GetCurrentProcessId()));
    const auto root = dir / "installed";
    const auto stage = dir / "staged";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(root);
    fs::create_directories(stage / "new");
    {
        std::ofstream(root / "Launcher.exe") << "placeholder";
        std::ofstream(root / "reg_probe.exe") << "old";
        std::ofstream(stage / "new" / "reg_probe.exe") << "new";
        std::ofstream(stage / "manifest.json") << "{}";
    }
    const auto checksum = reg::launcher::sha256File(stage / "new" / "reg_probe.exe");
    json plan = {{"schema_version", 1}, {"files", json::array()}};
    plan["files"].push_back({{"path", "reg_probe.exe"}, {"sha256", checksum}});
    {
        std::ofstream(stage / "plan.json") << plan.dump();
    }
    install(root, stage);
    if (reg::launcher::sha256File(root / "reg_probe.exe") != checksum ||
        !fs::exists(stage / "backup" / "reg_probe.exe")) {
        throw std::runtime_error("Updater self-test failed");
    }
    fs::remove_all(dir);
    return 0;
}

int run(int argc, LPWSTR* argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") {
        return selfTest();
    }
    if (argc != 7 || std::wstring_view(argv[1]) != L"--pid" ||
        std::wstring_view(argv[3]) != L"--root" ||
        std::wstring_view(argv[5]) != L"--stage") {
        throw std::runtime_error("Invalid updater invocation");
    }
    const DWORD pid = static_cast<DWORD>(std::stoul(argv[2]));
    const fs::path root = fs::absolute(fs::path(argv[4]));
    const fs::path stage = fs::absolute(fs::path(argv[6]));
    if (root == stage || !fs::is_directory(stage)) {
        throw std::runtime_error("Invalid update directories");
    }
    waitForParent(pid);
    try {
        install(root, stage);
        std::ofstream(stage / "result.txt") << "Installed successfully\n";
    } catch (const std::exception& e) {
        std::ofstream(stage / "result.txt") << "Update failed: " << e.what() << '\n';
        // An old version is still available after rollback.
        try { restart(root); } catch (...) {}
        throw;
    }
    restart(root);
    return 0;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    try {
        const int result = run(argc, argv);
        LocalFree(argv);
        return result;
    } catch (const std::exception& error) {
        const bool testMode = argc == 2 && std::wstring_view(argv[1]) == L"--self-test";
        LocalFree(argv);
        if (!testMode) {
            MessageBoxA(nullptr, error.what(), "Update failed", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
