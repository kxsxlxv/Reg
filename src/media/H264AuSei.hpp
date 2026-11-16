#pragma once

#include "media/FrameIdentity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace reg::media {

// AU-level unregistered SEI transport. This parser intentionally does not
// depend on FFmpeg, JSON, a decoder, or a display surface.
enum class AuNalFraming : std::uint8_t { AnnexB, Avcc1, Avcc2, Avcc4 };
enum class AuSeiKind : std::uint8_t { Rgf1, Kgt1, Ksm1, Kdm1, Unknown };
inline constexpr std::size_t kMaxH264AuBytes = 8U * 1024U * 1024U;
inline constexpr std::array<std::uint8_t, 16> kSkyMaskSeiUuid{
    0xa1,0x60,0x1e,0x83,0x61,0xaf,0x44,0xdf,
    0x91,0x1e,0x78,0x64,0x73,0x6b,0x79,0x31};
inline constexpr std::array<std::uint8_t, 16> kDepthMapSeiUuid{
    0x30,0x0b,0x72,0xc7,0x05,0xa8,0x47,0x29,
    0xa0,0xb3,0x5d,0x38,0xa9,0xf9,0xc1,0x42};
inline constexpr std::array<std::uint8_t, 16> kKgt1SeiUuid{
    0x62,0x42,0xb3,0x58,0xd3,0x0d,0x48,0x4a,
    0x9e,0xf7,0x5e,0x3f,0x9c,0x24,0x1b,0x07};

struct H264AuSei {
    // Store post-UUID payload bytes; each known field is independent.
    std::array<std::optional<std::vector<std::uint8_t>>, 4> payloads;
    std::array<bool, 4> conflicts{};
    std::uint32_t byteIdenticalDuplicates{};
    std::uint32_t unknownUuidMessages{};
    std::uint32_t seiMessages{};
    bool malformed{};      // complete AU could not be safely walked
    bool oversized{};      // >8 MiB transport envelope

    [[nodiscard]] const std::optional<std::vector<std::uint8_t>>&
    get(AuSeiKind kind) const noexcept {
        return payloads[static_cast<std::size_t>(kind)];
    }
};

namespace au_sei_detail {
inline bool startCode(std::span<const std::uint8_t> data,
                      std::size_t offset, std::size_t& size) noexcept {
    size = 0;
    if (offset + 3U > data.size() || data[offset] != 0U ||
        data[offset+1U] != 0U) return false;
    if (data[offset+2U] == 1U) { size = 3U; return true; }
    if (offset+4U <= data.size() &&
        data[offset+2U] == 0U && data[offset+3U] == 1U) {
        size = 4U; return true;
    }
    return false;
}
inline bool nal(std::span<const std::uint8_t> nalu, H264AuSei& out) {
    if (nalu.empty() || (nalu[0] & 0x1fU) != 6U) return !nalu.empty();
    // H.264 emulation prevention is required before interpreting SEI sizes.
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(nalu.size() - 1U);
    unsigned zeros = 0;
    for (std::size_t i=1U; i<nalu.size(); ++i) {
        const auto byte = nalu[i];
        if (zeros == 2U && byte == 3U) {
            if (i+1U == nalu.size() || nalu[i+1U] > 3U) return false;
            zeros = 0; continue;
        }
        if (zeros >= 2U && byte <= 3U) return false; // forbidden unescaped pattern
        rbsp.push_back(byte);
        zeros = byte == 0U ? std::min(zeros + 1U, 2U) : 0U;
    }
    std::size_t pos = 0;
    while (pos < rbsp.size()) {
        // rbsp_trailing_bits = one 1 bit, followed by zero bits.
        if (rbsp[pos] == 0x80U) {
            return std::all_of(rbsp.begin()+static_cast<std::ptrdiff_t>(pos+1U),
                               rbsp.end(), [](std::uint8_t v){return v==0U;});
        }
        std::size_t type=0U,size=0U;
        for (std::size_t* field : {&type,&size}) {
            while (pos < rbsp.size() && rbsp[pos] == 0xffU) {
                if (*field > kMaxH264AuBytes-255U) return false;
                *field += 255U; ++pos;
            }
            if (pos == rbsp.size()) return false;
            if (*field > kMaxH264AuBytes - rbsp[pos]) return false;
            *field += rbsp[pos++];
        }
        if (size > rbsp.size()-pos) return false;
        if (type == 5U) {
            ++out.seiMessages;
            if (size < 16U) return false;
            const auto* uuid = rbsp.data()+pos;
            AuSeiKind kind = AuSeiKind::Unknown;
            for (std::size_t k=0; k<4U; ++k) {
                const auto match = [uuid](const auto& value) {
                    return std::equal(value.begin(),value.end(),uuid);
                };
                if ((k==0U && match(kFrameIdentitySeiUuid)) ||
                    (k==1U && match(kKgt1SeiUuid)) ||
                    (k==2U && match(kSkyMaskSeiUuid)) ||
                    (k==3U && match(kDepthMapSeiUuid))) {
                    kind=static_cast<AuSeiKind>(k);break;
                }
            }
            if (kind==AuSeiKind::Unknown) {
                ++out.unknownUuidMessages;
            } else {
                const auto k=static_cast<std::size_t>(kind);
                if (!out.conflicts[k]) {
                    std::vector<std::uint8_t> payload(
                        rbsp.begin()+static_cast<std::ptrdiff_t>(pos+16U),
                        rbsp.begin()+static_cast<std::ptrdiff_t>(pos+size));
                    auto& slot=out.payloads[k];
                    if (!slot) slot=std::move(payload);
                    else if (*slot==payload) ++out.byteIdenticalDuplicates;
                    else {slot.reset();out.conflicts[k]=true;}
                }
            }
        }
        pos+=size;
    }
    return false; // no SEI trailing bits
}
} // namespace au_sei_detail

