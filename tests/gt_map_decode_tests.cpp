#include "media/GtMapDecode.hpp"

#include <zstd.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace reg::media;
void check(bool b,const char* cause) {
    if (!b) throw std::runtime_error(cause);
}
std::uint32_t fnv(const std::vector<std::uint8_t>& a) {
    std::uint32_t x=2166136261U;
    for(auto b:a) {x^=b;x*=16777619U;}
    return x;
}
std::uint32_t crc(const std::vector<std::uint8_t>& a) {
    std::uint32_t x=0xffffffffU;
    for(auto b:a) {
        x^=b;
        for(unsigned i=0;i<8;++i)x=(x>>1U)^((x&1U)?0xedb88320U:0U);
    }
    return x^0xffffffffU;
}
void setFloat(std::vector<std::uint8_t>& raw,std::size_t i,float value) {
    const auto b=std::bit_cast<std::uint32_t>(value);
    for(unsigned k=0;k<4U;++k)
        raw[i*4U+k]=static_cast<std::uint8_t>((b>>(8U*k))&0xffU);
}
int main() {
    try {
        GtMapMetadata sky;
        sky.kind=GtMapKind::Sky;
        sky.status=GtMapStatus::Encoded;
        sky.width=32;sky.height=16;
        sky.sourceWidth=64;sky.sourceHeight=32;
        sky.validityPlane=true;sky.unknownPixels=1;
        std::vector<std::uint8_t> bits(128U,0U);
        bits[0]=0xaaU;
        std::fill(bits.begin()+64,bits.end(),0xffU);
        bits[64]=0xfeU; // pixel 0 is invalid
        sky.rawBytes=bits.size();sky.checksum=fnv(bits);
        sky.codec="rle8";
        for(std::size_t i=0;i<bits.size();) {
            std::size_t n=1;
            while(n<255 && i+n<bits.size() && bits[i+n]==bits[i])++n;
            sky.encoded.push_back(static_cast<std::uint8_t>(n));
            sky.encoded.push_back(bits[i]);i+=n;
        }
        auto result=decodeGtMap(sky);
        check(result.status==GtMapStatus::Ready && result.raw &&
              *result.raw==bits,"sky RLE and validity plane exact bytes");
        auto corrupted=sky;
        corrupted.checksum^=1U;
        check(decodeGtMap(corrupted).error==GtMapDecodeError::Checksum,
              "sky FNV corruption rejected");
        corrupted=sky;corrupted.encoded[0]=0U;
        check(decodeGtMap(corrupted).error==GtMapDecodeError::Size,
              "sky zero run rejected");
        corrupted=sky;corrupted.unknownPixels=0;
        check(decodeGtMap(corrupted).error==GtMapDecodeError::Pixels,
              "sky unknown count checked");

        GtMapMetadata depth;
        depth.kind=GtMapKind::Depth;
        depth.status=GtMapStatus::Encoded;
        depth.width=32;depth.height=16;
        depth.sourceWidth=64;depth.sourceHeight=32;
        depth.codec="zstd_shuffle4";
        std::vector<std::uint8_t> raw(32U*16U*4U,0U);
        setFloat(raw,0,3.5F);
        setFloat(raw,1,100.25F);
        depth.validPixels=2;depth.minimumM=3.5;depth.maximumM=100.25;
        depth.rawBytes=raw.size();depth.checksum=crc(raw);
        std::vector<std::uint8_t> shuffled(raw.size());
        const auto n=raw.size()/4U;
        for(std::size_t i=0;i<n;++i)
            for(unsigned j=0;j<4U;++j)
                shuffled[j*n+i]=raw[4U*i+j];
        depth.encoded.resize(ZSTD_compressBound(shuffled.size()));
        const std::size_t compressed=ZSTD_compress(
            depth.encoded.data(),depth.encoded.size(),
            shuffled.data(),shuffled.size(),1);
        check(!ZSTD_isError(compressed),"zstd fixture compress");
        depth.encoded.resize(compressed);
        result=decodeGtMap(depth);
        check(result.status==GtMapStatus::Ready && result.raw &&
              *result.raw==raw,"metric optical depth unshuffle exact bytes");
        depth.checksum^=1U;
        check(decodeGtMap(depth).error==GtMapDecodeError::Checksum,
              "depth CRC mismatch");
        depth.checksum^=1U;depth.validPixels=3U;
        check(decodeGtMap(depth).error==GtMapDecodeError::Pixels,
              "valid_pixels enforced");
        depth.validPixels=2U;depth.encoded[0]^=0xffU;
        check(decodeGtMap(depth).status==GtMapStatus::Invalid,
              "malformed zstd cannot contaminate RGB");
        std::cout<<"gt_map_decode_tests: PASS\n";
    } catch(const std::exception& e) {
        std::cerr<<"gt_map_decode_tests: FAIL "<<e.what()<<'\n';
        return 1;
    }
}
