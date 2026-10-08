#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace reg::launcher {

struct Profile {
    std::string rtspUrl;
    bool overlayEnabled{true};
    bool telemetryEnabled{true};
    bool recorderEnabled{true};
    bool validationEnabled{false};
    bool requireFrameIdentity{false};
    int identityProbeFrames{0};
    int maxDelayUs{0};
    int reorderQueueSize{0};
    int extraHwFrames{32};
    // Zero keeps the original automatic role-to-monitor mapping.
    int rawDisplay{0};
    int overlayDisplay{0};
    int telemetryDisplay{0};
    bool netImguiEnabled{true};
    int netImguiPort{8888};
    std::string metadataBind{"0.0.0.0"};
    int metadataPort{50010};
    int overlayDelayMs{150};
    int recordSegmentMs{5000};
    int recordRetentionSec{300};
    int recordQueueCapacity{2048};
    int reconnectInitialMs{250};
    int reconnectMaxMs{2000};
};

std::filesystem::path dataDirectory();
std::filesystem::path executableDirectory();
std::filesystem::path profilesDirectory();
std::filesystem::path profilePath(const std::string& name);
std::vector<std::string> listProfiles();
std::string lastSelectedProfile();
void setLastSelectedProfile(const std::string& name);
Profile loadProfile(const std::string& name);
void saveProfile(const std::string& name, const Profile& profile);
std::string validate(const Profile& profile);
std::vector<std::string> arguments(
    const Profile& profile,
    const std::filesystem::path& recordingDirectory);
std::filesystem::path newSessionDirectory();
std::string redactCredentials(const std::string& url);
void saveSessionManifest(
    const std::filesystem::path& directory,
    const std::string& profileName,
    const Profile& profile);

} // namespace reg::launcher