inline H264AuSei parseH264AccessUnitSei(
    std::span<const std::uint8_t> bytes, AuNalFraming framing) noexcept {
    H264AuSei out;
    if (bytes.size() > kMaxH264AuBytes) {out.oversized=true;return out;}
    try {
        if (framing==AuNalFraming::AnnexB) {
            std::size_t prefix=0U;
            std::size_t cursor=0U;
            while (cursor<bytes.size() &&
                   !au_sei_detail::startCode(bytes,cursor,prefix)) ++cursor;
            if (cursor==bytes.size()) {out.malformed=true;return out;}
            // Annex-B byte_stream_nal_unit allows leading_zero_8bits.
            while (cursor<bytes.size()) {
                const std::size_t body=cursor+prefix;
                if (body>=bytes.size()) {out.malformed=true;break;}
                std::size_t next=body,nextPrefix=0U;
                while (next<bytes.size() &&
                       !au_sei_detail::startCode(bytes,next,nextPrefix)) ++next;
                std::size_t end=next;
                if (next==bytes.size()) {
                    while (end>body && bytes[end-1U]==0U) --end; // trailing_zero_8bits
                }
                if (end<=body || !au_sei_detail::nal(bytes.subspan(body,end-body),out)) {
                    out.malformed=true;break;
                }
                cursor=next;prefix=nextPrefix;
            }
        } else {
            const std::size_t width=framing==AuNalFraming::Avcc1?1U:
                                    framing==AuNalFraming::Avcc2?2U:4U;
            std::size_t pos=0U;
            while (pos<bytes.size()) {
                if (bytes.size()-pos<width) {out.malformed=true;break;}
                std::size_t count=0U;
                for (std::size_t n=0; n<width; ++n)
                    count=(count<<8U)|bytes[pos+n];
                pos+=width;
                if (!count || count>bytes.size()-pos) {out.malformed=true;break;}
                if (!au_sei_detail::nal(bytes.subspan(pos,count),out)) {
                    out.malformed=true;break;
                }
                pos+=count;
            }
        }
    } catch (...) {out.malformed=true;}
    if (out.malformed) out.payloads={};
    return out;
}

} // namespace reg::media
