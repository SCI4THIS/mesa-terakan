/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_PIPELINE_COMPUTE_H
#define TERAKAN_PIPELINE_COMPUTE_H

#include "terakan_pipeline.h"
#include "terakan_shader.h"

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute pipeline — paralleling `terakan_pipeline_graphics` but much smaller: compute has a single
 * shader stage (MESA_SHADER_COMPUTE) and no fixed-function state outside the shader itself, so the
 * "static state" infrastructure graphics uses for viewports/blending/rasterizer isn't needed.
 * Dispatch-time state (workgroup counts, RAT/UAV bindings, push constants) is tracked by the
 * command writer rather than baked into the pipeline.
 *
 * Dormant today — `vkCreateComputePipelines` returns `VK_ERROR_FEATURE_NOT_PRESENT`
 * (`terakan_pipeline.c:122-138`); the compile path lands in (d.2) and the factory + dispatch
 * wiring in (e). active-vulkan-1.0.md task #10 sub-step (d.1).
 */
struct terakan_pipeline_compute {
   struct terakan_pipeline base;

   struct terakan_shader_impl shader;

   /* `nir->info.workgroup_size` captured at compile time. Dispatch uses it to compute the total
    * thread count. Each component is bounded by `VK_MAX_COMPUTE_WORKGROUP_SIZE` (1024 on R8xx), so
    * uint16_t is sufficient.
    */
   uint16_t local_workgroup_size[3];
};

struct terakan_device;
struct terakan_gfx_command_writer;

/* To be implemented in (d.2) — SPIR-V → NIR → SFN CS codegen filling `shader.static_state` with
 * PGM_START_CS / `R_0288D4_SQ_PGM_RESOURCES_LS` / `R_028F1C_SQ_LDS_ALLOC` and `local_workgroup_size`
 * from `nir->info.workgroup_size`.
 */
VkResult terakan_pipeline_compute_create(struct terakan_device * device,
                                         VkComputePipelineCreateInfo const * create_info,
                                         VkAllocationCallbacks const * allocator,
                                         struct terakan_pipeline_compute ** pipeline_out);

/* To be implemented in (d.3) — materializes the compute-stage SQ register set referenced as
 * pending by `terakan_push_constants.c:135-142` and `terakan_pipeline_layout.c:65`.
 */
void terakan_pipeline_compute_bind(struct terakan_gfx_command_writer * command_writer,
                                   struct terakan_pipeline_compute const * pipeline);

void terakan_pipeline_compute_destroy(struct terakan_pipeline_compute * pipeline,
                                      VkAllocationCallbacks const * allocator);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PIPELINE_COMPUTE_H */
