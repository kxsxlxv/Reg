#include "launcher/UpdateShared.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    using reg::launcher::validManagedPath;
    assert(validManagedPath("Launcher.exe"));
    assert(validManagedPath("reg_probe.exe"));
    assert(validManagedPath("reg_replay.exe"));
    assert(validManagedPath("reg_updater.exe"));
    assert(validManagedPath("SDL3.dll"));
    assert(validManagedPath("fonts/Roboto.ttf"));
    assert(validManagedPath("shaders/video.vert.spv"));
    for (const auto* candidate : {
        "../Launcher.exe", "/Launcher.exe", "D:/Windows/system.ini",
        "fonts/../../settings.json", "fonts/evil.ttf/other",
        "shaders\\video.spv", "sessions/session.json", "profile.json",
        "contrib/../x.dll", "evil.exe", "fonts/sub/font.ttf",
        "evil.dll/", "Launcher.exe:stream"}) {
        assert(!validManagedPath(candidate));
    }

    const auto dir = std::filesystem::temp_directory_path() / "launcher-sha-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "test.txt";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "abc";
    }
    assert(reg::launcher::sha256File(file) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::filesystem::remove(file);
    std::filesystem::remove(dir);
}
