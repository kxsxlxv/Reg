#include "launcher/UpdateShared.hpp"

#include <array>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/sha.h>
#endif

namespace reg::launcher {
namespace {
bool safeComponent(std::string_view segment) {
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.back() == '.' || segment.back() == ' ') return false;
    for (const unsigned char c : segment) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

bool endsWith(std::string_view path, std::string_view suffix) {
    return path.size() > suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
}

std::string hexBytes(const unsigned char* data, std::size_t count) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(alphabet[data[i] >> 4]);
        result.push_back(alphabet[data[i] & 0x0f]);
    }
    return result;
}
} // namespace

bool validManagedPath(std::string_view path) {
    if (path.empty() || path.size() > 240 || path.front() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) return false;
    std::size_t offset = 0;
    for (;;) {
        const auto slash = path.find('/', offset);
        const auto segment = path.substr(offset, slash == std::string_view::npos
                                                  ? slash : slash - offset);
        if (!safeComponent(segment)) return false;
        if (slash == std::string_view::npos) break;
        offset = slash + 1;
    }
    if (path == "Launcher.exe" || path == "reg_probe.exe" ||
        path == "reg_replay.exe" || path == "reg_updater.exe") return true;
    if (path.find('/') == std::string_view::npos && endsWith(path, ".dll")) return true;
    if (path.starts_with("fonts/") && endsWith(path, ".ttf") &&
        path.find('/', 6) == std::string_view::npos) return true;
    if (path.starts_with("shaders/") && endsWith(path, ".spv") &&
        path.find('/', 8) == std::string_view::npos) return true;
    return false;
}

std::filesystem::path managedPath(
    const std::filesystem::path& root, std::string_view relative) {
    if (!validManagedPath(relative)) {
        throw std::runtime_error("Unsafe managed file path in update manifest");
    }
    return root / std::filesystem::u8path(relative.begin(), relative.end());
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string sha256File(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open update file: " + utf8Path(path));
    std::array<char, 64 * 1024> buffer{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE provider{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("BCrypt SHA256 unavailable");
    }
    if (BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(provider, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }
    try {
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = in.gcount();
            if (count && BCryptHashData(hash,
                    reinterpret_cast<PUCHAR>(buffer.data()),
                    static_cast<ULONG>(count), 0) < 0) {
                throw std::runtime_error("BCryptHashData failed");
            }
        }
        if (!in.eof()) throw std::runtime_error("Cannot read update file");
        std::array<unsigned char, 32> bytes{};
        if (BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) {
            throw std::runtime_error("BCryptFinishHash failed");
        }
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(provider, 0);
        return hexBytes(bytes.data(), bytes.size());
    } catch (...) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(provider, 0);
        throw;
    }
#else
    SHA256_CTX context;
    SHA256_Init(&context);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        if (count) SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(count));
    }
    if (!in.eof()) throw std::runtime_error("Cannot read update file");
    std::array<unsigned char, 32> bytes{};
    SHA256_Final(bytes.data(), &context);
    return hexBytes(bytes.data(), bytes.size());
#endif
}

} // namespace reg::launcher
