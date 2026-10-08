#include "launcher/UpdateManager.hpp"

#ifdef _WIN32

#include "launcher/LauncherConfig.hpp"
#include "launcher/UpdateShared.hpp"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace reg::launcher {
namespace {
using json = nlohmann::json;
constexpr std::uint64_t kMaxFileBytes = 650ULL * 1024 * 1024;
constexpr std::uint64_t kMaxUpdateBytes = 1500ULL * 1024 * 1024;
constexpr std::size_t kMaxFiles = 512;
constexpr std::size_t kMaxApiBytes = 8 * 1024 * 1024;

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid UTF-8 in update metadata");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), count);
    return output;
}

std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned slashCount = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashCount;
        } else {
            if (c == L'"') result.append(slashCount * 2 + 1, L'\\');
            else result.append(slashCount, L'\\');
            result += c;
            slashCount = 0;
        }
    }
    result.append(slashCount * 2, L'\\');
    return result + L"\"";
}

struct HttpHandle {
    HINTERNET value{};
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle() = default;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

void ensureWin(bool ok, const char* name) {
    if (!ok) {
        throw std::runtime_error(std::string(name) + " (WinHTTP error " +
                                 std::to_string(GetLastError()) + ")");
    }
}

bool hexSha256(const std::string& text) {
    return text.size() == 64 &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

bool safeAsset(std::string_view asset) {
    if (asset.size() < 5 || asset.size() > 180) return false;
    for (unsigned char c : asset) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

bool safeTag(const std::string& tag, const std::string& channel) {
    if (channel == "dev") {
        if (tag.size() != 44 || !tag.starts_with("dev-")) return false;
        return std::all_of(tag.begin() + 4, tag.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
    }
    if (channel == "stable") {
        if (tag.size() < 6 || tag.size() > 40 || tag[0] != 'v') return false;
        return std::all_of(tag.begin() + 1, tag.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-';
        });
    }
    return false;
}

// WinHTTP performs certificate validation. Redirects to GitHub's asset CDN
// are HTTPS-only. Never accept a URL supplied by the downloaded manifest.
void getHttps(
    const std::string& url,
    const std::function<void(const char*, std::size_t)>& consume) {
    auto wide = widen(url);
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    ensureWin(WinHttpCrackUrl(wide.c_str(), 0, 0, &components), "WinHttpCrackUrl");
    if (components.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("Update source must use HTTPS");
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    HttpHandle session(WinHttpOpen(L"VideoConsole-Update/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    ensureWin(session.value != nullptr, "WinHttpOpen");
    ensureWin(WinHttpSetTimeouts(session.value, 8000, 8000, 18000, 18000),
              "WinHttpSetTimeouts");
    HttpHandle connection(WinHttpConnect(session.value,
        host.c_str(), components.nPort, 0));
    ensureWin(connection.value != nullptr, "WinHttpConnect");
    HttpHandle request(WinHttpOpenRequest(connection.value, L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    ensureWin(request.value != nullptr, "WinHttpOpenRequest");
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n"
                             L"X-GitHub-Api-Version: 2022-11-28\r\n";
    ensureWin(WinHttpAddRequestHeaders(
        request.value, headers, static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD), "WinHttpAddRequestHeaders");
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    ensureWin(WinHttpSetOption(
        request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)),
        "WinHttpSetOption");
    ensureWin(WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS,
        0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "WinHttpSendRequest");
    ensureWin(WinHttpReceiveResponse(request.value, nullptr), "WinHttpReceiveResponse");
    DWORD status = 0, length = sizeof(status);
    ensureWin(WinHttpQueryHeaders(request.value,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX),
        "WinHttpQueryHeaders");
    if (status != HTTP_STATUS_OK) {
        throw std::runtime_error("GitHub returned HTTP " + std::to_string(status));
    }
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        DWORD got = 0;
        ensureWin(WinHttpReadData(
            request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &got),
            "WinHttpReadData");
        if (got == 0) break;
        consume(buffer.data(), got);
    }
}

std::string readHttpText(const std::string& url) {
    std::string text;
    getHttps(url, [&](const char* bytes, std::size_t count) {
        if (text.size() + count > kMaxApiBytes) {
            throw std::runtime_error("Update API response is too large");
        }
        text.append(bytes, count);
    });
    return text;
}

std::string urlForManifestAsset(const json& release, std::string_view assetName) {
    const auto& assets = release.at("assets");
    for (const auto& a : assets) {
        if (a.value("name", "") == assetName) {
            const std::string url = a.at("browser_download_url").get<std::string>();
            const std::string prefix =
                "https://github.com/kxsxlxv/Reg/releases/download/";
            if (!url.starts_with(prefix)) {
                throw std::runtime_error("Unexpected release asset host");
            }
            return url;
        }
    }
    throw std::runtime_error("Missing release asset: " + std::string(assetName));
}
} // namespace

UpdateManager::UpdateManager(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {}

UpdateManager::~UpdateManager() {
    if (worker_.joinable()) worker_.join();
}

bool UpdateManager::busy() const {
    return view_.phase == UpdatePhase::Checking ||
           view_.phase == UpdatePhase::Downloading;
}

void UpdateManager::setError(const std::string& error) {
    std::lock_guard lock(mutex_);
    view_.phase = UpdatePhase::Error;
    view_.message = error;
}

UpdateView UpdateManager::snapshot() const {
    std::lock_guard lock(mutex_);
    return view_;
}

void UpdateManager::startWorker(std::thread next) {
    if (worker_.joinable()) worker_.join();
    worker_ = std::move(next);
}

void UpdateManager::checkAsync(std::string channel) {
    if (channel != "dev" && channel != "stable") return;
    // Development build trees are not managed installations. Never overwrite
    // a directory that was not produced by the portable packager.
    if (!std::filesystem::is_regular_file(appDirectory_ / "Launcher.exe") ||
        !std::filesystem::is_regular_file(appDirectory_ / "manifest.json")) {
        std::lock_guard lock(mutex_);
        view_ = {};
        view_.phase = UpdatePhase::UpToDate;
        view_.message = "Автообновления доступны в portable-сборке";
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (busy()) return;
        view_ = {};
        view_.channel = channel;
        view_.phase = UpdatePhase::Checking;
        view_.message = "Проверка обновлений...";
        changed_.clear();
    }
    startWorker(std::thread([this, channel = std::move(channel)] {
        try { check(channel); }
        catch (const std::exception& e) { setError(e.what()); }
    }));
}

void UpdateManager::check(std::string channel) {
    const auto releases = json::parse(readHttpText(
        "https://api.github.com/repos/kxsxlxv/Reg/releases?per_page=40"));
    if (!releases.is_array()) throw std::runtime_error("Invalid releases API response");
    const json* selected = nullptr;
    std::string tag;
    for (const auto& release : releases) {
        if (release.value("draft", true)) continue;
        const auto current = release.value("tag_name", "");
        if (!safeTag(current, channel)) continue;
        if (channel == "dev" && !release.value("prerelease", false)) continue;
        if (channel == "stable" && release.value("prerelease", true)) continue;
        selected = &release;
        tag = current;
        break;
    }
    if (!selected) {
        std::lock_guard lock(mutex_);
        view_.phase = UpdatePhase::UpToDate;
        view_.message = "Для этого канала пока нет опубликованных сборок";
        return;
    }

    const std::string manifestUrl = urlForManifestAsset(*selected, "manifest.json");
    const std::string manifest = readHttpText(manifestUrl);
    const auto metadata = json::parse(manifest);
    if (metadata.value("schema_version", 0) != 1 ||
        metadata.value("repository", "") != "kxsxlxv/Reg" ||
        metadata.value("platform", "") != "windows-x64" ||
        metadata.value("tag", "") != tag ||
        metadata.value("channel", "") != channel) {
        throw std::runtime_error("Update manifest identity mismatch");
    }
    const auto& files = metadata.at("files");
    if (!files.is_array() || files.empty() || files.size() > kMaxFiles) {
        throw std::runtime_error("Invalid update file count");
    }
    std::vector<File> changed;
    std::set<std::string> names;
    std::uint64_t bytes = 0;
    for (const auto& entry : files) {
        File file;
        file.path = entry.at("path").get<std::string>();
        file.asset = entry.at("asset").get<std::string>();
        file.sha256 = entry.at("sha256").get<std::string>();
        file.size = entry.at("size").get<std::uint64_t>();
        if (!validManagedPath(file.path) || !safeAsset(file.asset) ||
            !hexSha256(file.sha256) || !names.insert(file.path).second ||
            file.size == 0 || file.size > kMaxFileBytes) {
            throw std::runtime_error("Unsafe update manifest entry");
        }
        const auto destination = managedPath(appDirectory_, file.path);
        if (std::filesystem::is_regular_file(destination) &&
            sha256File(destination) == file.sha256) continue;
        file.url = urlForManifestAsset(*selected, file.asset);
        bytes += file.size;
        if (bytes > kMaxUpdateBytes) throw std::runtime_error("Update is too large");
        changed.push_back(std::move(file));
    }
    std::lock_guard lock(mutex_);
    changed_ = std::move(changed);
    manifestText_ = manifest;
    view_.tag = tag;
    view_.totalBytes = bytes;
    view_.changedFiles = changed_.size();
    view_.phase = changed_.empty() ? UpdatePhase::UpToDate : UpdatePhase::Available;
    view_.message = changed_.empty() ? "Установлена актуальная версия"
                                    : "Доступно обновление";
}

void UpdateManager::downloadAsync() {
    {
        std::lock_guard lock(mutex_);
        if (view_.phase != UpdatePhase::Available) return;
        view_.phase = UpdatePhase::Downloading;
        view_.message = "Загрузка изменений...";
        view_.downloadedBytes = 0;
    }
    startWorker(std::thread([this] {
        try { download(); }
        catch (const std::exception& e) { setError(e.what()); }
    }));
}

void UpdateManager::download() {
    std::vector<File> files;
    std::string manifest, tag;
    {
        std::lock_guard lock(mutex_);
        files = changed_;
        manifest = manifestText_;
        tag = view_.tag;
    }
    const auto dir = dataDirectory() / "updates" / tag;
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir / "new");
    json plan = {{"schema_version", 1}, {"files", json::array()}};
    for (const auto& file : files) {
        const auto target = managedPath(dir / "new", file.path);
        std::filesystem::create_directories(target.parent_path());
        auto partial = target;
        partial += L".partial";
        std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Cannot create download file");
        std::uint64_t downloaded = 0;
        getHttps(file.url, [&](const char* data, std::size_t count) {
            downloaded += count;
            if (downloaded > file.size) throw std::runtime_error("Release asset exceeds manifest size");
            stream.write(data, static_cast<std::streamsize>(count));
            if (!stream) throw std::runtime_error("Cannot write downloaded update");
            std::lock_guard lock(mutex_);
            view_.downloadedBytes += count;
        });
        stream.close();
        if (downloaded != file.size || sha256File(partial) != file.sha256) {
            throw std::runtime_error("Downloaded file SHA-256/size mismatch: " + file.path);
        }
        std::filesystem::rename(partial, target);
        plan["files"].push_back({{"path", file.path}, {"sha256", file.sha256}});
    }
    {
        std::ofstream out(dir / "manifest.json", std::ios::binary | std::ios::trunc);
        out << manifest;
        if (!out) throw std::runtime_error("Cannot stage update manifest");
    }
    {
        std::ofstream out(dir / "plan.json", std::ios::binary | std::ios::trunc);
        out << plan.dump(2);
        if (!out) throw std::runtime_error("Cannot stage update plan");
    }
    std::lock_guard lock(mutex_);
    stageDirectory_ = dir;
    view_.phase = UpdatePhase::Prepared;
    view_.message = "Обновление загружено. Готово к установке.";
}

void UpdateManager::applyAndRestart() {
    std::filesystem::path stage;
    {
        std::lock_guard lock(mutex_);
        if (view_.phase != UpdatePhase::Prepared) {
            throw std::runtime_error("Update is not fully downloaded");
        }
        stage = stageDirectory_;
    }
    const auto helper = appDirectory_ / "reg_updater.exe";
    const auto copied = stage / "apply-update.exe";
    std::filesystem::copy_file(helper, copied,
        std::filesystem::copy_options::overwrite_existing);
    std::wstring command =
        quote(copied.wstring()) + L" --pid " +
        std::to_wstring(GetCurrentProcessId()) +
        L" --root " + quote(appDirectory_.wstring()) +
        L" --stage " + quote(stage.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(copied.c_str(), command.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
        stage.c_str(), &startup, &process)) {
        throw std::runtime_error("Cannot start update installer (Win32 " +
                                 std::to_string(GetLastError()) + ")");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

} // namespace reg::launcher
#endif
