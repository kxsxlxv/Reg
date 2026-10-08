#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
namespace reg::launcher {

enum class UpdatePhase {
    Idle, Checking, UpToDate, Available, Downloading, Prepared, Error
};

struct UpdateView {
    UpdatePhase phase{UpdatePhase::Idle};
    std::string channel{"dev"};
    std::string tag;
    std::string message;
    std::size_t changedFiles{0};
    std::uint64_t totalBytes{0};
    std::uint64_t downloadedBytes{0};
};

class UpdateManager {
public:
    explicit UpdateManager(std::filesystem::path appDirectory);
    ~UpdateManager();

    UpdateManager(const UpdateManager&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;

    void checkAsync(std::string channel);
    void downloadAsync();
    UpdateView snapshot() const;
    void applyAndRestart(); // Starts the detached helper; caller must exit its process.
private:
    struct File {
        std::string path, asset, url, sha256;
        std::uint64_t size{0};
    };
    void setError(const std::string& error);
    void check(std::string channel);
    void download();
    void startWorker(std::thread worker);
    bool busy() const;

    std::filesystem::path appDirectory_;
    std::filesystem::path stageDirectory_;
    mutable std::mutex mutex_;
    std::thread worker_;
    UpdateView view_;
    std::vector<File> changed_;
    std::string manifestText_;
};

} // namespace reg::launcher
#endif
