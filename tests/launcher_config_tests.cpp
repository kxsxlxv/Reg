#include "launcher/LauncherConfig.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main() {
    namespace fs = std::filesystem;
    using namespace reg::launcher;
    const auto root = fs::temp_directory_path() / "reg-launcher-test-profile";
    fs::remove_all(root);
    fs::create_directories(root);
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", root.string().c_str());
#else
    setenv("XDG_DATA_HOME", root.string().c_str(), 1);
#endif
    assert(!profilePath("default").empty());
    assert(!validate(Profile{}).empty());
    Profile profile;
    profile.rtspUrl = "rtsp://user:password@localhost:8554/demo";
    profile.overlayDelayMs = 125;
    profile.metadataPort = 50011;
    profile.telemetryEnabled = false;
    profile.recordRetentionSec = 600;
    assert(validate(profile).empty());
    saveProfile("test_01", profile);
    const auto loaded = loadProfile("test_01");
    assert(loaded.rtspUrl == profile.rtspUrl);
    assert(loaded.overlayDelayMs == 125);
    assert(loaded.metadataPort == 50011);
    assert(!loaded.telemetryEnabled);
    assert(loaded.recordRetentionSec == 600);
    const auto names = listProfiles();
    assert(std::find(names.begin(), names.end(), "test_01") != names.end());
    const auto recordingDir = root / "some path" / "blackbox";
    const auto args = arguments(loaded, recordingDir);
    assert(std::find(args.begin(), args.end(), "--disable-telemetry") != args.end());
    assert(std::find(args.begin(), args.end(), recordingDir.string()) != args.end());
    assert(redactCredentials(profile.rtspUrl) == "rtsp://***@localhost:8554/demo");
    const auto session1 = newSessionDirectory();
    const auto session2 = newSessionDirectory();
    assert(session1 != session2);
    saveSessionManifest(session1, "test_01", loaded);
    assert(fs::exists(session1 / "session.json"));
    std::ifstream manifest(session1 / "session.json");
    std::string content((std::istreambuf_iterator<char>(manifest)),
                         std::istreambuf_iterator<char>());
    assert(content.find("password") == std::string::npos);
    assert(content.find("***@") != std::string::npos);
    manifest.close(); // Windows prevents deletion of an open file.
    bool invalidNameRejected = false;
    try { saveProfile("../escape", profile); }
    catch (const std::exception&) { invalidNameRejected = true; }
    assert(invalidNameRejected);
    profile.reconnectMaxMs = 10;
    assert(!validate(profile).empty());
    fs::remove_all(root);
    std::cout << "launcher config tests passed\n";
}
