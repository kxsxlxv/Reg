#include "vulkan/VideoRenderer.hpp"

#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult=" + std::to_string(result));
    }
}

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

VideoRenderer::VideoRenderer(const VulkanContext& vulkan)
    : vulkan_(vulkan),
      frameAccess_(vulkan) {
    createCommandResources();
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
        checkVk(vkCreateSemaphore(
                    vulkan_.device(), &semaphoreInfo, nullptr, &frameSlots_[i].renderFinished),
                "vkCreateSemaphore(renderFinished)");

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
        if (slot.renderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(vulkan_.device(), slot.renderFinished, nullptr);
            slot.renderFinished = VK_NULL_HANDLE;
        }
        if (slot.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(vulkan_.device(), slot.imageAvailable, nullptr);
            slot.imageAvailable = VK_NULL_HANDLE;
        }
    }

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
    if (videoFormatInitialized_ && format == videoFormat_) {
        return;
    }

    checkVk(vkQueueWaitIdle(vulkan_.graphicsQueue().handle), "vkQueueWaitIdle(video format change)");
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
    const std::string path = std::string(REG_SHADER_DIR) + "/" + filename;
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
        swapchainImageLayouts_.size() == swapchain.imageCount()) {
        return;
    }

    observedSwapchain_ = swapchain.handle();
    swapchainImageLayouts_.assign(
        swapchain.imageCount(),
        VK_IMAGE_LAYOUT_UNDEFINED);
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
    Swapchain& swapchain) {
    if (!frame) {
        return false;
    }

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

    checkVk(vkResetFences(vulkan_.device(), 1, &slot.fence), "vkResetFences");
    checkVk(vkResetCommandBuffer(slot.commandBuffer, 0), "vkResetCommandBuffer");

    SurfaceEntry& surface = surfaceFor(*frame);
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

        vkCmdEndRendering(slot.commandBuffer);

        recordSwapchainToPresentBarrier(
            slot.commandBuffer,
            swapchain.image(imageIndex));

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
        presentSignal.semaphore = slot.renderFinished;
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
        throw;
    }

    swapchainImageLayouts_.at(imageIndex) = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    slot.retainedFrame = frame;

    const bool presentOk = swapchain.present(imageIndex, slot.renderFinished);

    nextFrameSlot_ = (nextFrameSlot_ + 1) % frameSlots_.size();

    if (!presentOk) {
        if (swapchain.recreate()) {
            observedSwapchain_ = VK_NULL_HANDLE;
        }
    }

    return true;
}

} // namespace reg::vulkan
