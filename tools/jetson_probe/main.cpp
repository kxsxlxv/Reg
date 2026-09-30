#include "media/FfmpegError.hpp"
#include "media/RtspDecoder.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::uint32_t kNvidiaVendorId = 0x10DE;
constexpr const char* kDefaultRtspUrl =
    "rtsp://192.168.50.1:8555/reg";
constexpr std::uint64_t kDefaultProbeFrames = 300;

struct Options {
    std::string url{kDefaultRtspUrl};
    std::uint64_t frames{kDefaultProbeFrames};
    std::int64_t maxDelayUs{0};
    int reorderQueueSize{0};
    int extraHwFrames{32};
};

[[noreturn]] void usageError(const std::string& message) {
    throw std::invalid_argument(message + " (use --help for usage)");
}

long long parseInteger(
    const char* value,
    const char* option) {
    if (value == nullptr || *value == '\0') {
        usageError(std::string(option) + " requires a value");
    }

    std::string text(value);
    std::size_t parsed = 0;
    long long result = 0;

    try {
        result = std::stoll(text, &parsed, 10);
    } catch (const std::exception&) {
        usageError(std::string("invalid integer for ") + option + ": " + text);
    }

    if (parsed != text.size()) {
        usageError(std::string("invalid integer for ") + option + ": " + text);
    }

    return result;
}

Options parseOptions(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);

        if (argument == "--help" || argument == "-h") {
            std::cout
                << "Reg Jetson FrameIdentity probe\n\n"
                << "Usage:\n"
                << "  reg_jetson_probe [options]\n\n"
                << "Options:\n"
                << "  --url URL                    RTSP URL (default: "
                << kDefaultRtspUrl << ")\n"
                << "  --frames N                   Frames to validate (default: 300)\n"
                << "  --identity-probe-frames N    Alias for --frames\n"
                << "  --max-delay-us N             FFmpeg RTSP max_delay (default: 0)\n"
                << "  --reorder-queue-size N       FFmpeg reorder_queue_size (default: 0)\n"
                << "  --extra-hw-frames N          Decoder extra_hw_frames (default: 32)\n";
            std::exit(EXIT_SUCCESS);
        }

        const auto requireValue = [&]() -> const char* {
            if (i + 1 >= argc) {
                usageError(std::string(argument) + " requires a value");
            }
            return argv[++i];
        };

        if (argument == "--url") {
            options.url = requireValue();
            if (options.url.empty()) {
                usageError("--url must not be empty");
            }
        } else if (
            argument == "--frames" ||
            argument == "--identity-probe-frames") {
            const long long value =
                parseInteger(requireValue(), argv[i - 1]);
            if (value <= 0) {
                usageError("probe frame count must be greater than zero");
            }
            options.frames = static_cast<std::uint64_t>(value);
        } else if (argument == "--max-delay-us") {
            const long long value =
                parseInteger(requireValue(), argv[i - 1]);
            if (value < 0) {
                usageError("--max-delay-us must be non-negative");
            }
            options.maxDelayUs = static_cast<std::int64_t>(value);
        } else if (argument == "--reorder-queue-size") {
            const long long value =
                parseInteger(requireValue(), argv[i - 1]);
            if (value < 0 ||
                value > std::numeric_limits<int>::max()) {
                usageError("--reorder-queue-size is out of range");
            }
            options.reorderQueueSize = static_cast<int>(value);
        } else if (argument == "--extra-hw-frames") {
            const long long value =
                parseInteger(requireValue(), argv[i - 1]);
            if (value <= 0 ||
                value > std::numeric_limits<int>::max()) {
                usageError("--extra-hw-frames is out of range");
            }
            options.extraHwFrames = static_cast<int>(value);
        } else {
            usageError(std::string("unknown option: ") + std::string(argument));
        }
    }

    return options;
}

