#include "cvv1/Cvv1.hpp"
#include "metadata/Protocol.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace reg::cvv1 {
namespace {
using json = nlohmann::json;
void require(bool ok, const char* why) {
    if (!ok) throw std::invalid_argument(why);
}
template<class T>
T integer(const json& j, const char* why, T maximum = std::numeric_limits<T>::max()) {
    require(j.is_number_integer() && !j.is_boolean(), why);
    if (j.is_number_unsigned()) {
        const auto v = j.get<std::uint64_t>();
        require(v <= static_cast<std::uint64_t>(maximum), why);
        return static_cast<T>(v);
    }
    const auto v = j.get<std::int64_t>();
    require(v >= 0 && static_cast<std::uint64_t>(v) <= static_cast<std::uint64_t>(maximum), why);
    return static_cast<T>(v);
}
template<class T>
T decimal(const json& j, const char* why) {
    require(j.is_string(), why);
    const auto& s = j.get_ref<const std::string&>();
    require(!s.empty(), why);
    T result{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    require(ec == std::errc{} && end == s.data() + s.size(), why);
    require(s[0] != '+' && !(std::is_unsigned_v<T> && s[0] == '-'), why);
    return result;
}
double number(const json& j, const char* why) {
    require(j.is_number(), why);
    const double n = j.get<double>();
    require(std::isfinite(n), why);
    return n;
}
double nonnegative(const json& j, const char* why) {
    const double n = number(j, why);
    require(n >= 0, why);
    return n;
}
float score(const json& j) {
    const double n = number(j, "CVV1 invalid confidence");
    require(n >= 0 && n <= 1, "CVV1 confidence outside [0,1]");
    return static_cast<float>(n);
}
bool boolean(const json& j, const char* why) {
    require(j.is_boolean(), why);
    return j.get<bool>();
}
Box box(const json& j) {
    require(j.is_array() && j.size() == 4, "CVV1 bbox must be [x,y,w,h]");
    const double x = number(j[0], "CVV1 invalid bbox x");
    const double y = number(j[1], "CVV1 invalid bbox y");
    const double w = number(j[2], "CVV1 invalid bbox width");
    const double h = number(j[3], "CVV1 invalid bbox height");
    require(x >= 0 && y >= 0 && w > 0 && h > 0 &&
            x + w <= 1.000001 && y + h <= 1.000001,
            "CVV1 bbox outside normalized source image");
    return {static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(w), static_cast<float>(h)};
}
State state(const json& j) {
    require(j.is_string(), "CVV1 invalid state");
    const auto& s = j.get_ref<const std::string&>();
    if (s == "SEARCH") return State::Search;
    if (s == "CONFIRMATION") return State::Confirmation;
    if (s == "TRACK") return State::Track;
    if (s == "RESET") return State::Reset;
    throw std::invalid_argument("CVV1 unknown algorithm state");
}
void validateNesting(std::string_view input) {
    int depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : input) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') require(++depth <= 16, "CVV1 JSON depth > 16");
        else if (ch == '}' || ch == ']') require(--depth >= 0, "CVV1 JSON unbalanced");
    }
    require(!quoted && depth == 0, "CVV1 JSON incomplete");
}
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t i) {
    return static_cast<std::uint16_t>(b[i] | (static_cast<unsigned>(b[i+1]) << 8));
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t i) {
    return static_cast<std::uint32_t>(u16(b,i)) |
           (static_cast<std::uint32_t>(u16(b,i+2)) << 16);
}
std::uint64_t u64(std::span<const std::uint8_t> b, std::size_t i) {
    return static_cast<std::uint64_t>(u32(b,i)) |
           (static_cast<std::uint64_t>(u32(b,i+4)) << 32);
}
} // namespace

