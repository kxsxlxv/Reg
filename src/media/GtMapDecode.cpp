#include "media/GtMapDecode.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace reg::media {
namespace {

std::uint32_t fnv32(const std::vector<std::uint8_t>& bytes) noexcept {
    std::uint32_t h=2166136261U;
    for(const auto byte:bytes) {h^=byte;h*=16777619U;}
    return h;
}
// Table-driven standard reflected CRC32 (zlib polynomial). A 480x270
// float32 depth frame contains 518400 bytes: the old bit-at-a-time loop
// executed more than four million CRC rounds per frame on the worker.
constexpr auto makeCrc32Table() noexcept {
    std::array<std::uint32_t,256> table{};
    for(std::uint32_t i=0;i<256U;++i) {
        auto crc=i;
        for(unsigned bit=0;bit<8U;++bit)
            crc=(crc>>1U)^((crc&1U)?0xedb88320U:0U);
        table[i]=crc;
    }
    return table;
}
std::uint32_t crc32(const std::vector<std::uint8_t>& bytes) noexcept {
    static constexpr auto table=makeCrc32Table();
    std::uint32_t value=0xffffffffU;
    for(const auto byte:bytes)
        value=table[(value^byte)&0xffU]^(value>>8U);
    return value^0xffffffffU;
}
bool bit(const std::vector<std::uint8_t>& bytes,std::size_t index) noexcept {
    return (bytes[index/8U] & (1U<<(index%8U)))!=0U;
}
float readLeFloat(const std::uint8_t* bytes) noexcept {
    static_assert(sizeof(float)==4U);
    const auto bits=std::uint32_t(bytes[0]) |
       (std::uint32_t(bytes[1])<<8U) |
       (std::uint32_t(bytes[2])<<16U) |
       (std::uint32_t(bytes[3])<<24U);
    return std::bit_cast<float>(bits);
}
} // namespace

GtDecodedMap decodeGtMap(const GtMapMetadata& map) noexcept {
    GtDecodedMap result;
    result.status=map.status;
    result.kind=map.kind;
    result.width=map.width;result.height=map.height;
    if(map.status!=GtMapStatus::Encoded) {
        result.error=GtMapDecodeError::NotEncoded;
        return result;
    }
    const auto start=std::chrono::steady_clock::now();
    auto fail=[&](GtMapDecodeError error) {
        result.status=GtMapStatus::Invalid;
        result.error=error;
        result.raw.reset();
        result.durationUs=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-start).count());
        return result;
    };
    const auto pixels=std::uint64_t(map.width)*map.height;
    if(map.width<32U || map.width>1920U ||
       map.height<16U || map.height>1080U ||
       !map.rawBytes || map.rawBytes>8U*1024U*1024U ||
       pixels>2073600U)
        return fail(GtMapDecodeError::Bounds);
    try {
        auto raw=std::make_shared<std::vector<std::uint8_t>>();
        raw->reserve(static_cast<std::size_t>(map.rawBytes));
        if(map.kind==GtMapKind::Sky) {
            if(map.codec=="bitpack") *raw=map.encoded;
            else if(map.codec=="rle8") {
                if(map.encoded.size()%2U!=0U) return fail(GtMapDecodeError::Size);
                for(std::size_t i=0;i<map.encoded.size();i+=2U) {
                    const auto count=map.encoded[i];
                    if(count==0U || count>map.rawBytes-raw->size())
                        return fail(GtMapDecodeError::Size);
                    raw->insert(raw->end(),count,map.encoded[i+1U]);
                }
            } else return fail(GtMapDecodeError::Codec);
            const auto planeSize=(pixels+7U)/8U;
            if(map.rawBytes!=planeSize*(map.validityPlane?2U:1U) ||
               raw->size()!=map.rawBytes) return fail(GtMapDecodeError::Size);
            if(fnv32(*raw)!=map.checksum) return fail(GtMapDecodeError::Checksum);
            std::uint64_t unknown=0;
            if(map.validityPlane) {
                const std::size_t offset=static_cast<std::size_t>(planeSize*8U);
                for(std::size_t pixel=0;pixel<static_cast<std::size_t>(pixels);++pixel)
                    if(!bit(*raw,offset+pixel)) ++unknown;
            }
            if(unknown!=map.unknownPixels) return fail(GtMapDecodeError::Pixels);
        } else {
            if(map.rawBytes!=pixels*4U) return fail(GtMapDecodeError::Size);
            if(map.codec=="raw") *raw=map.encoded;
            else if(map.codec=="zstd_shuffle4") {
                std::vector<std::uint8_t> shuffled(
                    static_cast<std::size_t>(map.rawBytes));
                const std::size_t n=ZSTD_decompress(
                    shuffled.data(),shuffled.size(),
                    map.encoded.data(),map.encoded.size());
                if(ZSTD_isError(n) || n!=map.rawBytes)
                    return fail(GtMapDecodeError::Codec);
                raw->resize(shuffled.size());
                const auto count=static_cast<std::size_t>(pixels);
                for(std::size_t i=0;i<count;++i)
                    for(unsigned b=0;b<4U;++b)
                        (*raw)[i*4U+b]=shuffled[b*count+i];
            } else return fail(GtMapDecodeError::Codec);
            if(raw->size()!=map.rawBytes) return fail(GtMapDecodeError::Size);
            if(crc32(*raw)!=map.checksum) return fail(GtMapDecodeError::Checksum);
            std::uint64_t valid=0;
            double min=std::numeric_limits<double>::infinity();
            double max=0;
            for(std::size_t i=0;i<static_cast<std::size_t>(pixels);++i) {
                const float z=readLeFloat(raw->data()+4U*i);
                if(!std::isfinite(z) || z<0.0F) return fail(GtMapDecodeError::Pixels);
                if(z>0.0F) {
                    ++valid;
                    min=std::min(min,static_cast<double>(z));
                    max=std::max(max,static_cast<double>(z));
                }
            }
            if(valid!=map.validPixels) return fail(GtMapDecodeError::Pixels);
            if(!valid) {min=0;max=0;}
            const auto close=[](double a,double b) {
                return std::fabs(a-b)<=std::max(0.0001,0.000001*std::max(a,b));
            };
            if(!close(min,map.minimumM) || !close(max,map.maximumM))
                return fail(GtMapDecodeError::Pixels);
        }
        result.raw=std::move(raw);
        result.status=GtMapStatus::Ready;
        result.durationUs=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-start).count());
        return result;
    } catch (...) {return fail(GtMapDecodeError::Allocation);}
}

} // namespace reg::media
