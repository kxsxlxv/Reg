#include "cvv1/Cvv1.hpp"
#include "cvv1/OverlayBuilder.hpp"
#include "metadata/Protocol.hpp"
#include "render/OverlayPrimitives.hpp"
#include "video/VideoTransform.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace reg::cvv1;
using nlohmann::json;

std::string example() {
    return R"JSON({
        "schema_version":1,
        "frame_key":{"stream_epoch":"9007199254740993","frame_id":"9007199254740995"},
        "source_generation":"3","ingest_sequence":"77",
        "source_width":1920,"source_height":1080,"source_pts_ns":null,
        "clock_domain":"sender_monotonic",
        "ingest_monotonic_ns":"1234567890123",
        "completed_monotonic_ns":"1234617890123",
        "state":"CONFIRMATION","phase_before":"SEARCH",
        "detector_ran":true,"tracker_ran":false,
        "acquired_this_frame":false,"lost_this_frame":false,
        "confidence":0.8,"processing_fps":20.0,
        "detector_threshold":0.55,"tracker_threshold":0.4,
        "weak_responses":0,"weak_limit":6,
        "confirmation":{
            "required_hits":25,"window":30,"max_misses":3,
            "buffer_first":0.2,"buffer_second":0.3,
            "minimum_first":0.1,"minimum_second":0.1,
            "nms_iou":0.85,"selection_grace_updates":10,
            "grace_remaining":4
        },
        "timing":{"worker_wall_ms":50,"tensor_rt_ms":48,
                  "preprocess_ms":0.3,"acquisition_wall_ms":0},
        "counts":{"detections":1,"candidates":2,"confirmed":1,"best_hits":26},
        "truncated":false,"omitted_detections":0,"omitted_candidates":0,
        "detections":[
            {"index":0,"class_id":0,"confidence":0.8,
             "bbox":[0.1,0.1,0.1,0.2]}
        ],
        "candidates":[
            {"id":"7","class_id":0,"confidence":0.8,
             "last_detection_confidence":0.8,"bbox":[0.1,0.1,0.1,0.2],
             "hit_count":26,"window_length":30,"confidence_sum":20.8,
             "miss_streak":0,"confirmed":true,"matched_this_frame":true,
             "selected":false},
            {"id":"8","class_id":0,"confidence":0,
             "last_detection_confidence":0.7,"bbox":[0.4,0.2,0.1,0.2],
             "hit_count":18,"window_length":23,"confidence_sum":12.6,
             "miss_streak":1,"confirmed":false,"matched_this_frame":false,
             "selected":false}
        ],
        "selected_target":null
    })JSON";
}
void check(bool value) {
    if (!value) throw std::runtime_error("CVV1 offline test assertion failed");
}
template<class F> void rejects(F&& call) {
    try { call(); } catch (const std::exception&) { return; }
    throw std::runtime_error("CVV1 invalid input was accepted");
}
void set16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off+1] = static_cast<std::uint8_t>(v>>8);
}
void set32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    set16(b,off,static_cast<std::uint16_t>(v));
    set16(b,off+2,static_cast<std::uint16_t>(v>>16));
}
void set64(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v) {
    set32(b,off,static_cast<std::uint32_t>(v));
    set32(b,off+4,static_cast<std::uint32_t>(v>>32));
}
void crc(std::vector<std::uint8_t>& b) {
    set32(b,b.size()-4,reg::metadata::protocol::crc32c(
        std::span<const std::uint8_t>(b.data(), b.size()-4)));
}
std::vector<std::vector<std::uint8_t>> encode(
    const std::string& payload, std::uint64_t session=44,
    std::uint64_t snapshot=1) {
    const std::size_t count = (payload.size()+kChunkBytes-1)/kChunkBytes;
    check(count > 0 && count <= 781);
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t i=0;i<count;++i) {
        const std::size_t off=i*kChunkBytes;
        const std::size_t len=std::min(kChunkBytes,payload.size()-off);
        std::vector<std::uint8_t> b(48+len+4);
        b[0]='C';b[1]='V';b[2]='V';b[3]='1';
        set16(b,4,1);set16(b,6,48);
        set32(b,8,static_cast<std::uint32_t>(b.size()));
        set32(b,12,static_cast<std::uint32_t>(i));
        set64(b,16,session);set64(b,24,snapshot);
        set16(b,32,static_cast<std::uint16_t>(i));
        set16(b,34,static_cast<std::uint16_t>(count));
        set32(b,36,static_cast<std::uint32_t>(payload.size()));
        set32(b,40,static_cast<std::uint32_t>(off));
        set16(b,44,static_cast<std::uint16_t>(len));
        std::copy_n(payload.begin()+static_cast<std::ptrdiff_t>(off),
                    len,b.begin()+48);
        crc(b);
        out.push_back(std::move(b));
    }
    return out;
}
void testParser() {
    auto s = parseSnapshot(example());
    check(s.key.streamEpoch == 9007199254740993ULL);
    check(s.key.frameId == 9007199254740995ULL);
    check(s.candidates.size()==2 && !s.candidates[1].matchedThisFrame);
    check(s.confirmation.graceRemaining == 4 && !s.selectedTarget);
    auto j = json::parse(example());
    j["frame_key"]["frame_id"] = 9007199254740995ULL;
    rejects([&]{parseSnapshot(j.dump());});
    j = json::parse(example());j["state"]="SLEEP";
    rejects([&]{parseSnapshot(j.dump());});
    j = json::parse(example());j["candidates"][1]["bbox"] = {-1,0,0.1,0.1};
    rejects([&]{parseSnapshot(j.dump());});
    j = json::parse(example());j["candidates"][0]["id"] = "8";
    rejects([&]{parseSnapshot(j.dump());});
    j = json::parse(example());j["confidence"] = 1e300 * 1e300;
    rejects([&]{parseSnapshot(j.dump());});
    rejects([&]{parseSnapshot(std::string(17,'[') + "0" + std::string(17,']'));});
    j = json::parse(example());j["selected_target"] = {
        {"id","9223372036854775815"},{"candidate_id","7"},{"class_id",0},
        {"confidence",0.8},{"bbox",{0.1,0.1,0.1,0.2}},
        {"source","detector_seed"},{"healthy",true}
    };
    j["state"]="TRACK";j["acquired_this_frame"]=true;
    s=parseSnapshot(j.dump());
    check(s.selectedTarget && s.selectedTarget->id==9223372036854775815ULL);
    reg::video::VideoTransform tx(1920,1080,1200,1920);
    OverlayBuilder builder;
    auto scene=builder.build(s,tx);
    std::size_t rectangles=0;
    for(const auto& prim:scene.primitives)
        if(std::holds_alternative<reg::render::RectPrimitive>(prim)) ++rectangles;
    check(rectangles==4); // YOLO + 2 candidates + selected target.
    j["selected_target"]=nullptr;j["state"]="RESET";j["lost_this_frame"]=true;
    s=parseSnapshot(j.dump());
    scene=builder.build(s,tx);
    rectangles=0;
    for(const auto& prim:scene.primitives)
        if(std::holds_alternative<reg::render::RectPrimitive>(prim)) ++rectangles;
    check(rectangles==3); // No resurrection of selected target.
    check(builder.noSnapshot(tx,Layers{},true).primitives.size()==1);
}
void testReassembly() {
    auto j = json::parse(example());
    j["padding"] = std::string(4000,'X'); // Multi-datagram, unknown field allowed.
    auto chunks=encode(j.dump());
    check(chunks.size()>=3);
    Reassembler a;
    const reg::platform::UdpEndpoint peer{0xC0A83203,1234};
    auto t=Clock::time_point{}+std::chrono::milliseconds(100);
    for(std::size_t i=chunks.size();i>0;--i) {
        auto result=a.accept(chunks[i-1],peer,t);
        check(result.has_value() == (i==1));
        if(result) check(result->key.streamEpoch==9007199254740993ULL);
    }
    check(a.pendingCount()==0 && a.bufferedBytes()==0);
    check(!a.accept(chunks[0],peer,t)); // Fully duplicated snapshot cache.
    a.clear();
    check(!a.accept(chunks[0],peer,t));
    check(!a.accept(chunks[0],peer,t)); // Identical partial duplicate.
    auto bad=chunks[0];bad[70]^=1;crc(bad);
    rejects([&]{a.accept(bad,peer,t);});
    check(a.pendingCount()==0 && a.bufferedBytes()==0);
    for(const auto& c:chunks) check(!a.accept(c,peer,t)); // Rejected ID.
    a.clear();check(!a.accept(chunks[0],peer,t));
    a.accept(chunks[1],peer,t+std::chrono::milliseconds(1100));
    check(a.pendingCount()==1 && a.bufferedBytes()==kChunkBytes);
    a.clear();
    bad=chunks[0];bad[80]^=1;
    rejects([&]{a.accept(bad,peer,t);}); // CRC32C
    bad=chunks[0];set32(bad,40,1);crc(bad);
    rejects([&]{a.accept(bad,peer,t);}); // Offset
    bad=chunks[0];set16(bad,4,2);crc(bad);
    rejects([&]{a.accept(bad,peer,t);}); // Version
    bad=chunks[0];set32(bad,8,42);crc(bad);
    rejects([&]{a.accept(bad,peer,t);}); // Datagram length
    a.clear();
    // Different sender endpoint and process session cannot share chunks.
    const reg::platform::UdpEndpoint another{0xC0A83204,1234};
    check(!a.accept(chunks[0],peer,t));
    check(!a.accept(chunks[1],another,t));
    check(a.pendingCount()==2);
    a.clear();
    auto restart=encode(j.dump(),55,1);
    check(!a.accept(chunks[0],peer,t));
    check(!a.accept(restart[0],peer,t));
    check(a.pendingCount()==2);
    Store store(2);
    store.insert(parseSnapshot(example()));
    check(store.find({9007199254740993ULL,9007199254740995ULL})!=nullptr);
    check(store.take({9007199254740993ULL,9007199254740995ULL})!=nullptr);
    check(store.size()==0);
}
} // namespace
int main() {
    try {
        testParser(); testReassembly();
        std::cout << "CVV1 offline parser, CRC, reassembly, renderer: PASS\n";
    } catch (const std::exception& e) {
        std::cerr << "CVV1 FAIL: " << e.what() << "\n";return 1;
    }
}
