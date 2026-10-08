#include "vulkan/VideoRenderer.hpp"

#include "capture/BmpWriter.hpp"
#include "render/VideoOverlayRecorder.hpp"
#include "render/OverlayChainRecorder.hpp"
#include "vulkan/BlankRenderer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanError.hpp"
#include "vulkan/VulkanContext.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixfmt.h>
}

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef REG_SHADER_DIR
#error REG_SHADER_DIR must be defined by CMake
#endif

namespace reg::vulkan {
namespace {

VkSamplerYcbcrModelConversion modelFor(AVColorSpace colorspace) {
    switch (colorspace) {
    case AVCOL_SPC_BT709:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
    case AVCOL_SPC_FCC:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
    case AVCOL_SPC_UNSPECIFIED:
        // The simulation stream is expected to be HD and should eventually carry
        // explicit VUI. Until then, BT.709 is the least-surprising HD default.
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    default:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    }
}

VkSamplerYcbcrRange rangeFor(AVColorRange range) {
    return range == AVCOL_RANGE_JPEG
        ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
        : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
}

std::pair<VkChromaLocation, VkChromaLocation> chromaOffsetsFor(AVChromaLocation location) {
    switch (location) {
    case AVCHROMA_LOC_CENTER:
        return {VK_CHROMA_LOCATION_MIDPOINT, VK_CHROMA_LOCATION_MIDPOINT};
    case AVCHROMA_LOC_TOPLEFT:
        return {VK_CHROMA_LOCATION_COSITED_EVEN, VK_CHROMA_LOCATION_COSITED_EVEN};
    case AVCHROMA_LOC_TOP:
        return {VK_CHROMA_LOCATION_MIDPOINT, VK_CHROMA_LOCATION_COSITED_EVEN};
    case AVCHROMA_LOC_LEFT:
    case AVCHROMA_LOC_UNSPECIFIED:
    default:
        // Common MPEG/H.264 4:2:0 placement: horizontal cosited, vertical midpoint.
        return {VK_CHROMA_LOCATION_COSITED_EVEN, VK_CHROMA_LOCATION_MIDPOINT};
    }
}

} // namespace

VideoRenderer::VideoRenderer(const VulkanContext& vulkan, bool allowNetImgui)
    : vulkan_(vulkan),
      frameAccess_(vulkan),
      blankRenderer_(std::make_unique<BlankRenderer>(vulkan)) {
#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    allowNetImgui_ = allowNetImgui;
#else
    static_cast<void>(allowNetImgui);
#endif
    createCommandResources();
}

bool VideoRenderer::renderRaw(
    const video::VideoFramePtr& frame,
    Swapchain& swapchain,
    render::VideoOverlayRecorder* overlay) {
#if defined(REG_ENABLE_NETIMGUI_REMOTE) && REG_ENABLE_NETIMGUI_REMOTE
    render::VideoOverlayRecorder* remote = ensureNetImguiOverlay(swapchain);
    if (remote != nullptr) {
        render::OverlayChainRecorder chain(overlay, remote);
        return render(frame, swapchain, &chain);
    }
#endif
    return render(frame, swapchain, overlay);
}

VideoRenderer::~VideoRenderer() {
    // renderFinished may still be referenced by a separate present queue.
    // A device-wide idle is acceptable during teardown and guarantees that
    // command buffers, decoded-frame references and WSI semaphores are retired.
    if (vulkan_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vulkan_.device());
    }
    destroyVideoResources();
    destroyCommandResources();
}

void VideoRenderer::resetVideoSession() {
    if (vulkan_.device() == VK_NULL_HANDLE) {
        return;
    }

    checkVk(
        vkQueueWaitIdle(
            vulkan_.graphicsQueue().handle),
        "vkQueueWaitIdle(video session reset)");

    // The swapchain keeps its last presented image until something new is
    // presented. Explicitly clear it before retiring decoder resources so a
    // dead/reconnecting source cannot leave a stale frozen video frame visible.
    if (lastSwapchain_ != nullptr && blankRenderer_) {
        try {
            static_cast<void>(
                blankRenderer_->render(*lastSwapchain_));
        } catch (const SurfaceLostError&) {
            // Surface recovery is owned by RenderWindow/main. Session cleanup
            // must still proceed even when the output disappeared concurrently.
        }
    }

    for (auto& slot : frameSlots_) {
        slot.retainedFrame.reset();
    }

    destroyVideoResources();
}

