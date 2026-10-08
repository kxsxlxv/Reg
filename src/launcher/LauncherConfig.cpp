#include "launcher/LauncherConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace reg::launcher {
namespace {
using json = nlohmann::json;

std::filesystem::path fromEnv(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0'
        ? std::filesystem::path(value)
        : std::filesystem::path{};
}

std::filesystem::path userHome() {
    auto home = fromEnv("HOME");
#ifdef _WIN32
    if (home.empty()) {
        home = fromEnv("USERPROFILE");
    }
#endif
    if (home.empty()) {
        throw std::runtime_error("Cannot determine user data directory");
    }
    return home;
}

void checkName(std::string_view name) {
    if (name.empty() || name.size() > 64U) {
        throw std::runtime_error("Profile name must have 1-64 characters");
    }
    for (const unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            throw std::runtime_error(
                "Profile name may contain only ASCII letters, digits, '_' and '-'");
        }
    }
}

json toJson(const Profile& p) {
    return {
        {"schema_version", 1},
        {"rtsp_url", p.rtspUrl},
        {"overlay_enabled", p.overlayEnabled},
        {"telemetry_enabled", p.telemetryEnabled},
        {"recorder_enabled", p.recorderEnabled},
        {"validation_enabled", p.validationEnabled},
        {"require_frame_identity", p.requireFrameIdentity},
        {"identity_probe_frames", p.identityProbeFrames},
        {"max_delay_us", p.maxDelayUs},
        {"reorder_queue_size", p.reorderQueueSize},
        {"extra_hw_frames", p.extraHwFrames},
        {"metadata_bind", p.metadataBind},
        {"metadata_port", p.metadataPort},
        {"overlay_delay_ms", p.overlayDelayMs},
        {"record_segment_ms", p.recordSegmentMs},
        {"record_retention_sec", p.recordRetentionSec},
        {"record_queue_capacity", p.recordQueueCapacity},
        {"reconnect_initial_ms", p.reconnectInitialMs},
        {"reconnect_max_ms", p.reconnectMaxMs}
    };
}

Profile fromJson(const json& j) {
    if (!j.is_object() || j.value("schema_version", 0) != 1) {
        throw std::runtime_error("Unsupported profile schema");
    }
    Profile p;
    p.rtspUrl = j.value("rtsp_url", p.rtspUrl);
    p.overlayEnabled = j.value("overlay_enabled", p.overlayEnabled);
    p.telemetryEnabled = j.value("telemetry_enabled", p.telemetryEnabled);
    p.recorderEnabled = j.value("recorder_enabled", p.recorderEnabled);
    p.validationEnabled = j.value("validation_enabled", p.validationEnabled);
    p.requireFrameIdentity = j.value("require_frame_identity", p.requireFrameIdentity);
    p.identityProbeFrames = j.value("identity_probe_frames", p.identityProbeFrames);
    p.maxDelayUs = j.value("max_delay_us", p.maxDelayUs);
    p.reorderQueueSize = j.value("reorder_queue_size", p.reorderQueueSize);
    p.extraHwFrames = j.value("extra_hw_frames", p.extraHwFrames);
    p.metadataBind = j.value("metadata_bind", p.metadataBind);
    p.metadataPort = j.value("metadata_port", p.metadataPort);
    p.overlayDelayMs = j.value("overlay_delay_ms", p.overlayDelayMs);
    p.recordSegmentMs = j.value("record_segment_ms", p.recordSegmentMs);
    p.recordRetentionSec = j.value("record_retention_sec", p.recordRetentionSec);
    p.recordQueueCapacity = j.value("record_queue_capacity", p.recordQueueCapacity);
    p.reconnectInitialMs = j.value("reconnect_initial_ms", p.reconnectInitialMs);
    p.reconnectMaxMs = j.value("reconnect_max_ms", p.reconnectMaxMs);
    return p;
}

void writeJson(const std::filesystem::path& path, const json& j) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Cannot write " + temporary.string());
        }
        out << j.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        out.flush();
        if (!out) {
            throw std::runtime_error("Failed writing " + temporary.string());
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(), "Cannot replace profile");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

std::string asText(int number) {
    return std::to_string(number);
}
} // namespace

std::filesystem::path dataDirectory() {
#ifdef _WIN32
    auto base = fromEnv("LOCALAPPDATA");
    if (base.empty()) {
        base = fromEnv("APPDATA");
    }
    return (base.empty() ? userHome() : base) / "Reg";
#else
    auto base = fromEnv("XDG_DATA_HOME");
    return (base.empty() ? userHome() / ".local" / "share" : base) / "reg";
#endif
}

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) {
        throw std::runtime_error("Cannot find executable directory");
    }
    buffer.resize(count);
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code error;
    const auto target = std::filesystem::read_symlink("/proc/self/exe", error);
    return !error ? target.parent_path() : std::filesystem::current_path();
#endif
}

std::filesystem::path profilesDirectory() {
    return dataDirectory() / "profiles";
}

std::filesystem::path profilePath(const std::string& name) {
    checkName(name);
    return profilesDirectory() / (name + ".json");
}

