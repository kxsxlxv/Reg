#include "media/FrameIdentity.hpp"
#include "source/FrameIdentityAccessUnitInjector.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(
    bool condition,
    std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            std::string(message));
    }
}

struct NalView {
    std::uint8_t type{};
    std::span<const std::uint8_t> nal{};
};

std::vector<NalView> splitAnnexB(
    std::span<const std::uint8_t> bytes) {
    struct StartCode {
        std::size_t offset{};
        std::size_t size{};
    };

    const auto findStart =
        [bytes](std::size_t from) {
            for (std::size_t i = from;
                 i + 2U < bytes.size();
                 ++i) {
                if (bytes[i] != 0x00U ||
                    bytes[i + 1U] != 0x00U) {
                    continue;
                }

                if (bytes[i + 2U] == 0x01U) {
                    return StartCode{i, 3U};
                }

                if (i + 3U < bytes.size() &&
                    bytes[i + 2U] == 0x00U &&
                    bytes[i + 3U] == 0x01U) {
                    return StartCode{i, 4U};
                }
            }

            return StartCode{
                bytes.size(),
                0U};
        };

    std::vector<NalView> result;

    StartCode current =
        findStart(0);

    while (current.size != 0U) {
        const std::size_t header =
            current.offset +
            current.size;

        require(
            header < bytes.size(),
            "test Annex-B stream ended after start code");

        const StartCode next =
            findStart(header + 1U);

        const std::size_t end =
            next.size == 0U
                ? bytes.size()
                : next.offset;

        result.push_back(
            NalView{
                .type =
                    static_cast<std::uint8_t>(
                        bytes[header] &
                        0x1fU),
                .nal =
                    bytes.subspan(
                        header,
                        end - header),
            });

        current = next;
    }

    return result;
}

std::vector<std::uint8_t> ebspToRbsp(
    std::span<const std::uint8_t> ebsp) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(ebsp.size());

    unsigned zeros = 0;

    for (std::size_t i = 0;
         i < ebsp.size();
         ++i) {
        const std::uint8_t byte =
            ebsp[i];

        if (zeros >= 2U &&
            byte == 0x03U) {
            require(
                i + 1U < ebsp.size() &&
                    ebsp[i + 1U] <= 0x03U,
                "invalid emulation-prevention byte");
            zeros = 0;
            continue;
        }

        rbsp.push_back(byte);

        if (byte == 0x00U) {
            ++zeros;
        } else {
            zeros = 0;
        }
    }

    return rbsp;
}

reg::media::SourceFrameIdentity
decodeInjectedIdentity(
    const NalView& seiNal) {
    require(
        seiNal.type == 6U,
        "requested NAL is not SEI");
    require(
        seiNal.nal.size() > 1U,
        "SEI NAL is empty");

    const auto rbsp =
        ebspToRbsp(
            seiNal.nal.subspan(1));

    require(
        rbsp.size() >=
            2U +
            reg::media::kFrameIdentitySeiUuid.size() +
            reg::media::kFrameIdentityPayloadSize,
        "SEI RBSP is too short");
    require(
        rbsp[0] == 5U,
        "SEI payload type is not user_data_unregistered");

    constexpr std::size_t expectedSize =
        reg::media::kFrameIdentitySeiUuid.size() +
        reg::media::kFrameIdentityPayloadSize;

    require(
        rbsp[1] == expectedSize,
        "SEI payload size mismatch");

    require(
        std::equal(
            reg::media::kFrameIdentitySeiUuid.begin(),
            reg::media::kFrameIdentitySeiUuid.end(),
            rbsp.begin() + 2),
        "SEI UUID mismatch");

    const auto decoded =
        reg::media::decodeFrameIdentityPayload(
            std::span<const std::uint8_t>{
                rbsp.data() +
                    2U +
                    reg::media::kFrameIdentitySeiUuid.size(),
                reg::media::kFrameIdentityPayloadSize});

    require(
        decoded.has_value(),
        "injected FrameIdentity payload did not decode");

    return *decoded;
}

std::vector<std::uint8_t>
makeAccessUnit(
    bool withAud,
    std::uint8_t vclType) {
    std::vector<std::uint8_t> bytes;

    const auto append4 =
        [&bytes](
            std::uint8_t header,
            std::initializer_list<std::uint8_t> payload) {
            bytes.insert(
                bytes.end(),
                {0x00U, 0x00U, 0x00U, 0x01U, header});
            bytes.insert(
                bytes.end(),
                payload);
        };

    const auto append3 =
        [&bytes](
            std::uint8_t header,
            std::initializer_list<std::uint8_t> payload) {
            bytes.insert(
                bytes.end(),
                {0x00U, 0x00U, 0x01U, header});
            bytes.insert(
                bytes.end(),
                payload);
        };

    if (withAud) {
        append4(0x09U, {0xf0U});
    }

    append4(0x67U, {0x64U, 0x00U, 0x1fU});
    append3(0x68U, {0xeeU, 0x3cU, 0x80U});

    const std::uint8_t vclHeader =
        static_cast<std::uint8_t>(
            0x60U | vclType);

    append4(
        vclHeader,
        {0x11U, 0x22U, 0x33U});

    return bytes;
}

