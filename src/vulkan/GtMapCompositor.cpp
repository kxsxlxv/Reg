#include "vulkan/GtMapCompositor.hpp"

#include "vulkan/VulkanError.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef REG_SHADER_DIR
#error REG_SHADER_DIR must be set by CMake
#endif

namespace reg::vulkan {
namespace {
constexpr VkImageSubresourceRange kColorRange{
    VK_IMAGE_ASPECT_COLOR_BIT,0U,1U,0U,1U};

VkShaderModule loadShader(const VulkanContext& context,const char* name) {
    std::filesystem::path path=std::filesystem::path(REG_SHADER_DIR)/name;
    if(const char* base=SDL_GetBasePath()) {
        const auto portable=std::filesystem::path(base)/"shaders"/name;
        if(std::filesystem::is_regular_file(portable))path=portable;
    }
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("GT shader not found: "+path.string());
    const std::streamsize count=input.tellg();
    if(count<=0 || count%4!=0)throw std::runtime_error("Invalid GT shader SPIR-V");
    std::vector<std::uint32_t> words(static_cast<std::size_t>(count)/4U);
    input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(words.data()),count))
        throw std::runtime_error("GT shader read failed");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize=static_cast<std::size_t>(count);
    info.pCode=words.data();
    VkShaderModule shader=VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(context.device(),&info,nullptr,&shader),
            "vkCreateShaderModule(gt)");
    return shader;
}

struct alignas(16) Push {
    std::array<float,4> controls{};
    std::array<std::int32_t,4> dimensions{};
    std::array<std::int32_t,4> flags{};
};
static_assert(sizeof(Push)==48U);
} // namespace

GtMapCompositor::GtMapCompositor(const VulkanContext& vulkan):vulkan_(vulkan) {
    VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampling.magFilter=VK_FILTER_NEAREST;
    sampling.minFilter=VK_FILTER_NEAREST;
    sampling.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampling.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampling.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampling.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    checkVk(vkCreateSampler(vulkan_.device(),&sampling,nullptr,&sampler_),
            "vkCreateSampler(gt)");

    VkDescriptorSetLayoutBinding bindings[2]{};
    for(std::uint32_t i=0;i<2U;++i) {
        bindings[i].binding=i;
        bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount=1;
        bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount=2;
    layout.pBindings=bindings;
    checkVk(vkCreateDescriptorSetLayout(vulkan_.device(),&layout,nullptr,&layout_),
            "vkCreateDescriptorSetLayout(gt)");

    VkDescriptorPoolSize count{};
    count.type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    count.descriptorCount=static_cast<std::uint32_t>(kSlots*2U);
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=static_cast<std::uint32_t>(kSlots);
    pool.poolSizeCount=1;
    pool.pPoolSizes=&count;
    checkVk(vkCreateDescriptorPool(vulkan_.device(),&pool,nullptr,&descriptorPool_),
            "vkCreateDescriptorPool(gt)");
    for(auto& slot:slots_) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool=descriptorPool_;
        alloc.descriptorSetCount=1;
        alloc.pSetLayouts=&layout_;
        checkVk(vkAllocateDescriptorSets(vulkan_.device(),&alloc,&slot.descriptorSet),
                "vkAllocateDescriptorSets(gt)");
    }
}

GtMapCompositor::~GtMapCompositor() {
    destroyPipeline();
    for(auto& slot:slots_) {
        destroyTexture(slot.sky);
        destroyTexture(slot.depth);
    }
    if(descriptorPool_)vkDestroyDescriptorPool(vulkan_.device(),descriptorPool_,nullptr);
    if(layout_)vkDestroyDescriptorSetLayout(vulkan_.device(),layout_,nullptr);
    if(sampler_)vkDestroySampler(vulkan_.device(),sampler_,nullptr);
}

std::uint32_t GtMapCompositor::findMemoryType(
    std::uint32_t bits,VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(vulkan_.physicalDevice(),&props);
    for(std::uint32_t i=0;i<props.memoryTypeCount;++i)
        if((bits&(1U<<i)) &&
           (props.memoryTypes[i].propertyFlags&flags)==flags)
            return i;
    throw std::runtime_error("No GT GPU image/staging memory type");
}

