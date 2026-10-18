// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <cstring>

#include "video_core/host_shaders/vulkan_overlay_frag_spv.h"
#include "video_core/host_shaders/vulkan_overlay_vert_spv.h"
#include "video_core/renderer_vulkan/present/overlay_pass.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

namespace {
struct OverlayPushConstants {
    float scale[2];
    float translate[2];
};

vk::Pipeline CreateOverlayPipeline(const Device& device, VkRenderPass render_pass,
                                   VkPipelineLayout layout, VkShaderModule vertex,
                                   VkShaderModule fragment) {
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment,
         .pName = "main"},
    }};
    const VkVertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(VideoCore::OverlayVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = offsetof(VideoCore::OverlayVertex, x)},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = offsetof(VideoCore::OverlayVertex, u)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .offset = offsetof(VideoCore::OverlayVertex, rgba)},
    }};
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    // Straight alpha, as Dear ImGui produces it.
    const VkPipelineColorBlendAttachmentState blend{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend,
    };
    constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    return device.GetLogical().CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic,
        .layout = layout,
        .renderPass = render_pass,
        .subpass = 0,
    });
}
} // namespace

OverlayPass::OverlayPass(const Device& device_, MemoryAllocator& allocator_, Scheduler& scheduler_,
                         VkFormat frame_format, const VideoCore::OverlayFontAtlas& font)
    : device{device_}, allocator{allocator_}, scheduler{scheduler_}, format{frame_format} {
    // Initial layout GENERAL: the pass loads what the window adapt pass drew.
    render_pass = CreateWrappedRenderPass(device, format, VK_IMAGE_LAYOUT_GENERAL);
    descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(OverlayPushConstants),
    };
    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    vertex_shader = BuildShader(device, VULKAN_OVERLAY_VERT_SPV);
    fragment_shader = BuildShader(device, VULKAN_OVERLAY_FRAG_SPV);
    pipeline = CreateOverlayPipeline(device, *render_pass, *pipeline_layout, *vertex_shader,
                                     *fragment_shader);

    const VkExtent2D font_extent{font.width, font.height};
    font_image = CreateWrappedImage(allocator, font_extent, VK_FORMAT_R8G8B8A8_UNORM);
    UploadImage(device, allocator, scheduler, font_image, font_extent, VK_FORMAT_R8G8B8A8_UNORM,
                font.rgba);
    font_view = CreateWrappedImageView(device, font_image, VK_FORMAT_R8G8B8A8_UNORM);
    sampler = CreateBilinearSampler(device);
    descriptor_pool = CreateWrappedDescriptorPool(device, 1, 1);
    descriptor_sets = CreateWrappedDescriptorSets(descriptor_pool, {*descriptor_set_layout});
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(1);
    const VkWriteDescriptorSet write =
        CreateWriteDescriptorSet(images, *sampler, *font_view, descriptor_sets[0], 0);
    device.GetLogical().UpdateDescriptorSets(std::array{write}, {});
}

OverlayPass::~OverlayPass() = default;

void OverlayPass::Reserve(vk::Buffer& buffer, size_t& capacity, size_t bytes,
                          VkBufferUsageFlags usage) {
    if (bytes <= capacity) return;
    capacity = std::max<size_t>(bytes * 2, 64 * 1024);
    buffer = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = capacity,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        },
        MemoryUsage::Upload);
}

OverlayPass::Slot& OverlayPass::AcquireSlot() {
    Slot& slot = slots[next_slot];
    next_slot = (next_slot + 1) % slots.size();
    // Eight frames back: in practice long done. Never overwrite what the GPU
    // may still be reading.
    if (slot.tick != 0 && !scheduler.IsFree(slot.tick)) {
        scheduler.Wait(slot.tick);
    }
    return slot;
}

void OverlayPass::Draw(Frame& frame, VideoCore::OverlayDrawData data) {
    if (data.empty() || frame.width == 0 || frame.height == 0) return;
    Slot& slot = AcquireSlot();
    const size_t vertex_bytes = data.vertices.size() * sizeof(VideoCore::OverlayVertex);
    const size_t index_bytes = data.indices.size() * sizeof(u16);
    Reserve(slot.vertices, slot.vertex_capacity, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    Reserve(slot.indices, slot.index_capacity, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::memcpy(slot.vertices.Mapped().data(), data.vertices.data(), vertex_bytes);
    std::memcpy(slot.indices.Mapped().data(), data.indices.data(), index_bytes);
    slot.vertices.Flush();
    slot.indices.Flush();
    slot.tick = scheduler.CurrentTick();

    // The draw data was built for data.width x data.height; map that onto the frame.
    const OverlayPushConstants push{
        .scale = {2.0f / data.width, 2.0f / data.height},
        .translate = {-1.0f, -1.0f},
    };
    const float sx = static_cast<float>(frame.width) / data.width;
    const float sy = static_cast<float>(frame.height) / data.height;
    std::vector<VkRect2D> scissors;
    scissors.reserve(data.commands.size());
    for (const auto& command : data.commands) {
        const s32 left = static_cast<s32>(command.clip[0] * sx);
        const s32 top = static_cast<s32>(command.clip[1] * sy);
        const s32 right = std::min(static_cast<s32>(command.clip[2] * sx + 0.5f), static_cast<s32>(frame.width));
        const s32 bottom = std::min(static_cast<s32>(command.clip[3] * sy + 0.5f), static_cast<s32>(frame.height));
        scissors.push_back({{std::max(left, 0), std::max(top, 0)},
                            {static_cast<u32>(std::max(right - left, 0)),
                             static_cast<u32>(std::max(bottom - top, 0))}});
    }

    const VkRenderPass pass = *render_pass;
    const VkFramebuffer framebuffer = *frame.framebuffer;
    const VkExtent2D extent{frame.width, frame.height};
    const VkPipeline graphics_pipeline = *pipeline;
    const VkPipelineLayout layout = *pipeline_layout;
    const VkDescriptorSet set = descriptor_sets[0];
    const VkBuffer vertex_buffer = *slot.vertices;
    const VkBuffer index_buffer = *slot.indices;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=, commands = std::move(data.commands),
                      scissors = std::move(scissors)](vk::CommandBuffer cmdbuf) {
        BeginRenderPass(cmdbuf, pass, framebuffer, extent);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, set, {});
        cmdbuf.BindVertexBuffer(0, vertex_buffer, 0);
        cmdbuf.BindIndexBuffer(index_buffer, 0, VK_INDEX_TYPE_UINT16);
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT, push);
        for (size_t i = 0; i < commands.size(); ++i) {
            if (scissors[i].extent.width == 0 || scissors[i].extent.height == 0) continue;
            cmdbuf.SetScissor(0, scissors[i]);
            cmdbuf.DrawIndexed(commands[i].index_count, 1, commands[i].index_offset,
                               commands[i].vertex_offset, 0);
        }
        cmdbuf.EndRenderPass();
    });
}

} // namespace Vulkan