Snapshot parseSnapshot(std::string_view payload, Clock::time_point receivedAt) {
    require(!payload.empty() && payload.size() <= kMaxPayload, "CVV1 payload size");
    validateNesting(payload);
    const json j = json::parse(payload.begin(), payload.end());
    require(j.is_object() && integer<unsigned>(j.at("schema_version"), "CVV1 schema") == 1,
            "CVV1 unsupported schema");
    Snapshot s;
    s.receivedAt = receivedAt;
    const auto& key = j.at("frame_key");
    require(key.is_object(), "CVV1 frame key");
    s.key = {decimal<std::uint64_t>(key.at("stream_epoch"), "CVV1 epoch"),
             decimal<std::uint64_t>(key.at("frame_id"), "CVV1 frame ID")};
    s.sourceGeneration = decimal<std::uint64_t>(j.at("source_generation"), "CVV1 source generation");
    s.ingestSequence = decimal<std::uint64_t>(j.at("ingest_sequence"), "CVV1 ingest sequence");
    s.sourceWidth = integer<std::uint32_t>(j.at("source_width"), "CVV1 source width");
    s.sourceHeight = integer<std::uint32_t>(j.at("source_height"), "CVV1 source height");
    require(s.sourceWidth > 0 && s.sourceHeight > 0, "CVV1 zero source size");
    if (!j.at("source_pts_ns").is_null())
        s.sourcePtsNs = decimal<std::int64_t>(j.at("source_pts_ns"), "CVV1 source PTS");
    require(j.at("clock_domain") == "sender_monotonic", "CVV1 clock domain");
    s.ingestMonotonicNs = decimal<std::int64_t>(j.at("ingest_monotonic_ns"), "CVV1 sender timestamp");
    s.completedMonotonicNs = decimal<std::int64_t>(j.at("completed_monotonic_ns"), "CVV1 sender timestamp");
    s.state = state(j.at("state")); s.phaseBefore = state(j.at("phase_before"));
    s.detectorRan = boolean(j.at("detector_ran"), "CVV1 detector ran");
    s.trackerRan = boolean(j.at("tracker_ran"), "CVV1 tracker ran");
    s.acquiredThisFrame = boolean(j.at("acquired_this_frame"), "CVV1 acquired");
    s.lostThisFrame = boolean(j.at("lost_this_frame"), "CVV1 lost");
    s.confidence = score(j.at("confidence"));
    s.processingFps = nonnegative(j.at("processing_fps"), "CVV1 FPS");
    s.detectorThreshold = score(j.at("detector_threshold"));
    s.trackerThreshold = score(j.at("tracker_threshold"));
    s.weakResponses = integer<std::uint32_t>(j.at("weak_responses"), "CVV1 weak responses");
    s.weakLimit = integer<std::uint32_t>(j.at("weak_limit"), "CVV1 weak limit");
    const auto& cf = j.at("confirmation");
    s.confirmation.requiredHits = integer<std::uint32_t>(cf.at("required_hits"), "CVV1 confirmation hits");
    s.confirmation.window = integer<std::uint32_t>(cf.at("window"), "CVV1 confirmation window");
    s.confirmation.maxMisses = integer<std::uint32_t>(cf.at("max_misses"), "CVV1 confirmation misses");
    s.confirmation.bufferFirst = score(cf.at("buffer_first"));
    s.confirmation.bufferSecond = score(cf.at("buffer_second"));
    s.confirmation.minimumFirst = score(cf.at("minimum_first"));
    s.confirmation.minimumSecond = score(cf.at("minimum_second"));
    s.confirmation.nmsIou = score(cf.at("nms_iou"));
    s.confirmation.selectionGraceUpdates = integer<std::uint32_t>(cf.at("selection_grace_updates"), "CVV1 grace updates");
    if (!cf.at("grace_remaining").is_null())
        s.confirmation.graceRemaining = integer<std::uint32_t>(cf.at("grace_remaining"), "CVV1 grace remaining");
    require(s.confirmation.window > 0 &&
            s.confirmation.requiredHits <= s.confirmation.window,
            "CVV1 invalid confirmation window");
    const auto& tm = j.at("timing");
    s.timing = {nonnegative(tm.at("worker_wall_ms"), "CVV1 worker timing"),
                nonnegative(tm.at("tensor_rt_ms"), "CVV1 TRT timing"),
                nonnegative(tm.at("preprocess_ms"), "CVV1 preprocess timing"),
                nonnegative(tm.at("acquisition_wall_ms"), "CVV1 acquisition timing")};
    const auto& ct = j.at("counts");
    s.counts = {integer<std::uint32_t>(ct.at("detections"), "CVV1 detection count"),
                integer<std::uint32_t>(ct.at("candidates"), "CVV1 candidate count"),
                integer<std::uint32_t>(ct.at("confirmed"), "CVV1 confirmed count"),
                integer<std::uint32_t>(ct.at("best_hits"), "CVV1 best hits")};
    s.truncated = boolean(j.at("truncated"), "CVV1 truncated");
    s.omittedDetections = integer<std::uint32_t>(j.at("omitted_detections"), "CVV1 omitted detections");
    s.omittedCandidates = integer<std::uint32_t>(j.at("omitted_candidates"), "CVV1 omitted candidates");
    const auto& detections = j.at("detections");
    const auto& candidates = j.at("candidates");
    require(detections.is_array() && candidates.is_array() &&
            detections.size() + candidates.size() <= 2048, "CVV1 record limit");
    std::unordered_set<std::uint64_t> candidateIds;
    for (const auto& entry : detections) {
        Detection d;
        d.index = integer<std::uint32_t>(entry.at("index"), "CVV1 detection index");
        d.classId = integer<std::uint32_t>(entry.at("class_id"), "CVV1 detection class", 65535);
        d.confidence = score(entry.at("confidence"));
        d.bbox = box(entry.at("bbox"));
        s.detections.push_back(d);
    }
    for (const auto& entry : candidates) {
        Candidate c;
        c.id = decimal<std::uint64_t>(entry.at("id"), "CVV1 candidate ID");
        require(candidateIds.insert(c.id).second, "CVV1 duplicate candidate ID");
        c.classId = integer<std::uint32_t>(entry.at("class_id"), "CVV1 candidate class", 65535);
        c.confidence = score(entry.at("confidence"));
        c.lastDetectionConfidence = score(entry.at("last_detection_confidence"));
        c.bbox = box(entry.at("bbox"));
        c.hitCount = integer<std::uint32_t>(entry.at("hit_count"), "CVV1 hit count");
        c.windowLength = integer<std::uint32_t>(entry.at("window_length"), "CVV1 window length");
        c.confidenceSum = nonnegative(entry.at("confidence_sum"), "CVV1 confidence sum");
        c.missStreak = integer<std::uint32_t>(entry.at("miss_streak"), "CVV1 miss streak");
        c.confirmed = boolean(entry.at("confirmed"), "CVV1 candidate confirmed");
        c.matchedThisFrame = boolean(entry.at("matched_this_frame"), "CVV1 matched");
        c.selected = boolean(entry.at("selected"), "CVV1 selected");
        require(c.hitCount <= c.windowLength && c.windowLength <= s.confirmation.window,
                "CVV1 invalid hit history");
        s.candidates.push_back(c);
    }
    if (!j.at("selected_target").is_null()) {
        const auto& entry = j.at("selected_target");
        Target t;
        t.id = decimal<std::uint64_t>(entry.at("id"), "CVV1 target ID");
        t.candidateId = decimal<std::uint64_t>(entry.at("candidate_id"), "CVV1 seed candidate ID");
        t.classId = integer<std::uint32_t>(entry.at("class_id"), "CVV1 target class", 65535);
        t.confidence = score(entry.at("confidence"));
        t.bbox = box(entry.at("bbox"));
        require(entry.at("source") == "detector_seed" || entry.at("source") == "lorat",
                "CVV1 invalid target source");
        t.detectorSeed = entry.at("source") == "detector_seed";
        t.healthy = boolean(entry.at("healthy"), "CVV1 target healthy");
        require(s.state == State::Track, "CVV1 target outside TRACK");
        s.selectedTarget = t;
    }
    require(detections.size() + s.omittedDetections == s.counts.detections &&
            candidates.size() + s.omittedCandidates == s.counts.candidates,
            "CVV1 count mismatch");
    require(s.truncated == (s.omittedDetections != 0 || s.omittedCandidates != 0),
            "CVV1 truncation mismatch");
    require(detections.size() + candidates.size() + (s.selectedTarget ? 1U : 0U) <= 2048,
            "CVV1 record limit");
    return s;
}