void GtMapCompositor::destroyTexture(Texture& t) noexcept {
    if(t.mapped)vkUnmapMemory(vulkan_.device(),t.stagingMemory);
    if(t.staging)vkDestroyBuffer(vulkan_.device(),t.staging,nullptr);
    if(t.stagingMemory)vkFreeMemory(vulkan_.device(),t.stagingMemory,nullptr);
    if(t.view)vkDestroyImageView(vulkan_.device(),t.view,nullptr);
    if(t.image)vkDestroyImage(vulkan_.device(),t.image,nullptr);
    if(t.imageMemory)vkFreeMemory(vulkan_.device(),t.imageMemory,nullptr);
    t={};
}

void GtMapCompositor::ensureTexture(
    Texture& t,VkFormat format,VkExtent2D extent) {
    if(t.image!=VK_NULL_HANDLE && t.extent.width==extent.width &&
       t.extent.height==extent.height && t.format==format)return;
    destroyTexture(t);
    t.extent=extent;
    t.format=format;
    try {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType=VK_IMAGE_TYPE_2D;
        image.format=format;
        image.extent={extent.width,extent.height,1U};
        image.mipLevels=1;
        image.arrayLayers=1;
        image.samples=VK_SAMPLE_COUNT_1_BIT;
        image.tiling=VK_IMAGE_TILING_OPTIMAL;
        image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        checkVk(vkCreateImage(vulkan_.device(),&image,nullptr,&t.image),
                "vkCreateImage(gt)");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(vulkan_.device(),t.image,&req);
        VkMemoryAllocateInfo mem{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mem.allocationSize=req.size;
        mem.memoryTypeIndex=findMemoryType(req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(vulkan_.device(),&mem,nullptr,&t.imageMemory),
                "vkAllocateMemory(gt image)");
        checkVk(vkBindImageMemory(vulkan_.device(),t.image,t.imageMemory,0),
                "vkBindImageMemory(gt)");

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=t.image;
        view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=format;
        view.subresourceRange=kColorRange;
        checkVk(vkCreateImageView(vulkan_.device(),&view,nullptr,&t.view),
                "vkCreateImageView(gt)");

        const VkDeviceSize size=VkDeviceSize(extent.width)*extent.height*
                                (format==VK_FORMAT_R32_SFLOAT?4U:1U);
        VkBufferCreateInfo buf{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buf.size=size;
        buf.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buf.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateBuffer(vulkan_.device(),&buf,nullptr,&t.staging),
                "vkCreateBuffer(gt)");
        vkGetBufferMemoryRequirements(vulkan_.device(),t.staging,&req);
        mem.allocationSize=req.size;
        mem.memoryTypeIndex=findMemoryType(
            req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checkVk(vkAllocateMemory(vulkan_.device(),&mem,nullptr,&t.stagingMemory),
                "vkAllocateMemory(gt staging)");
        checkVk(vkBindBufferMemory(vulkan_.device(),t.staging,t.stagingMemory,0),
                "vkBindBufferMemory(gt)");
        checkVk(vkMapMemory(vulkan_.device(),t.stagingMemory,0,size,0,&t.mapped),
                "vkMapMemory(gt)");
    } catch(...) {
        destroyTexture(t);
        throw;
    }
}

void GtMapCompositor::updateDescriptor(Slot& slot) const {
    VkDescriptorImageInfo infos[2]{};
    infos[0]={sampler_,slot.sky.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[1]={sampler_,slot.depth.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    for(std::uint32_t i=0;i<2U;++i) {
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet=slot.descriptorSet;
        writes[i].dstBinding=i;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount=1;
        writes[i].pImageInfo=&infos[i];
    }
    vkUpdateDescriptorSets(vulkan_.device(),2,writes,0,nullptr);
}

void GtMapCompositor::copyTexture(
    VkCommandBuffer cmd,Texture& t,const std::uint8_t* bytes,std::size_t length) {
    const auto size=std::size_t(t.extent.width)*t.extent.height*
                    (t.format==VK_FORMAT_R32_SFLOAT?4U:1U);
    if(length>size)throw std::invalid_argument("GT staging texture length");
    std::memset(t.mapped,0,size);
    if(bytes && length)std::memcpy(t.mapped,bytes,length);

    VkImageMemoryBarrier2 toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toTransfer.srcStageMask=t.layout==VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_PIPELINE_STAGE_2_NONE:VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toTransfer.srcAccessMask=t.layout==VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_ACCESS_2_NONE:VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toTransfer.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toTransfer.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout=t.layout;
    toTransfer.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.image=t.image;
    toTransfer.subresourceRange=kColorRange;
    VkDependencyInfo before{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    before.imageMemoryBarrierCount=1;
    before.pImageMemoryBarriers=&toTransfer;
    vkCmdPipelineBarrier2(cmd,&before);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount=1;
    copy.imageExtent={t.extent.width,t.extent.height,1U};
    vkCmdCopyBufferToImage(cmd,t.staging,t.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);

    VkImageMemoryBarrier2 toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toSample.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toSample.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toSample.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toSample.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toSample.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.image=t.image;
    toSample.subresourceRange=kColorRange;
    VkDependencyInfo after{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    after.imageMemoryBarrierCount=1;
    after.pImageMemoryBarriers=&toSample;
    vkCmdPipelineBarrier2(cmd,&after);
    t.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void GtMapCompositor::ensurePipeline(VkFormat format) {
    if(pipeline_ && colorFormat_==format)return;
    if(pipeline_) {
        checkVk(vkQueueWaitIdle(vulkan_.graphicsQueue().handle),
                "vkQueueWaitIdle(gt pipeline)");
        destroyPipeline();
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount=1;
    layoutInfo.pSetLayouts=&layout_;
    layoutInfo.pushConstantRangeCount=1;
    layoutInfo.pPushConstantRanges=&push;
    checkVk(vkCreatePipelineLayout(vulkan_.device(),&layoutInfo,nullptr,
                                    &pipelineLayout_),"vkCreatePipelineLayout(gt)");

    VkShaderModule vertex=loadShader(vulkan_,"video.vert.spv");
    VkShaderModule fragment=VK_NULL_HANDLE;
    try {
        fragment=loadShader(vulkan_,"gt_map.frag.spv");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module=vertex;
        stages[0].pName="main";
        stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module=fragment;
        stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount=1;
        viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode=VK_POLYGON_MODE_FILL;
        raster.cullMode=VK_CULL_MODE_NONE;
        raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth=1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable=VK_TRUE;
        attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp=VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.alphaBlendOp=VK_BLEND_OP_ADD;
        attachment.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|
                                   VK_COLOR_COMPONENT_G_BIT|
                                   VK_COLOR_COMPONENT_B_BIT|
                                   VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendState blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount=1;
        blend.pAttachments=&attachment;
        const VkDynamicState dynamicStates[]{
            VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount=2;
        dynamic.pDynamicStates=dynamicStates;
        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount=1;
        rendering.pColorAttachmentFormats=&format;
        VkGraphicsPipelineCreateInfo pipe{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipe.pNext=&rendering;
        pipe.stageCount=2;
        pipe.pStages=stages;
        pipe.pVertexInputState=&vertexInput;
        pipe.pInputAssemblyState=&assembly;
        pipe.pViewportState=&viewport;
        pipe.pRasterizationState=&raster;
        pipe.pMultisampleState=&multisample;
        pipe.pColorBlendState=&blend;
        pipe.pDynamicState=&dynamic;
        pipe.layout=pipelineLayout_;
        checkVk(vkCreateGraphicsPipelines(vulkan_.device(),VK_NULL_HANDLE,
                                           1,&pipe,nullptr,&pipeline_),
                "vkCreateGraphicsPipelines(gt)");
        colorFormat_=format;
    } catch(...) {
        if(fragment)vkDestroyShaderModule(vulkan_.device(),fragment,nullptr);
        vkDestroyShaderModule(vulkan_.device(),vertex,nullptr);
        destroyPipeline();
        throw;
    }
    vkDestroyShaderModule(vulkan_.device(),fragment,nullptr);
    vkDestroyShaderModule(vulkan_.device(),vertex,nullptr);
}

void GtMapCompositor::destroyPipeline() noexcept {
    if(pipeline_)vkDestroyPipeline(vulkan_.device(),pipeline_,nullptr);
    if(pipelineLayout_)vkDestroyPipelineLayout(vulkan_.device(),pipelineLayout_,nullptr);
    pipeline_=VK_NULL_HANDLE;
    pipelineLayout_=VK_NULL_HANDLE;
    colorFormat_=VK_FORMAT_UNDEFINED;
}

bool GtMapCompositor::prepare(
    VkCommandBuffer cmd,std::size_t index,VkFormat format,
    const std::shared_ptr<const media::GtDecodedBundle>& bundle,
    const GtMapStyle& style) {
    if(index>=kSlots || !bundle)return false;
    static_assert(std::endian::native==std::endian::little,
                  "KDM1 scalar bytes are canonical little-endian float32");
    const auto& sky=bundle->sky;
    const auto& depth=bundle->depth;
    const bool useSky=style.sky && style.skyOpacity>0.0F &&
        sky.status==media::GtMapStatus::Ready && sky.raw &&
        sky.width>0 && sky.height>0 &&
        (sky.width*std::uint64_t(sky.height))<=2073600ULL;
    const bool useDepth=style.depth && style.depthOpacity>0.0F &&
        depth.status==media::GtMapStatus::Ready && depth.raw &&
        depth.width>0 && depth.height>0 &&
        depth.raw->size()==std::uint64_t(depth.width)*depth.height*4U;
    if(!useSky && !useDepth)return false;
    auto& slot=slots_[index];
    ensurePipeline(format);
    slot.skyReady=useSky;slot.depthReady=useDepth;
    slot.skyWidth=useSky?sky.width:0U;slot.skyHeight=useSky?sky.height:0U;
    slot.depthWidth=useDepth?depth.width:0U;slot.depthHeight=useDepth?depth.height:0U;
    // Headers and decoder validated the plane size and checksum off-thread.
    slot.validityPlane=useSky &&
        sky.raw->size()==2U*((std::uint64_t(sky.width)*sky.height+7U)/8U);
    const auto skyBytes=useSky?sky.raw->size():std::size_t(1U);
    const VkExtent2D skyExtent=useSky
        ? VkExtent2D{kSkyByteRow,static_cast<std::uint32_t>((skyBytes+kSkyByteRow-1U)/kSkyByteRow)}
        : (slot.sky.image?slot.sky.extent:VkExtent2D{1U,1U});
    const VkExtent2D depthExtent=useDepth
        ? VkExtent2D{depth.width,depth.height}
        : (slot.depth.image?slot.depth.extent:VkExtent2D{1U,1U});
    const bool skyChanged=slot.sky.image==VK_NULL_HANDLE ||
        slot.sky.extent.width!=skyExtent.width ||
        slot.sky.extent.height!=skyExtent.height;
    const bool depthChanged=slot.depth.image==VK_NULL_HANDLE ||
        slot.depth.extent.width!=depthExtent.width ||
        slot.depth.extent.height!=depthExtent.height;
    ensureTexture(slot.sky,VK_FORMAT_R8_UNORM,skyExtent);
    ensureTexture(slot.depth,VK_FORMAT_R32_SFLOAT,depthExtent);
    if(skyChanged || depthChanged)updateDescriptor(slot);

    const auto& source=bundle->identity;
    const auto changed=[&](const Texture& t) {
        return !t.cached || t.epoch!=source.key.streamEpoch ||
               t.frameId!=source.key.frameId ||
               t.sourceTime!=source.sourceTimeNs;
    };
    if((useSky && changed(slot.sky)) || !slot.sky.cached) {
        copyTexture(cmd,slot.sky,useSky?sky.raw->data():nullptr,
                    useSky?sky.raw->size():0U);
        slot.sky.cached=true;
        slot.sky.epoch=source.key.streamEpoch;
        slot.sky.frameId=source.key.frameId;
        slot.sky.sourceTime=source.sourceTimeNs;
    }
    if((useDepth && changed(slot.depth)) || !slot.depth.cached) {
        copyTexture(cmd,slot.depth,useDepth?depth.raw->data():nullptr,
                    useDepth?depth.raw->size():0U);
        slot.depth.cached=true;
        slot.depth.epoch=source.key.streamEpoch;
        slot.depth.frameId=source.key.frameId;
        slot.depth.sourceTime=source.sourceTimeNs;
    }
    return true;
}

void GtMapCompositor::draw(
    VkCommandBuffer cmd,std::size_t index,const GtMapStyle& style) const {
    const auto& slot=slots_.at(index);
    Push push;
    push.controls={std::clamp(style.skyOpacity,0.0F,1.0F),
                   std::clamp(style.depthOpacity,0.0F,1.0F),
                   std::max(0.0F,style.nearM),
                   std::max(style.nearM+0.01F,style.farM)};
    push.dimensions={
        static_cast<std::int32_t>(slot.skyWidth),
        static_cast<std::int32_t>(slot.skyHeight),
        static_cast<std::int32_t>(slot.depthWidth),
        static_cast<std::int32_t>(slot.depthHeight)};
    push.flags={slot.skyReady?1:0,slot.depthReady?1:0,
                slot.validityPlane?1:0,0};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_,0,1,&slot.descriptorSet,0,nullptr);
    vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,sizeof(push),&push);
    vkCmdDraw(cmd,3,1,0,0);
}
} // namespace reg::vulkan