bool VideoRenderer::requestScreenshot(
    std::filesystem::path path) {
    if (path.empty() ||
        pendingScreenshot_.has_value()) {
        return false;
    }

    pendingScreenshot_ =
        std::move(path);
    return true;
}

void VideoRenderer::createCommandResources() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                     VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = vulkan_.graphicsQueue().familyIndex;
    checkVk(vkCreateCommandPool(vulkan_.device(), &poolInfo, nullptr, &commandPool_),
            "vkCreateCommandPool");

    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<std::uint32_t>(commandBuffers.size());
    checkVk(vkAllocateCommandBuffers(vulkan_.device(), &allocateInfo, commandBuffers.data()),
            "vkAllocateCommandBuffers");

    for (std::size_t i = 0; i < frameSlots_.size(); ++i) {
        frameSlots_[i].commandBuffer = commandBuffers[i];

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        checkVk(vkCreateSemaphore(
                    vulkan_.device(), &semaphoreInfo, nullptr, &frameSlots_[i].imageAvailable),
                "vkCreateSemaphore(imageAvailable)");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        checkVk(vkCreateFence(vulkan_.device(), &fenceInfo, nullptr, &frameSlots_[i].fence),
                "vkCreateFence");
    }
}

void VideoRenderer::destroyCommandResources() {
    for (auto& slot : frameSlots_) {
        slot.retainedFrame.reset();
        if (slot.fence != VK_NULL_HANDLE) {
            vkDestroyFence(vulkan_.device(), slot.fence, nullptr);
            slot.fence = VK_NULL_HANDLE;
        }
        if (slot.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(vulkan_.device(), slot.imageAvailable, nullptr);
            slot.imageAvailable = VK_NULL_HANDLE;
        }
    }

    destroySwapchainSyncResources();

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vulkan_.device(), commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
}

VideoRenderer::VideoFormat VideoRenderer::describeVideoFormat(
    const video::VideoFrame& frame) const {
    AVFrame* avFrame = frame.avFrame();
    if (avFrame == nullptr || avFrame->hw_frames_ctx == nullptr) {
        throw std::runtime_error("Video frame has no AVHWFramesContext");
    }

    auto* framesContext =
        reinterpret_cast<AVHWFramesContext*>(avFrame->hw_frames_ctx->data);
    auto* vkFrames =
        reinterpret_cast<AVVulkanFramesContext*>(framesContext->hwctx);

    if (framesContext->sw_format != AV_PIX_FMT_NV12) {
        throw std::runtime_error(
            "Phase A.2 currently requires FFmpeg Vulkan decode surfaces with sw_format=NV12");
    }
    if (vkFrames == nullptr || vkFrames->format[0] == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("FFmpeg Vulkan frame pool exposes no primary VkFormat");
    }
    if (vkFrames->format[1] != VK_FORMAT_UNDEFINED) {
        throw std::runtime_error(
            "Phase A.2 requires a single multiplanar Vulkan image, not separate plane formats");
    }

    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(
        vulkan_.physicalDevice(),
        vkFrames->format[0],
        &properties);

    const AVVkFrame* vkFrame = frame.vkFrame();
    const VkFormatFeatureFlags features =
        vkFrame->tiling == VK_IMAGE_TILING_LINEAR
            ? properties.linearTilingFeatures
            : properties.optimalTilingFeatures;

    if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        throw std::runtime_error("Decoded Vulkan video format is not sampleable");
    }

    auto [xOffset, yOffset] = chromaOffsetsFor(avFrame->chroma_location);

    const auto supportsOffset = [features](VkChromaLocation location) {
        const VkFormatFeatureFlagBits required =
            location == VK_CHROMA_LOCATION_COSITED_EVEN
                ? VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT
                : VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
        return (features & required) != 0;
    };

    if (!supportsOffset(xOffset)) {
        xOffset = xOffset == VK_CHROMA_LOCATION_COSITED_EVEN
            ? VK_CHROMA_LOCATION_MIDPOINT
            : VK_CHROMA_LOCATION_COSITED_EVEN;
    }
    if (!supportsOffset(yOffset)) {
        yOffset = yOffset == VK_CHROMA_LOCATION_COSITED_EVEN
            ? VK_CHROMA_LOCATION_MIDPOINT
            : VK_CHROMA_LOCATION_COSITED_EVEN;
    }
    if (!supportsOffset(xOffset) || !supportsOffset(yOffset)) {
        throw std::runtime_error("Decoded format exposes no compatible chroma sample location");
    }

    const bool linearFilter =
        (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0;

    return VideoFormat{
        .format = vkFrames->format[0],
        .model = modelFor(avFrame->colorspace),
        .range = rangeFor(avFrame->color_range),
        .xChromaOffset = xOffset,
        .yChromaOffset = yOffset,
        .filter = linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
    };
}

