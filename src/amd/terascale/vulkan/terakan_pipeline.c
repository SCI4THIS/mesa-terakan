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

#include "terakan_pipeline.h"

#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_pipeline_compute.h"
#include "terakan_pipeline_graphics.h"

#include "util/macros.h"
#include "vk_log.h"
#include "vk_shader_module.h"
#include "vk_util.h"

#include <assert.h>
#include <stddef.h>

void
terakan_CmdBindPipeline(VkCommandBuffer const commandBuffer,
                        VkPipelineBindPoint const pipelineBindPoint,
                        VkPipeline const pipelineHandle)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_pipeline const * const pipeline = terakan_pipeline_from_handle(pipelineHandle);
   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      assert(!pipeline->is_compute);
      terakan_pipeline_graphics_bind(
         command_writer, container_of(pipeline, struct terakan_pipeline_graphics const, base));
   } else {
      assert(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE);
      assert(pipeline->is_compute);
      terakan_pipeline_compute_bind(
         command_writer, container_of(pipeline, struct terakan_pipeline_compute const, base));
   }
}

void
terakan_pipeline_finish(struct terakan_pipeline * const pipeline)
{
   vk_object_base_finish(&pipeline->base);
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyPipeline(UNUSED VkDevice const deviceHandle, VkPipeline const pipelineHandle,
                        VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_pipeline * const pipeline = terakan_pipeline_from_handle(pipelineHandle);

   if (pipeline == NULL) {
      return;
   }

   if (pipeline->is_compute) {
      terakan_pipeline_compute_destroy(
         container_of(pipeline, struct terakan_pipeline_compute, base), pAllocator);
   } else {
      terakan_pipeline_graphics_destroy(
         container_of(pipeline, struct terakan_pipeline_graphics, base), pAllocator);
   }
}

uint32_t const *
terakan_pipeline_stage_spirv(VkPipelineShaderStageCreateInfo const * const stage_info,
                             size_t * const size_bytes_out)
{
   if (stage_info->module != VK_NULL_HANDLE) {
      struct vk_shader_module const * const module =
         vk_shader_module_from_handle(stage_info->module);
      *size_bytes_out = module->size;
      return (uint32_t const *)module->data;
   }

   VkShaderModuleCreateInfo const * const module_info =
      vk_find_struct_const(stage_info->pNext, SHADER_MODULE_CREATE_INFO);
   assert(module_info != NULL);
   *size_bytes_out = module_info->codeSize;
   return module_info->pCode;
}

void
terakan_pipeline_init(struct terakan_pipeline * const pipeline,
                      struct terakan_device * const device, bool const is_compute)
{
   vk_object_base_init(&device->vk, &pipeline->base, VK_OBJECT_TYPE_PIPELINE);

   pipeline->is_compute = is_compute;
}

/* Overrides the vk_runtime common entrypoint for compute pipelines. Same loop shape as
 * `terakan_CreateGraphicsPipelines`: delegate per-pipeline work to `terakan_pipeline_compute_create`
 * and respect `VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR`. `vkCmdDispatch*` still bails
 * on compute today (task #10 sub-step (e) remaining work) — a bound compute pipeline is reachable
 * but unusable until the dispatch packet build and `SQ_PGM_CS` emit function land.
 */
VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateComputePipelines(VkDevice const deviceHandle,
                               UNUSED VkPipelineCache const pipelineCache,
                               uint32_t const createInfoCount,
                               VkComputePipelineCreateInfo const * const pCreateInfos,
                               VkAllocationCallbacks const * const pAllocator,
                               VkPipeline * const pPipelines)
{
   VkResult result = VK_SUCCESS;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   uint32_t pipeline_index;

   for (pipeline_index = 0; pipeline_index < createInfoCount; ++pipeline_index) {
      struct terakan_pipeline_compute * pipeline = NULL;
      VkComputePipelineCreateInfo const * const create_info = &pCreateInfos[pipeline_index];
      VkResult const pipeline_result =
         terakan_pipeline_compute_create(device, create_info, pAllocator, &pipeline);
      if (pipeline_result != VK_SUCCESS) {
         result = pipeline_result;
         if (terakan_pipeline_create_flags(create_info->flags, create_info->pNext) &
             VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) {
            break;
         }
         pPipelines[pipeline_index] = VK_NULL_HANDLE;
         continue;
      }
      pPipelines[pipeline_index] = terakan_pipeline_to_handle(&pipeline->base);
   }

   for (; pipeline_index < createInfoCount; ++pipeline_index) {
      pPipelines[pipeline_index] = VK_NULL_HANDLE;
   }

   return result;
}
