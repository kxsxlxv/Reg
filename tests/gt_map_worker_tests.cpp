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
        // Regression: the AU worker can be 40+ frames ahead of Vulkan
        // decoded video. The old 16-frame cache silently evicted every
        // correctly decoded map before Raw presentation at ~60 FPS.
        std::shared_ptr<GtMapResultSlot> firstFrameSlot;
        for (std::uint64_t n=0;n<80U;++n) {
            auto ahead=std::make_shared<FrameGtParts>();
            ahead->source=SourceFrameIdentity{
                .key={9U,1000U+n},.sourceTimeNs=1000000U+n};
            ahead->decodedMaps=std::make_shared<GtMapResultSlot>();
            if(n==0U)firstFrameSlot=ahead->decodedMaps;
            ahead->sky.kind=GtMapKind::Sky;
            ahead->sky.status=GtMapStatus::Encoded;
            ahead->sky.width=32U;ahead->sky.height=16U;
            ahead->sky.rawBytes=64U;
            ahead->sky.codec="bitpack";
            ahead->sky.encoded=std::vector<std::uint8_t>(64U,0xa5U);
            ahead->sky.checksum=fnv(ahead->sky.encoded);
            check(worker.submit(ahead),"ahead-of-RGB map submitted");
            const auto until=std::chrono::steady_clock::now()+
                std::chrono::seconds(2);
            while(!worker.find(ahead->source->key) &&
                  std::chrono::steady_clock::now()<until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(static_cast<bool>(worker.find(ahead->source->key)),
                  "map available after asynchronous decode");
        }
        check(static_cast<bool>(worker.find(FrameKey{9U,1000U})),
              "oldest of 80 forward-decoded masks remains for delayed RGB");
        check(static_cast<bool>(worker.find(FrameKey{9U,1079U})),
              "newest mask remains available too");
        check(firstFrameSlot &&
              firstFrameSlot->ready.load(std::memory_order_acquire) &&
              firstFrameSlot->ready.load()->identity.key==FrameKey{9U,1000U},
              "decoded result attaches to its original GT AU slot");
        check(!worker.find(FrameKey{9U,99999U}),"missing key lookup test");
        const auto aheadStats=worker.stats();
        check(aheadStats.readyFrames>=80U &&
              aheadStats.cacheEvictedCapacity==0U &&
              aheadStats.lookupHits>0U &&
              aheadStats.lookupMisses>0U,
              "cache and lookup diagnostics across 80 GT frames");
        // Force eviction from the global cache while the original
        // VideoFrame keeps its own same-AU GT slot alive.
        for(std::uint64_t n=80U;n<165U;++n) {
            auto ahead=std::make_shared<FrameGtParts>();
            ahead->source=SourceFrameIdentity{
                .key={9U,1000U+n},.sourceTimeNs=1000000U+n};
            ahead->sky.kind=GtMapKind::Sky;
            ahead->sky.status=GtMapStatus::Encoded;
            ahead->sky.width=32U;ahead->sky.height=16U;
            ahead->sky.rawBytes=64U;
            ahead->sky.codec="bitpack";
            ahead->sky.encoded=std::vector<std::uint8_t>(64U,0xa5U);
            ahead->sky.checksum=fnv(ahead->sky.encoded);
            check(worker.submit(ahead),"more maps submitted");
            const auto until=std::chrono::steady_clock::now()+
                std::chrono::seconds(2);
            while(!worker.find(ahead->source->key) &&
                  std::chrono::steady_clock::now()<until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(static_cast<bool>(worker.find(ahead->source->key)),
                  "further ahead maps decoded");
        }
        check(!worker.find(FrameKey{9U,1000U}),
              "old global cache entry evicted by capacity");
        auto frameLinkedResult=firstFrameSlot->ready.load(std::memory_order_acquire);
        check(frameLinkedResult &&
              frameLinkedResult->identity.key==FrameKey{9U,1000U} &&
              frameLinkedResult->sky.status==GtMapStatus::Ready,
              "same-AU decoded map survives global cache eviction");
        check(worker.stats().cacheEvictedCapacity>0U,
              "global cache eviction is exercised");
        worker.reset();
        // A decoded GPU texture must never migrate to another video frame.
        FrameGtParts frame;
        frame.source=SourceFrameIdentity{.key={7U,42U},.sourceTimeNs=123456789U};
        frame.sky.kind=GtMapKind::Sky;
        frame.sky.status=GtMapStatus::Encoded;
        frame.sky.sourceValid=true;
        frame.sky.source=*frame.source;
        frame.sky.sourceWidth=2112;frame.sky.sourceHeight=1188;
        frame.sky.width=480;frame.sky.height=270;
        frame.depth.kind=GtMapKind::Depth;
        frame.depth.status=GtMapStatus::Encoded;
        frame.depth.sourceValid=true;
        frame.depth.source=*frame.source;
        frame.depth.sourceWidth=2112;frame.depth.sourceHeight=1188;
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
        check(selected.sky && selected.depth,
              "2112x1188 internal source raster must map to 1920x1080 RGB");
        decoded.identity.sourceTimeNs++;
        selected=selectGtDisplayLayers(frame,decoded,1920,1080,true,true);
        check(!selected.sky && !selected.depth,"no source timestamp reuse");
        decoded.identity=*frame.source;
        frame.sky.sourceWidth=320U;
        selected=selectGtDisplayLayers(frame,decoded,1920,1080,true,true);
        check(!selected.sky && selected.depth,
              "native raster smaller than its sky map is independently rejected");
        frame.sky.sourceWidth=2112U;
        selected=selectGtDisplayLayers(frame,decoded,1920,1080,false,true);
        check(!selected.sky && selected.depth,"UI toggles are per-layer");
        std::cout<<"gt_map_worker_tests: PASS\n";
    } catch(const std::exception& ex) {
        std::cerr<<"gt_map_worker_tests: FAIL "<<ex.what()<<'\n';
        return 1;
    }
}