std::vector<std::string> listProfiles() {
    std::vector<std::string> result;
    std::error_code ec;
    if (!std::filesystem::exists(profilesDirectory(), ec)) {
        return {"default"};
    }
    for (const auto& entry : std::filesystem::directory_iterator(profilesDirectory())) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto name = entry.path().stem().string();
        try {
            checkName(name);
            result.push_back(name);
        } catch (const std::exception&) {
            // Ignore unsupported filenames.
        }
    }
    std::sort(result.begin(), result.end());
    if (result.empty()) {
        result.push_back("default");
    }
    return result;
}

Profile loadProfile(const std::string& name) {
    const auto path = profilePath(name);
    if (!std::filesystem::exists(path)) {
        if (name == "default") {
            return {};
        }
        throw std::runtime_error("Profile not found: " + name);
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open " + path.string());
    }
    Profile p = fromJson(json::parse(in));
    const auto error = validate(p);
    if (!error.empty() && !p.rtspUrl.empty()) {
        throw std::runtime_error("Invalid profile: " + error);
    }
    return p;
}

void saveProfile(const std::string& name, const Profile& profile) {
    // Allow an empty URL when configuring a profile; Start will require one.
    Profile candidate = profile;
    if (candidate.rtspUrl.empty()) {
        candidate.rtspUrl = "rtsp://placeholder";
    }
    const auto error = validate(candidate);
    if (!error.empty()) {
        throw std::runtime_error(error);
    }
    writeJson(profilePath(name), toJson(profile));
}

std::string validate(const Profile& p) {
    if (!(p.rtspUrl.starts_with("rtsp://") || p.rtspUrl.starts_with("rtsps://"))) {
        return "RTSP URL must begin with rtsp:// or rtsps://";
    }
    if (p.maxDelayUs < 0 || p.reorderQueueSize < 0 || p.extraHwFrames < 0 ||
        p.identityProbeFrames < 0 || p.overlayDelayMs < 0) {
        return "Video and latency parameters cannot be negative";
    }
    if (p.metadataPort < 1 || p.metadataPort > 65535 || p.metadataBind.empty()) {
        return "Invalid metadata bind address/port";
    }
    if (p.recordSegmentMs <= 0 || p.recordRetentionSec <= 0 ||
        static_cast<std::int64_t>(p.recordRetentionSec) * 1000 < p.recordSegmentMs ||
        p.recordQueueCapacity <= 0) {
        return "Invalid recorder segment/retention/queue settings";
    }
    if (p.reconnectInitialMs <= 0 || p.reconnectMaxMs < p.reconnectInitialMs) {
        return "Reconnect maximum must be >= positive initial delay";
    }
    return {};
}

std::vector<std::string> arguments(
    const Profile& p,
    const std::filesystem::path& recordingDirectory) {
    auto result = std::vector<std::string>{
        "--url", p.rtspUrl,
        "--max-delay-us", asText(p.maxDelayUs),
        "--reorder-queue-size", asText(p.reorderQueueSize),
        "--extra-hw-frames", asText(p.extraHwFrames),
        "--metadata-bind", p.metadataBind,
        "--metadata-port", asText(p.metadataPort),
        "--overlay-delay-ms", asText(p.overlayDelayMs),
        "--record-dir", recordingDirectory.string(),
        "--record-segment-ms", asText(p.recordSegmentMs),
        "--record-retention-sec", asText(p.recordRetentionSec),
        "--record-queue-capacity", asText(p.recordQueueCapacity),
        "--reconnect-initial-ms", asText(p.reconnectInitialMs),
        "--reconnect-max-ms", asText(p.reconnectMaxMs)
    };
    if (!p.overlayEnabled) result.push_back("--disable-overlay");
    if (!p.telemetryEnabled) result.push_back("--disable-telemetry");
    if (!p.recorderEnabled) result.push_back("--disable-recorder");
    if (!p.validationEnabled) result.push_back("--no-validation");
    if (p.requireFrameIdentity) result.push_back("--require-frame-identity");
    if (p.identityProbeFrames > 0) {
        result.push_back("--identity-probe-frames");
        result.push_back(asText(p.identityProbeFrames));
    }
    return result;
}

std::filesystem::path newSessionDirectory() {
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    const auto base = dataDirectory() / "sessions";
    std::filesystem::create_directories(base);
    for (int i = 0; i < 1000; ++i) {
        const auto path = base / (std::to_string(stamp) + "-" + std::to_string(i));
        if (std::filesystem::create_directory(path)) {
            return path;
        }
    }
    throw std::runtime_error("Could not create unique session directory");
}

std::string redactCredentials(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return url;
    const auto begin = scheme + 3;
    const auto authorityEnd = url.find_first_of("/?#", begin);
    const auto at = url.find('@', begin);
    if (at == std::string::npos || (authorityEnd != std::string::npos && at > authorityEnd)) {
        return url;
    }
    return url.substr(0, begin) + "***@" + url.substr(at + 1);
}

void saveSessionManifest(
    const std::filesystem::path& directory,
    const std::string& profileName,
    const Profile& profile) {
    auto metadata = toJson(profile);
    metadata["rtsp_url"] = redactCredentials(profile.rtspUrl);
    metadata["profile"] = profileName;
    metadata["started_unix_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    writeJson(directory / "session.json", metadata);
}

} // namespace reg::launcher
