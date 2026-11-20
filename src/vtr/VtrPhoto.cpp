#include "vtr/VtrPhoto.hpp"
#include <zstd.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace reg::vtr {
namespace {
constexpr int kWidth = 320;
constexpr int kHeight = 168;
constexpr int kPaletteBytes = 256 * 3;
constexpr int kDecodedBytes = kPaletteBytes + kWidth * kHeight;

// The authentic user-supplied UAV photo, resized for preview and reduced to
// 128 palette colours. Base64 wraps Zstd-compressed palette + pixel indexes;
// it is data, not a generated illustration. Stored here so offline tests,
// Vulkan and portable installations all render the identical photo.
constexpr std::string_view kData =
    "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";

std::unordered_map<const ImFontAtlas*, ImFontAtlasRectId> gPhotoIds;

std::vector<std::uint8_t> decode64(std::string_view data) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(data.size() * 3 / 4);
    unsigned accumulator = 0;
    int bits = -8;
    for (const unsigned char c : data) {
        if (c == '=') break; // Standard Base64 padding at the end.
        int n = -1;
        if (c >= 'A' && c <= 'Z') n = c - 'A';
        else if (c >= 'a' && c <= 'z') n = c - 'a' + 26;
        else if (c >= '0' && c <= '9') n = c - '0' + 52;
        else if (c == '+') n = 62;
        else if (c == '/') n = 63;
        if (n < 0) throw std::runtime_error("Invalid embedded VTR photo");
        accumulator = (accumulator << 6U) | static_cast<unsigned>(n);
        bits += 6;
        if (bits >= 0) {
            bytes.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 255U));
            bits -= 8;
        }
    }
    return bytes;
}
} // namespace

void installPreviewPhoto(ImFontAtlas* atlas) {
    if (atlas == nullptr) throw std::runtime_error("VTR photo font atlas null");
    const auto packed = decode64(kData);
    std::array<std::uint8_t, kDecodedBytes> decoded{};
    const size_t n = ZSTD_decompress(decoded.data(), decoded.size(),
                                     packed.data(), packed.size());
    if (ZSTD_isError(n) || n != decoded.size())
        throw std::runtime_error("VTR photo decompression failed");
    atlas->TexDesiredFormat = ImTextureFormat_RGBA32;
    atlas->TexPixelsUseColors = true;
    const ImFontAtlasRectId id = atlas->AddCustomRect(kWidth,kHeight);
    if (id == ImFontAtlasRectId_Invalid)
        throw std::runtime_error("VTR photo atlas rectangle failed");
    ImFontAtlasRect rect;
    if (!atlas->GetCustomRect(id, &rect) || atlas->TexData == nullptr ||
        atlas->TexData->Format != ImTextureFormat_RGBA32)
        throw std::runtime_error("VTR photo texture format unavailable");
    for (int y = 0; y < kHeight; ++y) {
        auto* destination = static_cast<std::uint8_t*>(
            atlas->TexData->GetPixelsAt(rect.x,rect.y+y));
        for (int x = 0; x < kWidth; ++x) {
            const unsigned index = decoded[kPaletteBytes + y*kWidth + x];
            for (int channel = 0; channel < 3; ++channel)
                destination[4*x + channel] = decoded[3*index + channel];
            destination[4*x + 3] = 255;
        }
    }
    gPhotoIds[atlas] = id;
}

bool previewPhoto(PreviewPhoto& photo) {
    const ImFontAtlas* atlas=ImGui::GetIO().Fonts;
    const auto found=gPhotoIds.find(atlas);
    if(found==gPhotoIds.end()) return false;
    ImFontAtlasRect rect;
    if(!atlas->GetCustomRect(found->second,&rect)) return false;
    photo.texture=atlas->TexRef;
    photo.uv0=rect.uv0;
    photo.uv1=rect.uv1;
    photo.width=rect.w;
    photo.height=rect.h;
    return true;
}
} // namespace reg::vtr