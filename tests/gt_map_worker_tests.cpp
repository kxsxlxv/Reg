#include "media/GtMapWorker.hpp"
#include "media/GtDisplaySelection.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace reg::media;
void check(bool value,const char* msg) {
    if(!value)throw std::runtime_error(msg);
}
std::uint32_t fnv(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t v=2166136261U;
    for(auto b:bytes){v^=b;v*=16777619U;}
    return v;
}
int main() {
    try {
        GtMapWorker worker;
        auto parts=std::make_shared<FrameGtParts>();
        parts->source=SourceFrameIdentity{
            .key={UINT64_MAX-1U,9007199254740993ULL},
            .sourceTimeNs=UINT64_MAX};
        parts->sky.status=GtMapStatus::Encoded;
        parts->sky.kind=GtMapKind::Sky;
        parts->sky.width=32U;parts->sky.height=16U;
        parts->sky.rawBytes=64U;
        parts->sky.codec="bitpack";
        parts->sky.encoded=std::vector<std::uint8_t>(64U,0xffU);
        parts->sky.checksum=fnv(parts->sky.encoded);
        check(worker.submit(parts),"submit independent compressed metadata");
        parts.reset(); // the worker owns GT only, no AVFrame/VkImage
        const FrameKey key{UINT64_MAX-1U,9007199254740993ULL};
        std::shared_ptr<const GtDecodedBundle> ready;
        const auto deadline=std::chrono::steady_clock::now()+
            std::chrono::seconds(2);
        while(!ready && std::chrono::steady_clock::now()<deadline) {
            ready=worker.find(key);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(ready && ready->sky.status==GtMapStatus::Ready &&
              ready->sky.raw && ready->sky.raw->size()==64U &&
              ready->identity.key==key,"worker decodes immutable GT by exact key");
        auto stats=worker.stats();
        check(stats.submitted==1U && stats.decodedSky==1U &&
              stats.invalidSky==0U,"worker source-owned counters");
        worker.reset();
        check(!worker.find(key) && worker.stats().sessionResets==1U,
              "new session evicts prior decoded maps");
        // A decoded GPU texture must never migrate to another video frame.
        FrameGtParts frame;
        frame.source=SourceFrameIdentity{.key={7U,42U},.sourceTimeNs=123456789U};
        frame.sky.kind=GtMapKind::Sky;
        frame.sky.status=GtMapStatus::Encoded;
        frame.sky.sourceValid=true;
        frame.sky.source=*frame.source;
        frame.sky.sourceWidth=1920;frame.sky.sourceHeight=1080;
        frame.sky.width=480;frame.sky.height=270;
        frame.depth.kind=GtMapKind::Depth;
        frame.depth.status=GtMapStatus::Encoded;
        frame.depth.sourceValid=true;
        frame.depth.source=*frame.source;
        frame.depth.sourceWidth=1920;frame.depth.sourceHeight=1080;
        frame.depth.width=480;frame.depth.height=270;
        GtDecodedBundle decoded;
        decoded.identity=*frame.source;
        decoded.sky.kind=GtMapKind::Sky;
        decoded.sky.status=GtMapStatus::Ready;
        decoded.sky.width=480;decoded.sky.height=270;
        decoded.sky.raw=std::make_shared<const std::vector<std::uint8_t>>(16200U,0xffU);
        decoded.depth.kind=GtMapKind::Depth;
        decoded.depth.status=GtMapStatus::Ready;
        decoded.depth.width=480;decoded.depth.height=270;
        decoded.depth.raw=std::make_shared<const std::vector<std::uint8_t>>(518400U,0U);
        auto selected=selectGtDisplayLayers(frame,decoded,1920,1080,true,true);
        check(selected.sky && selected.depth,"both independent scalar maps");
        decoded.identity.sourceTimeNs++;
        selected=selectGtDisplayLayers(frame,decoded,1920,1080,true,true);
        check(!selected.sky && !selected.depth,"no source timestamp reuse");
        decoded.identity=*frame.source;
        frame.sky.sourceWidth=1280U;
        selected=selectGtDisplayLayers(frame,decoded,1920,1080,true,true);
        check(!selected.sky && selected.depth,"sky raster mismatch must not suppress depth");
        frame.sky.sourceWidth=1920U;
        selected=selectGtDisplayLayers(frame,decoded,1920,1080,false,true);
        check(!selected.sky && selected.depth,"UI toggles are per-layer");
        std::cout<<"gt_map_worker_tests: PASS\n";
    } catch(const std::exception& ex) {
        std::cerr<<"gt_map_worker_tests: FAIL "<<ex.what()<<'\n';
        return 1;
    }
}
