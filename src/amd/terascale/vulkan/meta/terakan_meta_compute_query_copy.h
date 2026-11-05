/*
 * Copyright © 2026 Terakan Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef TERAKAN_META_COMPUTE_QUERY_COPY_H
#define TERAKAN_META_COMPUTE_QUERY_COPY_H

#include "nir.h"
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_device;
struct terakan_buffer;
struct terakan_gfx_command_writer;
struct terakan_query_pool;

/* Push-constant block layout consumed by the shader built by
 * terakan_meta_build_query_copy_nir(). Mirrors the `push` interface block
 * declared inside the builder so the dispatch-time push matches 1:1.
 */
struct terakan_meta_query_copy_push {
   uint32_t max_rb;
   uint32_t pool_slot_size;
   uint32_t first_query;
   uint32_t query_count;
   uint32_t flags;
   uint32_t elem_size_bytes;
   uint32_t pool_byte_offset;
   uint32_t dst_byte_offset;
   uint32_t dst_stride_bytes;
   uint32_t _pad0;
};

/* SSBO binding indices consumed by the shader. */
#define TERAKAN_META_QUERY_COPY_SSBO_BINDING_POOL 0u
#define TERAKAN_META_QUERY_COPY_SSBO_BINDING_DST  1u

/* VK_QUERY_RESULT_* bits the shader branches on (mirrors VkQueryResultFlagBits). */
#define TERAKAN_META_QUERY_COPY_FLAG_64_BIT               (1u << 0)
#define TERAKAN_META_QUERY_COPY_FLAG_WAIT_BIT             (1u << 1)
#define TERAKAN_META_QUERY_COPY_FLAG_WITH_AVAILABILITY    (1u << 2)
#define TERAKAN_META_QUERY_COPY_FLAG_PARTIAL              (1u << 3)

#define TERAKAN_META_QUERY_COPY_WORKGROUP_SIZE 64u

/* Builds the OCCLUSION vkCmdCopyQueryPoolResults reduction compute shader as a
 * nir_shader. Caller owns the returned pointer and feeds it to
 * terakan_shader_impl_compile (possibly after terakan_shader_lower_and_optimize_post_link
 * with pipeline_layout = NULL, since the shader emits no vulkan_resource_index
 * or load_vulkan_descriptor intrinsics that terakan_nir_lower_bindings would
 * try to walk).
 *
 * `options` must be the compiler options the shader will be compiled against
 * (e.g. terakan_physical_device::nir_options_non_fs). Several standard NIR
 * passes (notably nir_lower_compute_system_values) dereference
 * shader->options; a null here crashes at lowering time, not build time.
 *
 * Returns NULL on allocation failure.
 */
struct nir_shader * terakan_meta_build_query_copy_nir(
   struct nir_shader_compiler_options const * options);

/* Lazy-init contract: `terakan_device_query_copy_shader_init` compiles the
 * NIR shader produced above into `device->query_copy_shader`. On success the
 * shader is ready for dispatch; on failure the slot is re-zeroed, so
 * `terakan_device_query_copy_shader_finish` is a no-op (safe to call on any
 * device whose init reached the shaders BO stage, whether this helper
 * succeeded or not).
 */
VkResult terakan_device_query_copy_shader_init(struct terakan_device * device);
void terakan_device_query_copy_shader_finish(struct terakan_device * device);

/* OCCLUSION branch of vkCmdCopyQueryPoolResults: emits a compute dispatch that
 * sums `end[i] - begin[i]` across render backends for each query in
 * [first_query, first_query + query_count) and writes the result (optionally
 * with an availability word) at `dst_buffer + dst_offset + i * dst_stride`.
 *
 * Preconditions:
 * - `device->query_copy_shader.static_state.program_bo != NULL` (compiled at
 *   device creation).
 * - `pool->type == VK_QUERY_TYPE_OCCLUSION`, `pool->bo != NULL`,
 *   `pool->max_render_backends > 0`.
 * - `dst_buffer->bo != NULL`, `query_count > 0`.
 *
 * Side effects on command_writer:
 * - Pollutes CS kcache slot TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS and compute
 *   resource slots TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS,
 *   R600_IMAGE_REAL_RESOURCE_OFFSET+0. Also perturbs CB_COLOR#1,
 *   CB_TARGET_MASK, CB_COLOR_CONTROL — `restore_cb_state_after_compute`
 *   re-widens before the next graphics draw.
 * - Clears the compute bit of
 *   `push_constants_state.up_to_date_push_constants_bound_to_stages` so the
 *   next app CmdDispatch rebinds its own push constants into the kcache slot.
 * - Accumulates post-dispatch barrier actions (`FLUSH_INV_CB_UAV`,
 *   `PARTIAL_FLUSH_CS`) into `post_color_image_copy_write_barrier_actions`.
 */
void terakan_meta_compute_query_copy_emit(
   struct terakan_gfx_command_writer * command_writer,
   struct terakan_query_pool const * pool,
   struct terakan_buffer const * dst_buffer, VkDeviceSize dst_offset,
   VkDeviceSize dst_stride, uint32_t first_query, uint32_t query_count,
   VkQueryResultFlags flags);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_META_COMPUTE_QUERY_COPY_H */
