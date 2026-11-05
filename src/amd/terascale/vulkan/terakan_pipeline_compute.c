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

#include "terakan_pipeline_compute.h"

#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_hw_state.h"
#include "terakan_pipeline_layout.h"
#include "terakan_shader.h"

#include "gallium/drivers/r600/r600_shader_common.h"
#include "util/ralloc.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_log.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

VkResult
terakan_pipeline_compute_create(struct terakan_device * const device,
                                VkComputePipelineCreateInfo const * const create_info,
                                VkAllocationCallbacks const * const allocator,
                                struct terakan_pipeline_compute ** const pipeline_out)
{
   VkResult result;

   struct terakan_pipeline_compute * const pipeline =
      vk_alloc2(&device->vk.alloc, allocator, sizeof(struct terakan_pipeline_compute),
                alignof(struct terakan_pipeline_compute), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   terakan_pipeline_init(&pipeline->base, device, true);

   struct terakan_pipeline_layout const * const pipeline_layout =
      terakan_pipeline_layout_from_handle(create_info->layout);

   VkPipelineShaderStageCreateInfo const * const stage_info = &create_info->stage;
   assert(stage_info->stage == VK_SHADER_STAGE_COMPUTE_BIT);

   size_t spirv_size_bytes;
   uint32_t const * const spirv = terakan_pipeline_stage_spirv(stage_info, &spirv_size_bytes);
   assert(spirv != NULL);

   nir_shader * nir =
      terakan_shader_spirv_to_nir(device, spirv_size_bytes, spirv, MESA_SHADER_COMPUTE,
                                  stage_info->pName, stage_info->pSpecializationInfo);
   if (nir == NULL) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_pipeline;
   }

   struct terakan_shader_impl * const shader = &pipeline->shader;
   memset(shader, 0, sizeof(*shader));

   shader->push_constants_usage.app_extent_bytes =
      pipeline_layout->shader_app_push_constants_extents_bytes[MESA_SHADER_COMPUTE];

   /* `fragment_data_uncompacted_locations_out = NULL` — only the fragment stage contributes to
    * that output (terakan_shader.c:243-245 guards the write). Compute never populates it.
    */
   terakan_shader_lower_and_optimize_post_link(nir, pipeline_layout, shader->resources_needed,
                                               &shader->samplers_needed,
                                               &shader->push_constants_usage.driver_constants,
                                               NULL);

   /* Compute uses an empty shader key: no `union r600_shader_key` field gates a compute variant
    * (every keyed field addresses fixed-function GL state that compute does not involve).
    */
   union r600_shader_key const shader_key = {};
   result = terakan_shader_impl_compile(shader, device, &shader_key, nir, allocator);
   if (result != VK_SUCCESS) {
      ralloc_free(nir);
      goto fail_pipeline;
   }

   /* `nir->info.workgroup_size` is populated by SPIR-V → NIR from the `LocalSize` / `LocalSizeId`
    * decorations. Each axis is ≤ `VK_MAX_COMPUTE_WORKGROUP_SIZE` (1024 on R8xx), which fits in
    * `uint16_t`. The shader-level copy is written by `terakan_shader_impl_compile`'s CS case for
    * `_emit_sq_pgm_cs`; this pipeline-level copy is for convenient access at dispatch time.
    */
   pipeline->local_workgroup_size[0] = (uint16_t)nir->info.workgroup_size[0];
   pipeline->local_workgroup_size[1] = (uint16_t)nir->info.workgroup_size[1];
   pipeline->local_workgroup_size[2] = (uint16_t)nir->info.workgroup_size[2];

   ralloc_free(nir);

   *pipeline_out = pipeline;
   return VK_SUCCESS;

fail_pipeline:
   terakan_pipeline_finish(&pipeline->base);
   vk_free2(&device->vk.alloc, allocator, pipeline);
   return result;
}

void
terakan_pipeline_compute_bind(struct terakan_gfx_command_writer * const command_writer,
                              struct terakan_pipeline_compute const * const pipeline)
{
   /* Writes `hw_state_draw.sq_pgm_cs` directly rather than going through a
    * `terakan_state_draw`-level intermediate because compute has no stage-remap variant: unlike
    * VS (which routes through `sq_pgm_ls_es_gs_vs.vs_as_vs` / `.vs_as_ls` / `.vs_as_es` and an
    * apply function to materialize the final `hw_state_draw.sq_pgm_vs`), the CS logical stage maps
    * one-to-one to the CS hardware pipeline stage. The emit function at
    * `emit_functions[TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_CS]` emits
    * `R_0288D0_SQ_PGM_START_LS` + `SQ_PGM_RESOURCES_LS` + `SQ_PGM_RESOURCES_2_LS`. The
    * `state_modified` bit is set here but only consumed if a subsequent graphics draw invokes
    * `terakan_hw_state_draw_emit_modified`; `vkCmdDispatch*` itself is still a no-op pending task
    * #10 sub-step (e)'s dispatch packet build (`SQ_LDS_ALLOC` + `DISPATCH_DIRECT`).
    */
   struct terakan_hw_state_draw * const state = &command_writer->hw_state_draw;
   struct terakan_shader_static const * const cs = &pipeline->shader.static_state;
   if (state->sq_pgm_cs != cs) {
      state->sq_pgm_cs = cs;
      BITSET_SET(state->state_modified, TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_CS);
      BITSET_SET(state->state_ever_written, TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_CS);
   }
}

void
terakan_pipeline_compute_destroy(struct terakan_pipeline_compute * const pipeline,
                                 VkAllocationCallbacks const * allocator)
{
   if (allocator == NULL) {
      allocator = &pipeline->base.base.device->alloc;
   }

   terakan_shader_impl_finish(&pipeline->shader, allocator);

   terakan_pipeline_finish(&pipeline->base);

   vk_free(allocator, pipeline);
}
