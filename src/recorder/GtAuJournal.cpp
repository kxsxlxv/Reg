#include "recorder/GtAuJournal.hpp"

#include "media/H264AuSei.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace reg::recorder {
namespace {
constexpr std::array<char,8> kMagic{'R','G','A','U','1',0,0,0};
constexpr std::array<char,4> kRecord{'P','A','U','1'};
constexpr std::size_t kHeaderSize=12U,kRecordHeaderSize=44U;

void write32(std::ostream& out,std::uint32_t value) {
    for(unsigned i=0;i<4U;++i) out.put(static_cast<char>(
        (value>>(8U*i))&255U));
}
void write64(std::ostream& out,std::uint64_t value) {
    for(unsigned i=0;i<8U;++i) out.put(static_cast<char>(
        (value>>(8U*i))&255U));
}
std::uint32_t read32(std::istream& in) {
    std::uint32_t value=0;
    for(unsigned i=0;i<4U;++i) {
        const int byte=in.get();
        if(byte<0) throw std::runtime_error("Truncated RGAU1 field");
        value|=static_cast<std::uint32_t>(byte)<<(8U*i);
    }
    return value;
}
std::uint64_t read64(std::istream& in) {
    std::uint64_t value=0;
    for(unsigned i=0;i<8U;++i) {
        const int byte=in.get();
        if(byte<0) throw std::runtime_error("Truncated RGAU1 field");
        value|=static_cast<std::uint64_t>(byte)<<(8U*i);
    }
    return value;
}
std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc=0xffffffffU;
    for(const auto byte:bytes) {
        crc^=byte;
        for(unsigned j=0;j<8U;++j)
            crc=(crc>>1U)^((crc&1U)?0xedb88320U:0U);
    }
    return crc^0xffffffffU;
}
} // namespace

GtAuJournal::GtAuJournal(GtAuJournalConfig config)
    : config_(std::move(config)) {
    if(config_.maxQueuePackets==0U ||
       config_.maxQueueBytes==0U ||
       config_.maxFileBytes<=kHeaderSize+kRecordHeaderSize)
        throw std::invalid_argument("GtAuJournal requires positive bounds");
    worker_=std::jthread([this](std::stop_token token){run(token);});
}

GtAuJournal::~GtAuJournal(){stop();}

bool GtAuJournal::submit(media::CompressedVideoPacketPtr packet,
                         std::uint64_t session) noexcept {
    if(!packet) return false;
    const AVPacket* const raw=packet->avPacket();
    if(!raw || raw->size<=0 || raw->data==nullptr ||
       static_cast<std::uint64_t>(raw->size)>media::kMaxH264AuBytes) {
        dropped_.fetch_add(1U,std::memory_order_relaxed);
        failed_.store(true,std::memory_order_release);
        return false;
    }
    const auto bytes=static_cast<std::size_t>(raw->size);
    try {
        // No wait or disk IO on critical video thread.
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || stopped_.load(std::memory_order_acquire) ||
           failed_.load(std::memory_order_acquire) ||
           pending_.size()>=config_.maxQueuePackets ||
           bytes>config_.maxQueueBytes ||
           pendingBytes_>config_.maxQueueBytes-bytes) {
            dropped_.fetch_add(1U,std::memory_order_relaxed);
            failed_.store(true,std::memory_order_release);
            return false;
        }
        pending_.push_back({std::move(packet),session,bytes});
        pendingBytes_+=bytes;
        accepted_.fetch_add(1U,std::memory_order_relaxed);
        changed_.notify_one();
        return true;
    } catch(...) {
        dropped_.fetch_add(1U,std::memory_order_relaxed);
        failed_.store(true,std::memory_order_release);
        return false;
    }
}

void GtAuJournal::fail() noexcept {
    failed_.store(true,std::memory_order_release);
    fileErrors_.fetch_add(1U,std::memory_order_relaxed);
}

void GtAuJournal::run(std::stop_token token) noexcept {
    try {
        std::filesystem::create_directories(config_.path.parent_path());
        std::ofstream output(config_.path,std::ios::binary|std::ios::trunc);
        if(!output) {fail();return;}
        output.write(kMagic.data(),static_cast<std::streamsize>(kMagic.size()));
        write32(output,1U);
        std::uint64_t total=kHeaderSize;
        if(!output) {fail();return;}
        while(true) {
            Entry entry;
            {
                std::unique_lock lock(mutex_);
                changed_.wait_for(lock,token,std::chrono::milliseconds(150),
                                  [this]{return !pending_.empty();});
                // Shutdown drains accepted entries, so a clean stop is complete.
                if(pending_.empty() && token.stop_requested()) break;
                if(pending_.empty()) continue;
                entry=std::move(pending_.front());
                pending_.pop_front();
                pendingBytes_-=entry.bytes;
            }
            const AVPacket* const raw=entry.packet->avPacket();
            if(!raw || raw->size<0 || static_cast<std::size_t>(raw->size)!=entry.bytes ||
               entry.bytes>media::kMaxH264AuBytes) {fail();break;}
            constexpr std::uint64_t overhead=kRecordHeaderSize;
            if(total>config_.maxFileBytes ||
               entry.bytes>config_.maxFileBytes-total ||
               overhead>config_.maxFileBytes-total-entry.bytes) {
                fail();
                break;  // Explicit quota failure: never silently discard old AUs.
            }
            output.write(kRecord.data(),static_cast<std::streamsize>(kRecord.size()));
            write64(output,entry.decoderSession);
            write64(output,static_cast<std::uint64_t>(raw->pts));
            write64(output,static_cast<std::uint64_t>(raw->dts));
            const auto timeBase=entry.packet->timeBase();
            write32(output,static_cast<std::uint32_t>(timeBase.num));
            write32(output,static_cast<std::uint32_t>(timeBase.den));
            write32(output,static_cast<std::uint32_t>(entry.bytes));
            write32(output,crc32(std::span<const std::uint8_t>(
                raw->data,entry.bytes)));
            output.write(reinterpret_cast<const char*>(raw->data),
                         static_cast<std::streamsize>(entry.bytes));
            if(!output) {fail();break;}
            total+=overhead+entry.bytes;
            written_.fetch_add(1U,std::memory_order_relaxed);
            bytesWritten_.store(total,std::memory_order_relaxed);
        }
        output.flush();
        if(!output)fail();
        output.close();
        if(!output)fail();
    } catch(...) {fail();}
}

