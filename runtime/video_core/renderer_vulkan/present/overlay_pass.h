// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include "video_core/overlay_draw_data.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
struct Frame;
class MemoryAllocator;
class Scheduler;

/// Draws the frontend overlay on top of a presented frame, after the window
/// adapt pass (so after FSR, at window resolution). Everything goes through the
/// scheduler's command buffer: the font upload, and vertex and index buffers
/// that are reused only once the scheduler tick that last read them is done.
class OverlayPass final {
public:
    explicit OverlayPass(const Device& device, MemoryAllocator& allocator, Scheduler& scheduler,
                         VkFormat frame_format, const VideoCore::OverlayFontAtlas& font);
    ~OverlayPass();

    VkFormat Format() const {
        return format;
    }

    void Draw(Frame& frame, VideoCore::OverlayDrawData data);

private:
    struct Slot {
        vk::Buffer vertices;
        vk::Buffer indices;
        size_t vertex_capacity = 0;
        size_t index_capacity = 0;
        u64 tick = 0;
    };

    Slot& AcquireSlot();
    void Reserve(vk::Buffer& buffer, size_t& capacity, size_t bytes, VkBufferUsageFlags usage);

    const Device& device;
    MemoryAllocator& allocator;
    Scheduler& scheduler;
    VkFormat format;

    vk::RenderPass render_pass;
    vk::DescriptorSetLayout descriptor_set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;
    vk::Pipeline pipeline;
    vk::Image font_image;
    vk::ImageView font_view;
    vk::Sampler sampler;
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSets descriptor_sets;

    std::array<Slot, 8> slots;
    size_t next_slot = 0;
};

} // namespace Vulkan
