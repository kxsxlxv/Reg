#include "media/H264AuSei.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace reg::media;

void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
void scalar(std::vector<std::uint8_t>& out, std::size_t n) {
    while (n>=255U) {out.push_back(255U);n-=255U;}
    out.push_back(static_cast<std::uint8_t>(n));
}
std::vector<std::uint8_t> sei(const std::array<std::uint8_t,16>& uuid,
                              const std::vector<std::uint8_t>& body,
                              AuNalFraming framing=AuNalFraming::AnnexB) {
    std::vector<std::uint8_t> rbsp;
    scalar(rbsp,5U);scalar(rbsp,16U+body.size());
    rbsp.insert(rbsp.end(),uuid.begin(),uuid.end());
    rbsp.insert(rbsp.end(),body.begin(),body.end());
    rbsp.push_back(0x80U);
    std::vector<std::uint8_t> nalu{0x06U};
    unsigned zeros=0;
    for (const auto b:rbsp) {
        if (zeros>=2U && b<=3U) {nalu.push_back(3U);zeros=0;}
        nalu.push_back(b);
        zeros=b==0U ? zeros+1U:0U;
    }
    std::vector<std::uint8_t> result;
    if (framing==AuNalFraming::AnnexB) result={0U,0U,0U,1U};
    else {
        const auto len=framing==AuNalFraming::Avcc1?1U:
                       framing==AuNalFraming::Avcc2?2U:4U;
        for (std::size_t i=len;i>0U;--i)
            result.push_back(static_cast<std::uint8_t>(
                (nalu.size() >> (8U*(i-1U))) & 255U));
    }
    result.insert(result.end(),nalu.begin(),nalu.end());
    return result;
}
void join(std::vector<std::uint8_t>& target,
          const std::vector<std::uint8_t>& fragment) {
    target.insert(target.end(),fragment.begin(),fragment.end());
}
int main() {
    try {
        const std::vector<std::uint8_t> rgf{'R','G','F','1',1,0,32,0,0,0,0,0};
        const std::vector<std::uint8_t> kgt{'{','}',0,0,1,2,3,4};
        std::vector<std::uint8_t> sky(724U,0U);
        sky[0]='K';sky[1]='S';sky[2]='M';sky[3]='1';
        std::vector<std::uint8_t> depth(240000U,0U);
        depth[0]='K';depth[1]='D';depth[2]='M';depth[3]='1';
        // Real 480x270 depth compressed SEIs exceed 64 KiB; no truncation.
        for(std::size_t i=4U;i<depth.size();i+=257U) depth[i]=3U;
        std::vector<std::uint8_t> au;
        join(au,sei(kFrameIdentitySeiUuid,rgf));
        join(au,sei(kKgt1SeiUuid,kgt));
        join(au,sei(kSkyMaskSeiUuid,sky));
        join(au,sei(kDepthMapSeiUuid,depth));
        join(au,sei(kDepthMapSeiUuid,depth)); // byte-identical duplicate
        // An unrelated UUID must not override any known field.
        join(au,sei(std::array<std::uint8_t,16>{},std::vector<std::uint8_t>{1U,2U}));
        const auto parsed=parseH264AccessUnitSei(au,AuNalFraming::AnnexB);
        check(!parsed.malformed && !parsed.oversized,"Annex-B parse");
        check(parsed.seiMessages==6U && parsed.byteIdenticalDuplicates==1U &&
              parsed.unknownUuidMessages==1U,"all SEI messages preserved");
        check(parsed.get(AuSeiKind::Kdm1) &&
              *parsed.get(AuSeiKind::Kdm1)==depth,"240KB depth intact");
        check(parsed.get(AuSeiKind::Ksm1) &&
              *parsed.get(AuSeiKind::Ksm1)==sky,"sky intact");
        check(parsed.get(AuSeiKind::Rgf1) &&
              *parsed.get(AuSeiKind::Rgf1)==rgf,"RGF intact");
        check(parsed.get(AuSeiKind::Kgt1) &&
              *parsed.get(AuSeiKind::Kgt1)==kgt,"KGT intact");
        // Multiple same UUID with different content are independently invalid.
        join(au,sei(kDepthMapSeiUuid,std::vector<std::uint8_t>{9U}));
        const auto conflict=parseH264AccessUnitSei(au,AuNalFraming::AnnexB);
        check(conflict.conflicts[3] && !conflict.get(AuSeiKind::Kdm1) &&
              conflict.get(AuSeiKind::Kgt1),"conflicting duplicate isolation");

        for(const auto mode:{AuNalFraming::Avcc1,AuNalFraming::Avcc2,
                             AuNalFraming::Avcc4}) {
            std::vector<std::uint8_t> framed;
            join(framed,sei(kFrameIdentitySeiUuid,rgf,mode));
            join(framed,sei(kKgt1SeiUuid,kgt,mode));
            const auto read=parseH264AccessUnitSei(framed,mode);
            check(!read.malformed && read.get(AuSeiKind::Rgf1) &&
                  read.get(AuSeiKind::Kgt1),"length-prefix framing");
        }
        auto truncated=sei(kDepthMapSeiUuid,depth,AuNalFraming::Avcc4);
        truncated.pop_back();
        check(parseH264AccessUnitSei(truncated,AuNalFraming::Avcc4).malformed,
              "truncated AVCC rejected");
        const std::vector<std::uint8_t> badSei{0,0,1,6,5,0xff,0x80};
        check(parseH264AccessUnitSei(badSei,AuNalFraming::AnnexB).malformed,
              "truncated extended payload rejected");
        std::vector<std::uint8_t> huge(kMaxH264AuBytes+1U,0x55U);
        check(parseH264AccessUnitSei(huge,AuNalFraming::AnnexB).oversized,
              "AU size limit");
        std::cout<<"h264_au_sei_tests: PASS\n";
    } catch(const std::exception& ex) {
        std::cerr<<"h264_au_sei_tests: FAIL "<<ex.what()<<'\n';
        return 1;
    }
}
