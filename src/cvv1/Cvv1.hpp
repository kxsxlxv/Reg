#pragma once

#include "media/FrameIdentity.hpp"
#include "platform/UdpSocket.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace reg::cvv1 {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxDatagram = 1400;
constexpr std::size_t kHeaderBytes = 48;
constexpr std::size_t kChunkBytes = 1348;
constexpr std::size_t kMaxPayload = 1'048'576;

struct Box {
    float x{}, y{}, width{}, height{};
};
struct Detection {
    std::uint32_t index{}, classId{};
    float confidence{};
    Box bbox{};
};
struct Candidate {
    std::uint64_t id{};
    std::uint32_t classId{};
    float confidence{}, lastDetectionConfidence{};
    Box bbox{};
    std::uint32_t hitCount{}, windowLength{}, missStreak{};
    double confidenceSum{};
    bool confirmed{}, matchedThisFrame{}, selected{};
};
struct Target {
    std::uint64_t id{}, candidateId{};
    std::uint32_t classId{};
    float confidence{};
    Box bbox{};
    bool detectorSeed{}, healthy{};
};
struct Confirmation {
    std::uint32_t requiredHits{}, window{}, maxMisses{}, selectionGraceUpdates{};
    std::optional<std::uint32_t> graceRemaining;
    double bufferFirst{}, bufferSecond{}, minimumFirst{}, minimumSecond{}, nmsIou{};
};
struct Timing {
    double workerWallMs{}, tensorRtMs{}, preprocessMs{}, acquisitionWallMs{};
};
struct Counts {
    std::uint32_t detections{}, candidates{}, confirmed{}, bestHits{};
};
enum class State { Search, Confirmation, Track, Reset };

struct Snapshot {
    media::FrameKey key{};
    std::uint64_t senderSession{}, snapshotId{};
    std::uint64_t sourceGeneration{}, ingestSequence{};
    std::uint32_t sourceWidth{}, sourceHeight{};
    std::optional<std::int64_t> sourcePtsNs;
    std::int64_t ingestMonotonicNs{}, completedMonotonicNs{};
    State state{State::Search}, phaseBefore{State::Search};
    bool detectorRan{}, trackerRan{}, acquiredThisFrame{}, lostThisFrame{};
    double confidence{}, processingFps{}, detectorThreshold{}, trackerThreshold{};
    std::uint32_t weakResponses{}, weakLimit{};
    Confirmation confirmation{};
    Timing timing{};
    Counts counts{};
    bool truncated{};
    std::uint32_t omittedDetections{}, omittedCandidates{};
    std::vector<Detection> detections;
    std::vector<Candidate> candidates;
    std::optional<Target> selectedTarget;
    Clock::time_point receivedAt{};
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

// Parses a full, independent replacement snapshot. Throws on invalid schema or
// data, including unsafe numeric IDs, nonfinite numbers, or malformed geometry.
Snapshot parseSnapshot(std::string_view json, Clock::time_point receivedAt = Clock::now());

class Reassembler {
public:
    std::optional<Snapshot> accept(std::span<const std::uint8_t> bytes,
                                   platform::UdpEndpoint endpoint,
                                   Clock::time_point now = Clock::now());
    void clear();
    std::size_t pendingCount() const noexcept { return pending_.size(); }
    std::size_t bufferedBytes() const noexcept { return bufferedBytes_; }

private:
    struct Id {
        platform::UdpEndpoint endpoint{};
        std::uint64_t session{}, snapshot{};
        bool operator==(const Id&) const = default;
    };
    struct Pending {
        Id id{};
        Clock::time_point created{};
        std::uint32_t payloadSize{};
        std::uint16_t chunkCount{};
        std::vector<std::vector<std::uint8_t>> chunks;
        std::size_t received{};
    };
    struct Finished {
        Id id{};
        Clock::time_point at{};
    };
    void expire(Clock::time_point now);
    void forget(std::size_t index);
    void finish(Id id, Clock::time_point now);
    std::vector<Pending> pending_;
    std::deque<Finished> finished_;
    std::size_t bufferedBytes_{};
};

class Store {
public:
    explicit Store(std::size_t capacity = 512) : capacity_(capacity ? capacity : 1) {}
    void insert(Snapshot snapshot);
    SnapshotPtr take(media::FrameKey key);
    SnapshotPtr find(media::FrameKey key) const;
    void clear();
    std::size_t size() const;
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<media::FrameKey, SnapshotPtr, media::FrameKeyHash> map_;
    std::deque<media::FrameKey> order_;
};

} // namespace reg::cvv1