void VideoRenderer::ensureVideoResources(const video::VideoFrame& frame) {
    const VideoFormat format = describeVideoFormat(frame);

    AVFrame* avFrame = frame.avFrame();
    if (avFrame == nullptr || avFrame->hw_frames_ctx == nullptr) {
        throw std::runtime_error(
            "Video frame has no hardware-frame pool identity");
    }

    const bool sameFramePool =
        retainedFramesContext_ != nullptr &&
        retainedFramesContext_->data ==
            avFrame->hw_frames_ctx->data;

    if (videoFormatInitialized_ &&
        format == videoFormat_ &&
        sameFramePool) {
        return;
    }

    checkVk(
        vkQueueWaitIdle(vulkan_.graphicsQueue().handle),
        "vkQueueWaitIdle(video frame-pool change)");
    destroyVideoResources();
    createVideoResources(frame, format);

    if (frame.avFrame()->colorspace == AVCOL_SPC_UNSPECIFIED) {
        std::cerr << "[renderer] warning: stream colorspace is unspecified; assuming BT.709\n";
    }
    if (frame.avFrame()->color_range == AVCOL_RANGE_UNSPECIFIED) {
        std::cerr << "[renderer] warning: stream color range is unspecified; assuming limited range\n";
    }
}

void VideoRenderer::createVideoResources(
    const video::VideoFrame& frame,
    const VideoFormat& format) {
    if (frame.avFrame() == nullptr ||
        frame.avFrame()->hw_frames_ctx == nullptr) {
        throw std::runtime_error(
            "Cannot retain missing FFmpeg hardware-frame context");
    }

    retainedFramesContext_ =
        av_buffer_ref(
            frame.avFrame()->hw_frames_ctx);

    if (retainedFramesContext_ == nullptr) {
        throw std::bad_alloc{};
    }

    VkSamplerYcbcrConversionCreateInfo conversionInfo{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    conversionInfo.format = format.format;
    conversionInfo.ycbcrModel = format.model;
    conversionInfo.ycbcrRange = format.range;
    conversionInfo.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
    };
    conversionInfo.xChromaOffset = format.xChromaOffset;
    conversionInfo.yChromaOffset = format.yChromaOffset;
    conversionInfo.chromaFilter = format.filter;
    conversionInfo.forceExplicitReconstruction = VK_FALSE;

    checkVk(vkCreateSamplerYcbcrConversion(
                vulkan_.device(), &conversionInfo, nullptr, &ycbcrConversion_),
            "vkCreateSamplerYcbcrConversion");

    VkSamplerYcbcrConversionInfo samplerConversionInfo{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    samplerConversionInfo.conversion = ycbcrConversion_;

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.pNext = &samplerConversionInfo;
    samplerInfo.magFilter = format.filter;
    samplerInfo.minFilter = format.filter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0F;
    samplerInfo.maxLod = 0.0F;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    checkVk(vkCreateSampler(vulkan_.device(), &samplerInfo, nullptr, &sampler_),
            "vkCreateSampler(video)");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &sampler_;

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    checkVk(vkCreateDescriptorSetLayout(
                vulkan_.device(), &layoutInfo, nullptr, &descriptorSetLayout_),
            "vkCreateDescriptorSetLayout(video)");

    VkSamplerYcbcrConversionImageFormatProperties ycbcrProperties{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 imageProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    imageProperties.pNext = &ycbcrProperties;

    VkPhysicalDeviceImageFormatInfo2 formatInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    formatInfo.format = format.format;
    formatInfo.type = VK_IMAGE_TYPE_2D;
    formatInfo.tiling = frame.vkFrame()->tiling;
    formatInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    checkVk(
        vkGetPhysicalDeviceImageFormatProperties2(
            vulkan_.physicalDevice(),
            &formatInfo,
            &imageProperties),
        "vkGetPhysicalDeviceImageFormatProperties2(video)");

    if (ycbcrProperties.combinedImageSamplerDescriptorCount == 0) {
        throw std::runtime_error("Vulkan reported zero descriptors for YCbCr combined sampler");
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount =
        kMaxCachedVideoSurfaces * ycbcrProperties.combinedImageSamplerDescriptorCount;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kMaxCachedVideoSurfaces;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    checkVk(vkCreateDescriptorPool(
                vulkan_.device(), &poolInfo, nullptr, &descriptorPool_),
            "vkCreateDescriptorPool(video)");

    videoFormat_ = format;
    videoFormatInitialized_ = true;
}

void VideoRenderer::destroyVideoResources() {
    destroyGraphicsPipeline();

    for (const auto& surface : surfaces_) {
        if (surface.view != VK_NULL_HANDLE) {
            vkDestroyImageView(vulkan_.device(), surface.view, nullptr);
        }
    }
    surfaces_.clear();

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vulkan_.device(), descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vulkan_.device(), descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(vulkan_.device(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (ycbcrConversion_ != VK_NULL_HANDLE) {
        vkDestroySamplerYcbcrConversion(vulkan_.device(), ycbcrConversion_, nullptr);
        ycbcrConversion_ = VK_NULL_HANDLE;
    }

    av_buffer_unref(&retainedFramesContext_);

    videoFormat_ = {};
    videoFormatInitialized_ = false;
}

void VideoRenderer::ensureGraphicsPipeline(VkFormat swapchainFormat) {
    if (pipeline_ != VK_NULL_HANDLE && pipelineSwapchainFormat_ == swapchainFormat) {
        return;
    }
    if (descriptorSetLayout_ == VK_NULL_HANDLE) {
        throw std::logic_error("Video descriptor layout must exist before graphics pipeline");
    }

    if (pipeline_ != VK_NULL_HANDLE || pipelineLayout_ != VK_NULL_HANDLE) {
        checkVk(vkQueueWaitIdle(vulkan_.graphicsQueue().handle), "vkQueueWaitIdle(pipeline recreate)");
        destroyGraphicsPipeline();
    }

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    checkVk(vkCreatePipelineLayout(vulkan_.device(), &layoutInfo, nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout(video)");

    const VkShaderModule vertexShader = loadShaderModule("video.vert.spv");
    const VkShaderModule fragmentShader = loadShaderModule("video.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader;
    stages[0].pName = "main";

    stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[]{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &swapchainFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout_;

    const VkResult pipelineResult = vkCreateGraphicsPipelines(
        vulkan_.device(),
        VK_NULL_HANDLE,
        1,
        &pipelineInfo,
        nullptr,
        &pipeline_);

    vkDestroyShaderModule(vulkan_.device(), fragmentShader, nullptr);
    vkDestroyShaderModule(vulkan_.device(), vertexShader, nullptr);

    checkVk(pipelineResult, "vkCreateGraphicsPipelines(video)");
    pipelineSwapchainFormat_ = swapchainFormat;
}

void VideoRenderer::destroyGraphicsPipeline() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vulkan_.device(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vulkan_.device(), pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    pipelineSwapchainFormat_ = VK_FORMAT_UNDEFINED;
}

VideoRenderer::SurfaceEntry& VideoRenderer::surfaceFor(const video::VideoFrame& frame) {
    AVVkFrame* vkFrame = frame.vkFrame();
    if (vkFrame == nullptr || vkFrame->img[0] == VK_NULL_HANDLE) {
        throw std::runtime_error("Cannot create video surface for an invalid AVVkFrame");
    }

    const auto existing = std::ranges::find_if(surfaces_, [vkFrame](const SurfaceEntry& entry) {
        return entry.image == vkFrame->img[0];
    });
    if (existing != surfaces_.end()) {
        return *existing;
    }

    if (surfaces_.size() >= kMaxCachedVideoSurfaces) {
        throw std::runtime_error("Video surface cache exhausted");
    }

    VkSamplerYcbcrConversionInfo conversionInfo{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    conversionInfo.conversion = ycbcrConversion_;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.pNext = &conversionInfo;
    viewInfo.image = vkFrame->img[0];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = videoFormat_.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    SurfaceEntry entry{};
    entry.image = vkFrame->img[0];
    checkVk(vkCreateImageView(vulkan_.device(), &viewInfo, nullptr, &entry.view),
            "vkCreateImageView(video)");

    VkDescriptorSetAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &descriptorSetLayout_;

    const VkResult allocateResult =
        vkAllocateDescriptorSets(vulkan_.device(), &allocateInfo, &entry.descriptorSet);
    if (allocateResult != VK_SUCCESS) {
        vkDestroyImageView(vulkan_.device(), entry.view, nullptr);
        checkVk(allocateResult, "vkAllocateDescriptorSets(video)");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = VK_NULL_HANDLE; // immutable sampler in descriptor-set layout
    imageInfo.imageView = entry.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = entry.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(vulkan_.device(), 1, &write, 0, nullptr);

    surfaces_.push_back(entry);
    return surfaces_.back();
}

VkShaderModule VideoRenderer::loadShaderModule(const char* filename) const {
    std::filesystem::path shaderPath =
        std::filesystem::path(REG_SHADER_DIR) / filename;
    if (const char* base = SDL_GetBasePath()) {
        const auto portable = std::filesystem::path(base) / "shaders" / filename;
        if (std::filesystem::is_regular_file(portable)) {
            shaderPath = portable;
        }
    }
    const std::string path = shaderPath.string();
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("Cannot open SPIR-V shader: " + path);
    }

    const std::streamsize byteSize = stream.tellg();
    if (byteSize <= 0 || (byteSize % 4) != 0) {
        throw std::runtime_error("Invalid SPIR-V size for shader: " + path);
    }

    std::vector<std::uint32_t> code(static_cast<std::size_t>(byteSize) / sizeof(std::uint32_t));
    stream.seekg(0, std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(code.data()), byteSize)) {
        throw std::runtime_error("Failed to read SPIR-V shader: " + path);
    }

    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = static_cast<std::size_t>(byteSize);
    createInfo.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(vulkan_.device(), &createInfo, nullptr, &module),
            "vkCreateShaderModule");
    return module;
}

void VideoRenderer::syncSwapchainState(const Swapchain& swapchain) {
    if (observedSwapchain_ == swapchain.handle() &&
        swapchainImageLayouts_.size() == swapchain.imageCount() &&
        renderFinishedSemaphores_.size() == swapchain.imageCount()) {
        return;
    }

    // Swapchain::recreate() uses vkDeviceWaitIdle(), so when the handle changes
    // no presentation operation can still be using these semaphores.
    destroySwapchainSyncResources();

    observedSwapchain_ = swapchain.handle();
    swapchainImageLayouts_.assign(
        swapchain.imageCount(),
        VK_IMAGE_LAYOUT_UNDEFINED);

    renderFinishedSemaphores_.resize(
        swapchain.imageCount(),
        VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& semaphore : renderFinishedSemaphores_) {
        checkVk(
            vkCreateSemaphore(
                vulkan_.device(),
                &semaphoreInfo,
                nullptr,
                &semaphore),
            "vkCreateSemaphore(renderFinished)");
    }
}

void VideoRenderer::destroySwapchainSyncResources() {
    for (VkSemaphore semaphore : renderFinishedSemaphores_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vulkan_.device(), semaphore, nullptr);
        }
    }
    renderFinishedSemaphores_.clear();
    swapchainImageLayouts_.clear();
    observedSwapchain_ = VK_NULL_HANDLE;
}

void VideoRenderer::recordSwapchainToColorBarrier(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout oldLayout) const {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    // Availability from the presentation engine is established by waiting on
    // imageAvailable in the submission. There is no prior Vulkan pipeline stage
    // to wait on for PRESENT_SRC_KHR.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void VideoRenderer::recordSwapchainToPresentBarrier(
    VkCommandBuffer commandBuffer,
    VkImage image) const {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void VideoRenderer::recordSwapchainToTransferBarrier(
    VkCommandBuffer commandBuffer,
    VkImage image) const {
    VkImageMemoryBarrier2 barrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask =
        VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout =
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(
        commandBuffer,
        &dependency);
}

void VideoRenderer::recordSwapchainTransferToPresentBarrier(
    VkCommandBuffer commandBuffer,
    VkImage image) const {
    VkImageMemoryBarrier2 barrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask =
        VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask =
        VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask =
        VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask = 0;
    barrier.oldLayout =
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout =
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(
        commandBuffer,
        &dependency);
}

VideoRenderer::ScreenshotBuffer
VideoRenderer::createScreenshotBuffer(
    VkExtent2D extent) const {
    if (extent.width == 0 ||
        extent.height == 0) {
        throw std::invalid_argument(
            "screenshot extent must be non-zero");
    }

    ScreenshotBuffer result{};
    result.byteSize =
        static_cast<VkDeviceSize>(
            extent.width) *
        static_cast<VkDeviceSize>(
            extent.height) *
        4U;

    VkBufferCreateInfo bufferInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size =
        result.byteSize;
    bufferInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode =
        VK_SHARING_MODE_EXCLUSIVE;

    checkVk(
        vkCreateBuffer(
            vulkan_.device(),
            &bufferInfo,
            nullptr,
            &result.buffer),
        "vkCreateBuffer(screenshot)");

    try {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(
            vulkan_.device(),
            result.buffer,
            &requirements);

        VkPhysicalDeviceMemoryProperties
            memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(
            vulkan_.physicalDevice(),
            &memoryProperties);

        std::optional<std::uint32_t>
            fallbackMemoryType;

        std::optional<std::uint32_t>
            preferredMemoryType;

        for (std::uint32_t index = 0;
             index <
             memoryProperties.memoryTypeCount;
             ++index) {
            const std::uint32_t bit =
                1U << index;

            if ((requirements.memoryTypeBits &
                 bit) == 0) {
                continue;
            }

            const VkMemoryPropertyFlags flags =
                memoryProperties
                    .memoryTypes[index]
                    .propertyFlags;

            if ((flags &
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ==
                0) {
                continue;
            }

            if (!fallbackMemoryType) {
                fallbackMemoryType = index;
            }

            if ((flags &
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) !=
                0) {
                preferredMemoryType = index;
                break;
            }
        }

        const auto selected =
            preferredMemoryType
                ? preferredMemoryType
                : fallbackMemoryType;

        if (!selected) {
            throw std::runtime_error(
                "No host-visible Vulkan memory type is available for screenshot readback");
        }

        const VkMemoryPropertyFlags
            selectedFlags =
                memoryProperties
                    .memoryTypes[*selected]
                    .propertyFlags;

        result.coherent =
            (selectedFlags &
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) !=
            0;

        VkMemoryAllocateInfo allocationInfo{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocationInfo.allocationSize =
            requirements.size;
        allocationInfo.memoryTypeIndex =
            *selected;

        result.allocationSize =
            requirements.size;

        checkVk(
            vkAllocateMemory(
                vulkan_.device(),
                &allocationInfo,
                nullptr,
                &result.memory),
            "vkAllocateMemory(screenshot)");

        checkVk(
            vkBindBufferMemory(
                vulkan_.device(),
                result.buffer,
                result.memory,
                0),
            "vkBindBufferMemory(screenshot)");
    } catch (...) {
        destroyScreenshotBuffer(result);
        throw;
    }

    return result;
}

void VideoRenderer::destroyScreenshotBuffer(
    ScreenshotBuffer& buffer) const noexcept {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(
            vulkan_.device(),
            buffer.buffer,
            nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }

    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(
            vulkan_.device(),
            buffer.memory,
            nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }

    buffer.byteSize = 0;
    buffer.allocationSize = 0;
}

std::vector<std::uint8_t>
VideoRenderer::readScreenshotBuffer(
    const ScreenshotBuffer& buffer) const {
    if (buffer.memory == VK_NULL_HANDLE ||
        buffer.byteSize == 0) {
        throw std::logic_error(
            "screenshot buffer is not initialized");
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(
            buffer.byteSize));

    void* mapped = nullptr;

    checkVk(
        vkMapMemory(
            vulkan_.device(),
            buffer.memory,
            0,
            VK_WHOLE_SIZE,
            0,
            &mapped),
        "vkMapMemory(screenshot)");

    if (!buffer.coherent) {
        VkMappedMemoryRange range{
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;

        try {
            checkVk(
                vkInvalidateMappedMemoryRanges(
                    vulkan_.device(),
                    1,
                    &range),
                "vkInvalidateMappedMemoryRanges(screenshot)");
        } catch (...) {
            vkUnmapMemory(
                vulkan_.device(),
                buffer.memory);
            throw;
        }
    }

    std::memcpy(
        pixels.data(),
        mapped,
        pixels.size());

    vkUnmapMemory(
        vulkan_.device(),
        buffer.memory);

    return pixels;
}

VkViewport VideoRenderer::videoViewport(
    const video::VideoFrame& frame,
    VkExtent2D extent) const {
    const float sourceWidth = static_cast<float>(frame.width());
    const float sourceHeight = static_cast<float>(frame.height());
    const float targetWidth = static_cast<float>(extent.width);
    const float targetHeight = static_cast<float>(extent.height);

    const float scale = std::min(
        targetWidth / sourceWidth,
        targetHeight / sourceHeight);

    const float width = sourceWidth * scale;
    const float height = sourceHeight * scale;

    return VkViewport{
        .x = (targetWidth - width) * 0.5F,
        .y = (targetHeight - height) * 0.5F,
        .width = width,
        .height = height,
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
}

bool VideoRenderer::render(
    const video::VideoFramePtr& frame,
    Swapchain& swapchain,
    render::VideoOverlayRecorder* overlay) {
    if (!frame) {
        return false;
    }

    lastSwapchain_ = &swapchain;

    FrameSlot& slot = frameSlots_[nextFrameSlot_];

    const VkResult fenceStatus = vkGetFenceStatus(vulkan_.device(), slot.fence);
    if (fenceStatus == VK_NOT_READY) {
        return false;
    }
    checkVk(fenceStatus, "vkGetFenceStatus");
    slot.retainedFrame.reset();

    ensureVideoResources(*frame);
    ensureGraphicsPipeline(swapchain.format());
    syncSwapchainState(swapchain);

    std::uint32_t imageIndex = 0;
    const AcquireStatus acquireStatus = swapchain.acquire(slot.imageAvailable, imageIndex);
    if (acquireStatus == AcquireStatus::NotReady) {
        return false;
    }
    if (acquireStatus == AcquireStatus::Recreate) {
        if (swapchain.recreate()) {
            observedSwapchain_ = VK_NULL_HANDLE;
        }
        return false;
    }

    std::optional<std::filesystem::path>
        screenshotPath;

    ScreenshotBuffer screenshotBuffer{};
    bool captureThisFrame = false;

    if (pendingScreenshot_) {
        screenshotPath =
            std::move(*pendingScreenshot_);
        pendingScreenshot_.reset();

        if (!swapchain.transferSourceSupported()) {
            std::cerr
                << "[capture] swapchain does not support VK_IMAGE_USAGE_TRANSFER_SRC_BIT; screenshot skipped\n";
        } else {
            try {
                screenshotBuffer =
                    createScreenshotBuffer(
                        swapchain.extent());
                captureThisFrame = true;
            } catch (const DeviceLostError&) {
                throw;
            } catch (const std::exception& error) {
                std::cerr
                    << "[capture] staging buffer creation failed: "
                    << error.what()
                    << '\n';
            }
        }
    }

    checkVk(vkResetFences(vulkan_.device(), 1, &slot.fence), "vkResetFences");
    checkVk(vkResetCommandBuffer(slot.commandBuffer, 0), "vkResetCommandBuffer");

    SurfaceEntry& surface = surfaceFor(*frame);
    const VkSemaphore renderFinished =
        renderFinishedSemaphores_.at(imageIndex);
    auto lockedFrame = frameAccess_.lock(*frame);

    try {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(slot.commandBuffer, &beginInfo), "vkBeginCommandBuffer");

        frameAccess_.recordSampleBarrier(slot.commandBuffer, lockedFrame);

        recordSwapchainToColorBarrier(
            slot.commandBuffer,
            swapchain.image(imageIndex),
            swapchainImageLayouts_.at(imageIndex));

        VkClearValue clear{};
        clear.color = {{0.0F, 0.0F, 0.0F, 1.0F}};

        VkRenderingAttachmentInfo colorAttachment{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = swapchain.imageView(imageIndex);
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = clear;

        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea.extent = swapchain.extent();
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(slot.commandBuffer, &renderingInfo);

        const VkViewport viewport = videoViewport(*frame, swapchain.extent());
        const VkRect2D scissor{
            .offset = {0, 0},
            .extent = swapchain.extent(),
        };

        vkCmdSetViewport(slot.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(slot.commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(
            slot.commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_);
        vkCmdBindDescriptorSets(
            slot.commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout_,
            0,
            1,
            &surface.descriptorSet,
            0,
            nullptr);
        vkCmdDraw(slot.commandBuffer, 3, 1, 0, 0);

        if (overlay != nullptr) {
            overlay->record(
                slot.commandBuffer,
                swapchain.format(),
                swapchain.extent());
        }

        vkCmdEndRendering(slot.commandBuffer);

        if (captureThisFrame) {
            recordSwapchainToTransferBarrier(
                slot.commandBuffer,
                swapchain.image(imageIndex));

            VkBufferImageCopy copyRegion{};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;
            copyRegion.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageOffset = {0, 0, 0};
            copyRegion.imageExtent = {
                swapchain.extent().width,
                swapchain.extent().height,
                1,
            };

            vkCmdCopyImageToBuffer(
                slot.commandBuffer,
                swapchain.image(imageIndex),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                screenshotBuffer.buffer,
                1,
                &copyRegion);

            recordSwapchainTransferToPresentBarrier(
                slot.commandBuffer,
                swapchain.image(imageIndex));
        } else {
            recordSwapchainToPresentBarrier(
                slot.commandBuffer,
                swapchain.image(imageIndex));
        }

        checkVk(vkEndCommandBuffer(slot.commandBuffer), "vkEndCommandBuffer");

        const VkSemaphoreSubmitInfo frameWait = frameAccess_.waitInfo(lockedFrame);

        VkSemaphoreSubmitInfo imageWait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        imageWait.semaphore = slot.imageAvailable;
        imageWait.value = 0;
        imageWait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        const std::array<VkSemaphoreSubmitInfo, 2> waits{
            frameWait,
            imageWait,
        };

        const VkSemaphoreSubmitInfo frameSignal = frameAccess_.signalInfo(lockedFrame);

        VkSemaphoreSubmitInfo presentSignal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        presentSignal.semaphore = renderFinished;
        presentSignal.value = 0;
        presentSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        const std::array<VkSemaphoreSubmitInfo, 2> signals{
            frameSignal,
            presentSignal,
        };

        VkCommandBufferSubmitInfo commandInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = slot.commandBuffer;

        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
        submitInfo.pWaitSemaphoreInfos = waits.data();
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        submitInfo.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
        submitInfo.pSignalSemaphoreInfos = signals.data();

        checkVk(
            vkQueueSubmit2(
                vulkan_.graphicsQueue().handle,
                1,
                &submitInfo,
                slot.fence),
            "vkQueueSubmit2(video)");

        frameAccess_.commitSubmission(lockedFrame);
        frameAccess_.unlock(lockedFrame);
    } catch (...) {
        frameAccess_.unlock(lockedFrame);

        if (captureThisFrame) {
            destroyScreenshotBuffer(
                screenshotBuffer);
        }

        throw;
    }

    if (captureThisFrame) {
        try {
            checkVk(
                vkWaitForFences(
                    vulkan_.device(),
                    1,
                    &slot.fence,
                    VK_TRUE,
                    UINT64_MAX),
                "vkWaitForFences(screenshot)");

            const auto pixels =
                readScreenshotBuffer(
                    screenshotBuffer);

            destroyScreenshotBuffer(
                screenshotBuffer);

            capture::PixelOrder pixelOrder;

            if (swapchain.format() ==
                VK_FORMAT_B8G8R8A8_UNORM) {
                pixelOrder =
                    capture::PixelOrder::Bgra8;
            } else if (
                swapchain.format() ==
                VK_FORMAT_R8G8B8A8_UNORM) {
                pixelOrder =
                    capture::PixelOrder::Rgba8;
            } else {
                throw std::runtime_error(
                    "unsupported swapchain format for screenshot");
            }

            try {
                capture::writeBmp32(
                    *screenshotPath,
                    swapchain.extent().width,
                    swapchain.extent().height,
                    pixels,
                    pixelOrder);

                std::cout
                    << "[capture] saved "
                    << screenshotPath->string()
                    << '\n';
            } catch (const std::exception& error) {
                std::cerr
                    << "[capture] file write failed: "
                    << error.what()
                    << '\n';
            }
        } catch (const DeviceLostError&) {
            destroyScreenshotBuffer(
                screenshotBuffer);
            throw;
        } catch (const std::exception& error) {
            destroyScreenshotBuffer(
                screenshotBuffer);

            std::cerr
                << "[capture] GPU readback failed: "
                << error.what()
                << '\n';
        }
    }

    swapchainImageLayouts_.at(imageIndex) = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    slot.retainedFrame = frame;

    const bool presentOk = swapchain.present(imageIndex, renderFinished);

    nextFrameSlot_ = (nextFrameSlot_ + 1) % frameSlots_.size();

    if (!presentOk) {
        if (swapchain.recreate()) {
            observedSwapchain_ = VK_NULL_HANDLE;
        }
    }

    return true;
}

} // namespace reg::vulkan