GtAuJournalStats GtAuJournal::stats() const noexcept {
    GtAuJournalStats out;
    out.accepted=accepted_.load(std::memory_order_relaxed);
    out.written=written_.load(std::memory_order_relaxed);
    out.dropped=dropped_.load(std::memory_order_relaxed);
    out.fileErrors=fileErrors_.load(std::memory_order_relaxed);
    out.bytesWritten=bytesWritten_.load(std::memory_order_relaxed);
    out.failed=failed_.load(std::memory_order_acquire);
    out.stopped=stopped_.load(std::memory_order_acquire);
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(lock.owns_lock()) {
        out.queuedPackets=pending_.size();
        out.queuedBytes=pendingBytes_;
    }
    return out;
}

void GtAuJournal::manifest() noexcept {
    try {
        const auto snapshot=stats();
        const auto complete=!snapshot.failed && snapshot.accepted==snapshot.written &&
                            snapshot.dropped==0U;
        std::ofstream out(config_.path.string()+".manifest.json",
                          std::ios::binary|std::ios::trunc);
        if(!out) {fail();return;}
        out<<"{\n  \"format\":\"RGAU1\",\n  \"version\":1,\n"
           <<"  \"complete\":"<<(complete?"true":"false")<<",\n"
           <<"  \"accepted\":\""<<snapshot.accepted<<"\",\n"
           <<"  \"written\":\""<<snapshot.written<<"\",\n"
           <<"  \"dropped\":\""<<snapshot.dropped<<"\",\n"
           <<"  \"file_errors\":\""<<snapshot.fileErrors<<"\",\n"
           <<"  \"bytes_written\":\""<<snapshot.bytesWritten<<"\",\n"
           <<"  \"max_file_bytes\":\""<<config_.maxFileBytes<<"\"\n}\n";
        out.flush();
        if(!out)fail();
    } catch(...) {fail();}
}

void GtAuJournal::stop() noexcept {
    if(stopped_.exchange(true,std::memory_order_acq_rel))return;
    worker_.request_stop();
    changed_.notify_all();
    if(worker_.joinable())worker_.join();
    // No records are accepted after stopped_, all successful writes have finished.
    manifest();
}

GtAuJournalReader::GtAuJournalReader(
    const std::filesystem::path& path)
    :input_(path,std::ios::binary) {
    if(!input_)throw std::runtime_error("RGAU1 journal not found");
    std::array<char,kMagic.size()> header{};
    input_.read(header.data(),static_cast<std::streamsize>(header.size()));
    if(!input_ || header!=kMagic || read32(input_)!=1U)
        throw std::runtime_error("Invalid RGAU1 file header");
}

std::optional<GtAuJournalRecord> GtAuJournalReader::next() {
    std::array<char,4> tag{};
    input_.read(tag.data(),static_cast<std::streamsize>(tag.size()));
    if(input_.gcount()==0 && input_.eof())return std::nullopt;
    if(!input_ || tag!=kRecord)
        throw std::runtime_error("Invalid or truncated RGAU1 record tag");
    GtAuJournalRecord record;
    record.decoderSession=read64(input_);
    record.pts=static_cast<std::int64_t>(read64(input_));
    record.dts=static_cast<std::int64_t>(read64(input_));
    record.timeBaseNum=static_cast<std::int32_t>(read32(input_));
    record.timeBaseDen=static_cast<std::int32_t>(read32(input_));
    const auto size=read32(input_);
    const auto crc=read32(input_);
    if(size==0U || size>media::kMaxH264AuBytes ||
       record.timeBaseNum<=0 || record.timeBaseDen<=0)
        throw std::runtime_error("Invalid RGAU1 PTS timebase or packet bound");
    record.bytes.resize(size);
    input_.read(reinterpret_cast<char*>(record.bytes.data()),
                static_cast<std::streamsize>(record.bytes.size()));
    if(!input_ || crc32(record.bytes)!=crc)
        throw std::runtime_error("RGAU1 packet CRC mismatch or truncation");
    return record;
}
} // namespace reg::recorder