void Reassembler::forget(std::size_t index) {
    const auto& p = pending_[index];
    for (const auto& part : p.chunks) bufferedBytes_ -= part.size();
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
}
void Reassembler::finish(Id id, Clock::time_point now) {
    finished_.push_back({id, now});
    while (finished_.size() > 1024) finished_.pop_front();
}
void Reassembler::expire(Clock::time_point now) {
    for (std::size_t i=0; i<pending_.size();) {
        if (now - pending_[i].created >= std::chrono::seconds(1)) forget(i);
        else ++i;
    }
    while (!finished_.empty() && now - finished_.front().at >= std::chrono::seconds(1))
        finished_.pop_front();
}
void Reassembler::clear() {
    pending_.clear(); finished_.clear(); bufferedBytes_ = 0;
}
std::optional<Snapshot> Reassembler::accept(
    std::span<const std::uint8_t> bytes, platform::UdpEndpoint endpoint, Clock::time_point now) {
    expire(now);
    require(bytes.size() >= 53 && bytes.size() <= kMaxDatagram, "CVV1 datagram length");
    require(std::memcmp(bytes.data(), "CVV1", 4) == 0 &&
            u16(bytes,4) == 1 && u16(bytes,6) == kHeaderBytes &&
            u32(bytes,8) == bytes.size() && u16(bytes,46) == 0,
            "CVV1 invalid header");
    const auto session = u64(bytes,16), snapshot = u64(bytes,24);
    const auto index = u16(bytes,32), count = u16(bytes,34);
    const auto total = u32(bytes,36), offset = u32(bytes,40);
    const auto length = u16(bytes,44);
    require(session != 0 && snapshot != 0 && total > 0 && total <= kMaxPayload,
            "CVV1 invalid identity/size");
    require(count == (total + kChunkBytes - 1) / kChunkBytes &&
            index < count && offset == index * kChunkBytes &&
            length == std::min<std::size_t>(kChunkBytes, total - offset) &&
            bytes.size() == kHeaderBytes + length + 4,
            "CVV1 invalid chunk geometry");
    require(metadata::protocol::crc32c(bytes.first(bytes.size()-4)) ==
            u32(bytes,bytes.size()-4), "CVV1 CRC32C mismatch");
    const Id id{endpoint,session,snapshot};
    for (const auto& f : finished_) if (f.id == id) return std::nullopt;
    auto it = std::find_if(pending_.begin(),pending_.end(),
                           [&](const Pending& p){return p.id == id;});
    if (it == pending_.end()) {
        if (pending_.size() >= 64) forget(0);
        pending_.push_back({id,now,total,count,std::vector<std::vector<std::uint8_t>>(count),0});
        it = std::prev(pending_.end());
    } else if (it->payloadSize != total || it->chunkCount != count) {
        forget(static_cast<std::size_t>(it-pending_.begin()));
        finish(id,now);
        throw std::invalid_argument("CVV1 inconsistent snapshot dimensions");
    }
    std::vector<std::uint8_t> part(bytes.begin()+48,bytes.end()-4);
    auto& existing = it->chunks[index];
    if (!existing.empty()) {
        if (existing != part) {
            forget(static_cast<std::size_t>(it-pending_.begin()));
            finish(id,now);
            throw std::invalid_argument("CVV1 conflicting duplicate chunk");
        }
        return std::nullopt;
    }
    existing = std::move(part);
    ++it->received;
    bufferedBytes_ += existing.size();
    while (bufferedBytes_ > 16U*1024U*1024U && !pending_.empty()) forget(0);
    it = std::find_if(pending_.begin(),pending_.end(),
                       [&](const Pending& p){return p.id == id;});
    if (it == pending_.end() || it->received != it->chunkCount) return std::nullopt;
    std::string payload;
    payload.reserve(total);
    for (const auto& p : it->chunks) {
        require(!p.empty(), "CVV1 missing chunk");
        payload.append(reinterpret_cast<const char*>(p.data()),p.size());
    }
    forget(static_cast<std::size_t>(it-pending_.begin()));
    finish(id,now);
    Snapshot result = parseSnapshot(payload,now);
    result.senderSession = session;
    result.snapshotId = snapshot;
    return result;
}

void Store::insert(Snapshot snapshot) {
    std::scoped_lock lock(mutex_);
    const auto key = snapshot.key;
    if (map_.contains(key)) return; // First complete replacement wins for this exact frame.
    while (map_.size() >= capacity_) {
        map_.erase(order_.front()); order_.pop_front();
    }
    map_.emplace(key,std::make_shared<const Snapshot>(std::move(snapshot)));
    order_.push_back(key);
}
SnapshotPtr Store::find(media::FrameKey key) const {
    std::scoped_lock lock(mutex_);
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : it->second;
}
SnapshotPtr Store::take(media::FrameKey key) {
    std::scoped_lock lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) return {};
    auto value = it->second;
    map_.erase(it);
    const auto where = std::find(order_.begin(),order_.end(),key);
    if (where != order_.end()) order_.erase(where);
    return value;
}
void Store::clear() {
    std::scoped_lock lock(mutex_);
    map_.clear(); order_.clear();
}
std::size_t Store::size() const {
    std::scoped_lock lock(mutex_);
    return map_.size();
}
} // namespace reg::cvv1
