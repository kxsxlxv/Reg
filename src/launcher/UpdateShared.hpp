#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace reg::launcher {

// Only executable/runtime files in the portable bundle are managed by updates.
// User settings, sessions and recordings can never be included in a manifest.
bool validManagedPath(std::string_view path);
std::filesystem::path managedPath(
    const std::filesystem::path& root, std::string_view relative);
std::string sha256File(const std::filesystem::path& path);
std::string utf8Path(const std::filesystem::path& path);

} // namespace reg::launcher
