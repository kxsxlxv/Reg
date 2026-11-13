#include "render/Cvm1SemanticStyleConfig.hpp"
#include "render/Cvm1SemanticStyle.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace reg::render::cvm1;

int main() {
    const auto path = std::filesystem::path("config") / "gt-range-ui.json";
    std::filesystem::create_directories(path.parent_path());
    const bool existed = std::filesystem::exists(path);
    const auto backup = std::filesystem::path("cvm1-test-config-backup.json");
    if (existed) std::filesystem::copy_file(
        path, backup, std::filesystem::copy_options::overwrite_existing);
    try {
        {
            std::ofstream out(path, std::ios::trunc);
            out << R"({"version":2,"styles":{"detection":{"color":"#112233","width":6,"pattern":"dashed","marker_size":10,"label":"custom_label","visible":false}}})";
        }
        refreshConfig();
        auto s = current()->style(kDetector);
        assert(s.width == 6.0F && s.markerSize == 10.0F);
        assert(!s.visible && s.label == "custom_label");
        assert(s.pattern == Pattern::dashed);
        assert(s.color.r > 0.065F && s.color.r < 0.068F);

        std::this_thread::sleep_for(std::chrono::milliseconds(340));
        {
            std::ofstream out(path, std::ios::trunc);
            out << R"({"version":2,"styles":{"detection":{"color":"#F5C542","width":3,"visible":true}}})";
        }
        refreshConfig();
        s = current()->style(kDetector);
        assert(s.width == 3.0F && s.visible);

        std::this_thread::sleep_for(std::chrono::milliseconds(340));
        {
            std::ofstream out(path, std::ios::trunc);
            out << "{ broken";
        }
        refreshConfig();
        assert(current()->style(kDetector).width == 3.0F);
        if (existed) {
            std::filesystem::copy_file(
                backup, path, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove(backup);
        } else {
            std::filesystem::remove(path);
        }
        std::cout << "CVM1 style hot reload: PASS\n";
        return 0;
    } catch (...) {
        if (existed && std::filesystem::exists(backup)) {
            std::filesystem::copy_file(
                backup, path, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove(backup);
        }
        throw;
    }
}