void seiIsInsertedImmediatelyBeforeVcl() {
    const auto input =
        makeAccessUnit(
            true,
            5U);

    const reg::media::SourceFrameIdentity identity{
        .key = {
            .streamEpoch =
                0x0102030405060708ULL,
            .frameId = 42,
        },
        .sourceTimeNs = 987654321ULL,
    };

    std::vector<std::uint8_t> output;

    reg::source::
        injectFrameIdentitySeiIntoAnnexBAccessUnit(
            identity,
            input,
            output);

    const auto nals =
        splitAnnexB(output);

    require(
        nals.size() == 5U,
        "unexpected NAL count after injection");

    const std::array<std::uint8_t, 5>
        expectedTypes{
            9U,
            7U,
            8U,
            6U,
            5U,
        };

    for (std::size_t i = 0;
         i < expectedTypes.size();
         ++i) {
        require(
            nals[i].type ==
                expectedTypes[i],
            "injected SEI did not preserve prefix NAL ordering");
    }

    const auto decoded =
        decodeInjectedIdentity(
            nals[3]);

    require(
        decoded.key == identity.key,
        "injected FrameKey mismatch");
    require(
        decoded.sourceTimeNs ==
            identity.sourceTimeNs,
        "injected source timestamp mismatch");
}

void nonIdrVclAlsoGetsIdentity() {
    const auto input =
        makeAccessUnit(
            false,
            1U);

    const reg::media::SourceFrameIdentity identity{
        .key = {
            .streamEpoch = 77,
            .frameId = 9,
        },
        .sourceTimeNs = 123,
    };

    std::vector<std::uint8_t> output;

    reg::source::
        injectFrameIdentitySeiIntoAnnexBAccessUnit(
            identity,
            input,
            output);

    const auto nals =
        splitAnnexB(output);

    require(
        nals.size() == 4U,
        "unexpected non-IDR NAL count");
    require(
        nals[0].type == 7U &&
            nals[1].type == 8U &&
            nals[2].type == 6U &&
            nals[3].type == 1U,
        "SEI was not inserted before non-IDR VCL");
}

void sessionAssignsMonotonicExactKeys() {
    reg::source::FrameIdentityAccessUnitInjector
        injector(
            0x1122334455667788ULL,
            100);

    const auto input =
        makeAccessUnit(
            true,
            1U);

    std::vector<std::uint8_t> output;

    const auto first =
        injector.injectNext(
            input,
            1'000'000,
            output);

    const auto second =
        injector.injectNext(
            input,
            2'000'000,
            output);

    require(
        first.key.streamEpoch ==
            0x1122334455667788ULL &&
            second.key.streamEpoch ==
                first.key.streamEpoch,
        "stream epoch changed inside one session");
    require(
        first.key.frameId == 100 &&
            second.key.frameId == 101,
        "frame_id is not monotonic");
    require(
        first.sourceTimeNs == 1'000'000 &&
            second.sourceTimeNs == 2'000'000,
        "source timestamp was not preserved");
    require(
        injector.nextFrameId() == 102,
        "next frame_id did not advance");
}

void invalidAccessUnitDoesNotConsumeFrameId() {
    reg::source::FrameIdentityAccessUnitInjector
        injector(
            55,
            7);

    const std::array<std::uint8_t, 8>
        noVcl{
            0x00U, 0x00U, 0x00U, 0x01U,
            0x67U, 0x64U, 0x00U, 0x1fU,
        };

    std::vector<std::uint8_t> output;

    bool threw = false;

    try {
        static_cast<void>(
            injector.injectNext(
                noVcl,
                10,
                output));
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    require(
        threw,
        "access unit without VCL was accepted");
    require(
        injector.nextFrameId() == 7,
        "failed injection consumed frame_id");
}

void zeroEpochAndFrameIdOverflowAreRejected() {
    bool zeroEpochRejected = false;

    try {
        reg::source::FrameIdentityAccessUnitInjector
            invalid(0);
    } catch (const std::invalid_argument&) {
        zeroEpochRejected = true;
    }

    require(
        zeroEpochRejected,
        "zero stream epoch was accepted");

    reg::source::FrameIdentityAccessUnitInjector
        exhausted(
            1,
            std::numeric_limits<
                std::uint64_t>::max());

    const auto input =
        makeAccessUnit(
            false,
            1U);

    std::vector<std::uint8_t> output;

    bool overflowRejected = false;

    try {
        static_cast<void>(
            exhausted.injectNext(
                input,
                0,
                output));
    } catch (const std::overflow_error&) {
        overflowRejected = true;
    }

    require(
        overflowRejected,
        "frame_id wraparound was accepted");
}

} // namespace

int main() {
    try {
        seiIsInsertedImmediatelyBeforeVcl();
        nonIdrVclAlsoGetsIdentity();
        sessionAssignsMonotonicExactKeys();
        invalidAccessUnitDoesNotConsumeFrameId();
        zeroEpochAndFrameIdOverflowAreRejected();

        std::cout
            << "source_frame_identity_injector_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr
            << "source_frame_identity_injector_tests: FAIL: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    }
}
