#include "recorder/GtAuJournal.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace reg;
void check(bool condition,const char* reason) {
    if(!condition)throw std::runtime_error(reason);
}

media::CompressedVideoPacketPtr makePacket(std::uint8_t payload,
                                           std::int64_t pts) {
    AVPacket* packet=av_packet_alloc();
    check(packet!=nullptr,"allocate packet");
    check(av_new_packet(packet,7)==0,"allocate packet bytes");
    const std::array<std::uint8_t,7> bytes{0U,0U,1U,6U,5U,payload,0x80U};
    for(std::size_t i=0;i<bytes.size();++i)packet->data[i]=bytes[i];
    packet->pts=pts;
    packet->dts=pts-10;
    const auto clone=media::CompressedVideoPacket::cloneFrom(
        packet,AVRational{1,90000});
    av_packet_free(&packet);
    return clone;
}

int main() {
    using namespace reg::recorder;
    try {
        const auto base=std::filesystem::temp_directory_path()/
            ("reg-gt-au-test-"+std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto file=base/"capture.rgau";
        std::filesystem::create_directories(base);
        {
            GtAuJournal journal(GtAuJournalConfig{
                .path=file,
                .maxQueueBytes=4096,
                .maxQueuePackets=64,
                .maxFileBytes=4096,
            });
            check(journal.submit(makePacket(1U,-5),3U),"first packet accepted");
            check(journal.submit(makePacket(2U,9007199254740993LL),3U),
                  "large positive PTS accepted");
            journal.stop();
            const auto stats=journal.stats();
            check(stats.accepted==2U && stats.written==2U &&
                  stats.dropped==0U && stats.fileErrors==0U && !stats.failed,
                  "complete benchmark journal status");
        }
        {
            GtAuJournalReader reader(file);
            auto first=reader.next();
            auto second=reader.next();
            check(first && second && !reader.next(),"reader exact packet count");
            check(first->pts==-5 && first->dts==-15 && first->decoderSession==3 &&
                  first->timeBaseNum==1 && first->timeBaseDen==90000 &&
                  first->bytes==std::vector<std::uint8_t>({0,0,1,6,5,1,0x80}),
                  "first packet identity and payload preserved");
            check(second->pts==9007199254740993LL &&
                  second->bytes.back()==0x80,"uint64+ precision preserved");
        }
        {
            std::ifstream in(file,std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
            check(bytes.size()>100U,"journal format");
            bytes.back()^=static_cast<char>(1);
            const auto corrupt=base/"corrupt.rgau";
            std::ofstream out(corrupt,std::ios::binary);
            out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
            out.close();
            bool rejected=false;
            try {
                GtAuJournalReader bad(corrupt);
                static_cast<void>(bad.next());
                static_cast<void>(bad.next());
            } catch(const std::runtime_error&) {rejected=true;}
            check(rejected,"CRC corruption rejected");
        }
        {
            GtAuJournal journal(GtAuJournalConfig{
                .path=base/"quota.rgau",
                .maxQueueBytes=1024,
                .maxQueuePackets=8,
                .maxFileBytes=62, // header+one full packet record needs 63
            });
            check(journal.submit(makePacket(3U,5),1U),"quota input queued");
            journal.stop();
            check(journal.stats().failed &&
                  journal.stats().fileErrors>0U,"quota failure must be explicit");
        }
        const auto manifest=std::filesystem::path(file.string()+".manifest.json");
        check(std::filesystem::exists(manifest),"benchmark manifest emitted");
        std::filesystem::remove_all(base);
        std::cout<<"gt_au_journal_tests: PASS\n";
    } catch(const std::exception& e) {
        std::cerr<<"gt_au_journal_tests: FAIL "<<e.what()<<'\n';
        return 1;
    }
}