bool hasEnabledDeviceExtension(
    const AVVulkanDeviceContext& context,
    const char* extension) {
    for (int i = 0;
         i < context.nb_enabled_dev_extensions;
         ++i) {
        const char* enabled = context.enabled_dev_extensions[i];
        if (enabled != nullptr &&
            std::strcmp(enabled, extension) == 0) {
            return true;
        }
    }
    return false;
}

class HeadlessVulkanDevice final {
public:
    HeadlessVulkanDevice() {
        const int result = av_hwdevice_ctx_create(
            &deviceRef_,
            AV_HWDEVICE_TYPE_VULKAN,
            nullptr,
            nullptr,
            0);
        reg::media::checkFfmpeg(
            result,
            "av_hwdevice_ctx_create(Vulkan)");

        validate();
    }

    ~HeadlessVulkanDevice() {
        av_buffer_unref(&deviceRef_);
    }

    HeadlessVulkanDevice(const HeadlessVulkanDevice&) = delete;
    HeadlessVulkanDevice& operator=(
        const HeadlessVulkanDevice&) = delete;

    AVBufferRef* ref() const noexcept {
        return deviceRef_;
    }

private:
    void validate() const {
        if (deviceRef_ == nullptr ||
            deviceRef_->data == nullptr) {
            throw std::runtime_error(
                "FFmpeg returned an invalid Vulkan device context");
        }

        const auto* base =
            reinterpret_cast<const AVHWDeviceContext*>(
                deviceRef_->data);
        const auto* vulkan =
            reinterpret_cast<const AVVulkanDeviceContext*>(
                base->hwctx);

        if (vulkan == nullptr ||
            vulkan->phys_dev == VK_NULL_HANDLE ||
            vulkan->act_dev == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "FFmpeg Vulkan device has no active Vulkan physical/device handles");
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(
            vulkan->phys_dev,
            &properties);

        if (properties.vendorID != kNvidiaVendorId) {
            throw std::runtime_error(
                "reg_jetson_probe requires an NVIDIA Vulkan device");
        }
        if (properties.apiVersion < VK_API_VERSION_1_3) {
            throw std::runtime_error(
                "reg_jetson_probe requires Vulkan 1.3 or newer");
        }

        const bool hasVideoQueueExtension =
            hasEnabledDeviceExtension(
                *vulkan,
                VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        const bool hasDecodeQueueExtension =
            hasEnabledDeviceExtension(
                *vulkan,
                VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        const bool hasH264DecodeExtension =
            hasEnabledDeviceExtension(
                *vulkan,
                VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);

        if (!hasVideoQueueExtension ||
            !hasDecodeQueueExtension ||
            !hasH264DecodeExtension) {
            throw std::runtime_error(
                "FFmpeg Vulkan device did not enable the required "
                "VK_KHR_video_queue / video_decode_queue / "
                "video_decode_h264 extensions");
        }

        bool hasH264DecodeQueue = false;
        int h264QueueFamily = -1;

        for (int i = 0; i < vulkan->nb_qf; ++i) {
            const auto& family = vulkan->qf[i];
            const bool decodeQueue =
                (family.flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0;
            const bool h264 =
                (family.video_caps &
                 VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0;

            if (decodeQueue && h264) {
                hasH264DecodeQueue = true;
                h264QueueFamily = family.idx;
                break;
            }
        }

        if (!hasH264DecodeQueue) {
            throw std::runtime_error(
                "FFmpeg Vulkan device has no queue family with H.264 "
                "Vulkan Video decode capability");
        }

        std::cout
            << "[jetson-probe] GPU="
            << properties.deviceName
            << " vendor=0x"
            << std::hex
            << properties.vendorID
            << std::dec
            << " vulkan="
            << VK_API_VERSION_MAJOR(properties.apiVersion)
            << '.'
            << VK_API_VERSION_MINOR(properties.apiVersion)
            << '.'
            << VK_API_VERSION_PATCH(properties.apiVersion)
            << " h264_decode_qf="
            << h264QueueFamily
            << '\n';
    }

    AVBufferRef* deviceRef_{nullptr};
};

struct ProbeStats {
    std::uint64_t decoded{0};
    std::uint64_t identified{0};
    std::uint64_t missing{0};
    std::uint64_t nonMonotonic{0};
    std::uint64_t epochChanges{0};
    std::optional<reg::media::FrameKey> lastIdentity;
};

bool isPass(
    const ProbeStats& stats,
    std::uint64_t target) {
    return stats.decoded >= target &&
           stats.identified == stats.decoded &&
           stats.missing == 0 &&
           stats.nonMonotonic == 0;
}

void printSummary(
    const ProbeStats& stats,
    std::uint64_t target) {
    std::cout
        << "[jetson-probe] "
        << (isPass(stats, target) ? "PASS" : "FAIL")
        << " decoded=" << stats.decoded
        << " identified=" << stats.identified
        << " missing=" << stats.missing
        << " non_monotonic=" << stats.nonMonotonic
        << " epoch_changes=" << stats.epochChanges
        << " target=" << target
        << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);

        std::cout
            << "[jetson-probe] url="
            << options.url
            << " target="
            << options.frames
            << " max_delay_us="
            << options.maxDelayUs
            << " reorder_queue_size="
            << options.reorderQueueSize
            << " extra_hw_frames="
            << options.extraHwFrames
            << '\n';

        HeadlessVulkanDevice hwDevice;
        reg::media::RtspDecoder decoder(
            hwDevice.ref(),
            reg::media::RtspDecoderConfig{
                .url = options.url,
                .maxDelayUs = options.maxDelayUs,
                .reorderQueueSize = options.reorderQueueSize,
                .extraHwFrames = options.extraHwFrames,
            });

        ProbeStats stats;
        bool validationFailed = false;

        decoder.run(
            [&](reg::video::VideoFramePtr frame) {
                ++stats.decoded;

                const auto identity = frame->identity();
                if (!identity) {
                    ++stats.missing;
                    validationFailed = true;
                    std::cerr
                        << "[jetson-probe] missing FrameIdentity at decoded="
                        << stats.decoded
                        << '\n';
                } else {
                    ++stats.identified;

                    if (stats.lastIdentity) {
                        if (stats.lastIdentity->streamEpoch ==
                            identity->key.streamEpoch) {
                            if (identity->key.frameId <=
                                stats.lastIdentity->frameId) {
                                ++stats.nonMonotonic;
                                validationFailed = true;
                                std::cerr
                                    << "[jetson-probe] non-monotonic FrameIdentity"
                                    << " epoch="
                                    << identity->key.streamEpoch
                                    << " previous="
                                    << stats.lastIdentity->frameId
                                    << " current="
                                    << identity->key.frameId
                                    << '\n';
                            }
                        } else {
                            ++stats.epochChanges;
                        }
                    }

                    stats.lastIdentity = identity->key;

                    if (stats.decoded == 1 ||
                        stats.decoded % 60 == 0) {
                        std::cout
                            << "[jetson-probe] decoded="
                            << stats.decoded
                            << " epoch="
                            << identity->key.streamEpoch
                            << " frame_id="
                            << identity->key.frameId
                            << " size="
                            << frame->width()
                            << 'x'
                            << frame->height()
                            << '\n';
                    }
                }

                if (validationFailed ||
                    stats.decoded >= options.frames) {
                    decoder.requestStop();
                }
            });

        printSummary(stats, options.frames);
        return isPass(stats, options.frames)
            ? EXIT_SUCCESS
            : 2;
    } catch (const std::exception& error) {
        std::cerr
            << "[jetson-probe] ERROR: "
            << error.what()
            << '\n';
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr
            << "[jetson-probe] ERROR: unknown failure\n";
        return EXIT_FAILURE;
    }
}
