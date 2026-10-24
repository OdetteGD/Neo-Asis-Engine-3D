// ===========================================================================
//  CommandList — thin recording helper over VkCommandBuffer.
//
//  Everything here is header-inline so the hot path inlines away. The intent is
//  that renderer passes read like a script, not like Vulkan boilerplate.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/gfx/VulkanContext.h"

#include <volk.h>

namespace neo {

class CommandList {
public:
  CommandList() = default;
  explicit CommandList(VkCommandBuffer cmd) : cmd_(cmd) {}

  [[nodiscard]] VkCommandBuffer raw() const { return cmd_; }
  [[nodiscard]] explicit operator bool() const { return cmd_ != VK_NULL_HANDLE; }

  // --- Lifetime ----------------------------------------------------------
  void begin(VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) const {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = flags;
    NEO_VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
  }
  void end() const { NEO_VK_CHECK(vkEndCommandBuffer(cmd_)); }

  // --- Dynamic rendering (no VkRenderPass objects anywhere) --------------
  void begin_rendering(const VkRenderingInfo& info) const {
    vkCmdBeginRendering(cmd_, &info);
  }
  void end_rendering() const { vkCmdEndRendering(cmd_); }

  void set_viewport(f32 x, f32 y, f32 w, f32 h, f32 min_depth = 0.0f,
                    f32 max_depth = 1.0f) const {
    const VkViewport vp{x, y, w, h, min_depth, max_depth};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
  }
  void set_scissor(i32 x, i32 y, u32 w, u32 h) const {
    const VkRect2D r{{x, y}, {w, h}};
    vkCmdSetScissor(cmd_, 0, 1, &r);
  }

  // --- Pipelines / descriptors -------------------------------------------
  void bind_pipeline(VkPipeline pipeline) const { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); }
  void bind_compute(VkPipeline pipeline) const { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); }

  void bind_vertex_buffer(VkBuffer buffer, VkDeviceSize offset = 0) const {
    vkCmdBindVertexBuffers(cmd_, 0, 1, &buffer, &offset);
  }
  void bind_index_buffer(VkBuffer buffer, VkDeviceSize offset = 0,
                         VkIndexType type = VK_INDEX_TYPE_UINT32) const {
    vkCmdBindIndexBuffer(cmd_, buffer, offset, type);
  }

  void bind_descriptor_sets(VkPipelineLayout layout, u32 first, u32 count,
                            const VkDescriptorSet* sets) const {
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, sets, 0,
                            nullptr);
  }
  void bind_compute_descriptor_sets(VkPipelineLayout layout, u32 first, u32 count,
                                    const VkDescriptorSet* sets) const {
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout, first, count, sets, 0,
                            nullptr);
  }

  void push_constants(VkPipelineLayout layout, VkShaderStageFlags stages, u32 size,
                      const void* data, u32 offset = 0) const {
    vkCmdPushConstants(cmd_, layout, stages, offset, size, data);
  }

  // --- Draws -------------------------------------------------------------
  void draw(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0,
            u32 first_instance = 0) const {
    vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, first_instance);
  }
  void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0,
                    i32 vertex_offset = 0, u32 first_instance = 0) const {
    vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
  }
  void draw_indexed_indirect(VkBuffer buffer, VkDeviceSize offset, u32 draw_count,
                             VkDeviceSize stride) const {
    vkCmdDrawIndexedIndirect(cmd_, buffer, offset, draw_count, stride);
  }
  void dispatch(u32 x, u32 y = 1, u32 z = 1) const { vkCmdDispatch(cmd_, x, y, z); }

  // --- Barriers (synchronization2 style, no legacy access flags) ---------
  void image_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                     VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                     u32 base_mip = 0, u32 mip_count = VK_REMAINING_MIP_LEVELS,
                     u32 base_layer = 0, u32 layer_count = VK_REMAINING_ARRAY_LAYERS) const {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, base_mip, mip_count, base_layer, layer_count};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
  }

  void memory_barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                      VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) const {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
  }

  // --- Debug -------------------------------------------------------------
  void label_begin(const VulkanContext& ctx, const char* name, vec4 color) const {
    ctx.begin_debug_label(cmd_, name, color);
  }
  void label_end(const VulkanContext& ctx) const { ctx.end_debug_label(cmd_); }

private:
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
};

} // namespace neo
