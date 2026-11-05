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

/* Query pool scaffolding. Occlusion and timestamp (the Vulkan 1.0 baseline) are being brought up
 * in staged iterations; see terakan_query.h for the per-slot BO layout.
 *
 * TIMESTAMP is end-to-end: lifetime + reset + CmdWriteTimestamp (TIMESTAMP + fence EOP pair) +
 * GetQueryPoolResults (host read from mapped BO, with a spin for WAIT_BIT) + CmdCopyQueryPoolResults
 * (WAIT_REG_MEM on the fence followed by a CP DMA copy of 4 or 8 bytes, with the availability word
 * written via CP DMA fill once the fence has landed).
 *
 * OCCLUSION lifetime + reset + Begin/End + GetQueryPoolResults are wired up. Begin emits a single
 * ZPASS_DONE EVENT_WRITE at the slot base; HW scatters the per-RB begin counter across the
 * `16 * max_render_backends`-byte range (one 64-bit value every 16 bytes). End emits ZPASS_DONE at
 * `slot_base + 8` so HW writes the per-RB end counters into the matching +8 half of each pair,
 * then emits a VALUE_32BIT EVENT_WRITE_EOP writing TERAKAN_QUERY_FENCE_READY to the fence word at
 * `slot_base + 16 * max_render_backends`. GetQueryPoolResults sums `end[i] - begin[i]` across RBs.
 * CmdCopyQueryPoolResults for OCCLUSION dispatches `terakan_meta_compute_query_copy_emit`, which
 * runs the compute reduction shader and writes the Σ (end - begin) result (plus an optional
 * availability word) to the destination buffer.
 */

#include "terakan_entrypoints.h"
#include "terakan_query.h"

#include "terakan_barrier.h"
#include "terakan_bo.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_cp_dma.h"
#include "terakan_device.h"
#include "meta/terakan_meta_compute_query_copy.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "util/u_atomic.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateQueryPool(VkDevice const deviceHandle,
                        VkQueryPoolCreateInfo const * const pCreateInfo,
                        VkAllocationCallbacks const * const pAllocator,
                        VkQueryPool * const pQueryPool)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);
   uint32_t const max_render_backends =
      terakan_device_physical_device(device)->max_render_backends;

   uint32_t const slot_size =
      terakan_query_pool_slot_size_for_type(pCreateInfo->queryType, max_render_backends);
   if (slot_size == 0) {
      /* OCCLUSION with no detected RBs (WDDM today), pipeline statistics, and all post-1.0 query
       * types are not supported.
       */
      *pQueryPool = VK_NULL_HANDLE;
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   struct terakan_query_pool * const pool =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(struct terakan_query_pool),
                alignof(struct terakan_query_pool), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL) {
      *pQueryPool = VK_NULL_HANDLE;
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_QUERY_POOL);

   pool->type = pCreateInfo->queryType;
   pool->query_count = pCreateInfo->queryCount;
   pool->slot_size = slot_size;
   pool->max_render_backends =
      pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION ? max_render_backends : 0;
   pool->bo = NULL;
   pool->va = 0;

   /* EVENT_WRITE_EOP writes a 64-bit value and requires an 8-byte-aligned destination address. */
   VkDeviceSize const bo_size = (VkDeviceSize)pCreateInfo->queryCount * slot_size;
   VkResult const bo_result = device->winsys_fn->bo->allocate_device_memory(
      device, bo_size, 8,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &pool->bo);
   if (bo_result != VK_SUCCESS) {
      vk_object_base_finish(&pool->base);
      vk_free2(&device->vk.alloc, pAllocator, pool);
      *pQueryPool = VK_NULL_HANDLE;
      return vk_error(device, bo_result);
   }
   pool->va = pool->bo->va;

   *pQueryPool = terakan_query_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyQueryPool(VkDevice const deviceHandle, VkQueryPool const queryPool,
                         VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_query_pool * const pool = terakan_query_pool_from_handle(queryPool);
   if (pool == NULL) {
      return;
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   if (pool->bo != NULL) {
      terakan_bo_free(pool->bo, pAllocator);
   }

   vk_object_base_finish(&pool->base);
   vk_free2(&device->vk.alloc, pAllocator, pool);
}

/* Emits one PKT3_EVENT_WRITE_EOP of EVENT_TYPE_BOTTOM_OF_PIPE_TS writing to `va`. `data_sel` picks
 * what gets stored (3=TIMESTAMP writes the 64-bit CP counter and ignores data_{lo,hi}; 1=VALUE_32BIT
 * writes `data_lo` as a 32-bit value); the DST BO must be an 8-byte-aligned location in `pool->bo`.
 */
static void
terakan_query_emit_eop(struct terakan_gfx_command_writer * const command_writer,
                       struct terakan_bo const * const pool_bo, uint64_t const va,
                       uint32_t const data_sel, uint32_t const data_lo)
{
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(command_writer, 6, false, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
   *packet++ = EVENT_TYPE(EVENT_TYPE_BOTTOM_OF_PIPE_TS) | EVENT_INDEX(5);
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)va;                                       /* ADDRESS_LO */
   *packet++ = ((va >> 32) & 0xFF) | EOP_DATA_SEL(data_sel);       /* ADDRESS_HI | DATA_SEL */
   *packet++ = data_lo;                                            /* DATA_LO */
   *packet++ = 0;                                                  /* DATA_HI */
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer, pool_bo,
                                                false, true, TERAKAN_BO_PRIORITY_QUERY));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

/* Emits a 7-dword PKT3_WAIT_REG_MEM that stalls the CP ME until `*fence_va == FENCE_READY`.
 * Used by CmdCopyQueryPoolResults to serialise GPU-side result copies behind the fence written by
 * CmdWriteTimestamp / CmdEndQuery, so the destination buffer never observes a torn or stale value.
 */
static void
terakan_query_emit_wait_reg_mem_fence(struct terakan_gfx_command_writer * const command_writer,
                                      struct terakan_bo const * const pool_bo,
                                      uint64_t const fence_va)
{
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(command_writer, 7, false, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_WAIT_REG_MEM, 5, 0);
   *packet++ = WAIT_REG_MEM_EQUAL | WAIT_REG_MEM_MEM_SPACE(1);
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)fence_va;          /* ADDRESS_LO */
   *packet++ = (fence_va >> 32) & 0xFF;     /* ADDRESS_HI */
   *packet++ = TERAKAN_QUERY_FENCE_READY;   /* reference */
   *packet++ = 0xFFFFFFFFu;                 /* mask */
   *packet++ = 4;                           /* poll interval (CP cycles) */
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_WAIT_REG_MEM_LO, TERASCALE_WDDM_PATCH_IDS_WAIT_REG_MEM_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer, pool_bo,
                                                true, false, TERAKAN_BO_PRIORITY_QUERY));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

/* Emits a 4-dword PKT3_EVENT_WRITE for ZPASS_DONE writing the current ZPASS counter(s) to `va`.
 * The HW scatters per-RB 64-bit values at `va + 16 * rb_index` for each enabled render backend.
 */
static void
terakan_query_emit_event_write_zpass_done(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_bo const * const pool_bo, uint64_t const va)
{
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(command_writer, 4, false, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_EVENT_WRITE, 2, 0);
   *packet++ = EVENT_TYPE(EVENT_TYPE_ZPASS_DONE) | EVENT_INDEX(1);
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)va;                 /* ADDRESS_LO */
   *packet++ = (va >> 32) & 0xFF;            /* ADDRESS_HI */
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer, pool_bo,
                                                false, true, TERAKAN_BO_PRIORITY_QUERY));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBeginQuery(VkCommandBuffer const commandBuffer, VkQueryPool const queryPool,
                      uint32_t const query, UNUSED VkQueryControlFlags const flags)
{
   struct terakan_query_pool const * const pool = terakan_query_pool_from_handle(queryPool);
   if (pool == NULL || pool->bo == NULL || pool->type != VK_QUERY_TYPE_OCCLUSION) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   /* VK_QUERY_CONTROL_PRECISE_BIT is a no-op: ZPASS_DONE always returns exact per-sample counts on
    * Evergreen regardless of this hint, which already satisfies the Vulkan contract (approximate
    * samples_passed when the bit is clear is a permitted *lower bound* on precision).
    */
   uint64_t const va = pool->va + (uint64_t)query * pool->slot_size;
   terakan_query_emit_event_write_zpass_done(command_writer, pool->bo, va);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdEndQuery(VkCommandBuffer const commandBuffer, VkQueryPool const queryPool,
                    uint32_t const query)
{
   struct terakan_query_pool const * const pool = terakan_query_pool_from_handle(queryPool);
   if (pool == NULL || pool->bo == NULL || pool->type != VK_QUERY_TYPE_OCCLUSION) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   /* ZPASS_DONE at `slot + 8` so each RB's end counter lands in the matching +8 half of its 16-byte
    * pair; the host-side result is `sum_i (end[i] - begin[i])`. Then a VALUE_32BIT EOP writes the
    * fence sentinel at `slot + 16 * max_render_backends`; because it is a bottom-of-pipe event the
    * CP serialises it after the ZPASS_DONE write has retired, which is exactly what the host poll
    * in GetQueryPoolResults relies on.
    */
   uint64_t const slot_va = pool->va + (uint64_t)query * pool->slot_size;
   terakan_query_emit_event_write_zpass_done(command_writer, pool->bo, slot_va + 8);
   terakan_query_emit_eop(command_writer, pool->bo,
                          slot_va + 16ull * pool->max_render_backends, 1 /* VALUE_32BIT */,
                          TERAKAN_QUERY_FENCE_READY);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdResetQueryPool(VkCommandBuffer const commandBuffer, VkQueryPool const queryPool,
                          uint32_t const firstQuery, uint32_t const queryCount)
{
   if (queryCount == 0) {
      return;
   }

   struct terakan_query_pool const * const pool = terakan_query_pool_from_handle(queryPool);
   if (pool == NULL || pool->bo == NULL) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   VkDeviceSize const offset = (VkDeviceSize)firstQuery * pool->slot_size;
   VkDeviceSize const size = (VkDeviceSize)queryCount * pool->slot_size;
   terakan_cp_dma_fill(command_writer, pool->bo, pool->va + offset, TERAKAN_BO_PRIORITY_QUERY, size,
                       0);
   command_writer->post_buffer_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdWriteTimestamp(VkCommandBuffer const commandBuffer,
                          UNUSED VkPipelineStageFlagBits const pipelineStage,
                          VkQueryPool const queryPool, uint32_t const query)
{
   struct terakan_query_pool const * const pool = terakan_query_pool_from_handle(queryPool);
   if (pool == NULL || pool->bo == NULL || pool->type != VK_QUERY_TYPE_TIMESTAMP) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   /* The spec allows pipelineStage to be any single stage; always emit BOTTOM_OF_PIPE_TS so the
    * timestamp reflects completion of all prior work in the queue, which is the maximally correct
    * approximation when the exact HW counterpart of the requested stage is not plumbed.
    *
    * Two EOPs: timestamp at `va + 0`, fence sentinel at `va + 8`. Both are BOTTOM_OF_PIPE events,
    * so the HW serialises them against the preceding pipeline; because the CP processes EOP writes
    * in order of issue, the fence lands only after the timestamp has been retired, which is what
    * the host-side GetQueryPoolResults poll relies on.
    */
   uint64_t const va = pool->va + (uint64_t)query * pool->slot_size;
   terakan_query_emit_eop(command_writer, pool->bo, va, 3 /* TIMESTAMP */, 0);
   terakan_query_emit_eop(command_writer, pool->bo, va + 8, 1 /* VALUE_32BIT */,
                          TERAKAN_QUERY_FENCE_READY);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyQueryPoolResults(VkCommandBuffer const commandBuffer,
                                VkQueryPool const queryPool, uint32_t const firstQuery,
                                uint32_t const queryCount, VkBuffer const dstBuffer,
                                VkDeviceSize const dstOffset, VkDeviceSize const stride,
                                VkQueryResultFlags const flags)
{
   struct terakan_query_pool const * const pool = terakan_query_pool_from_handle(queryPool);
   if (pool == NULL || pool->bo == NULL || queryCount == 0) {
      return;
   }

   struct terakan_buffer const * const dst_buffer = terakan_buffer_from_handle(dstBuffer);
   if (dst_buffer == NULL || dst_buffer->bo == NULL) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   /* OCCLUSION needs a per-RB begin/end reduction that the CP cannot do on its own: each slot
    * holds `max_render_backends` {begin,end} 64-bit pairs plus a fence word, and the result for a
    * single query is Σ (end[i] - begin[i]). The compute meta pass summed by the shader built in
    * `meta/terakan_meta_compute_query_copy.c` handles every VK_QUERY_RESULT_* combination the spec
    * allows — including WAIT_BIT (the shader spins on the fence inside the dispatch) and
    * WITH_AVAILABILITY. The compiled device-global shader (`device->query_copy_shader`) is required;
    * terakan_device_init guarantees it is present by this point.
    */
   if (pool->type == VK_QUERY_TYPE_OCCLUSION) {
      terakan_meta_compute_query_copy_emit(command_writer, pool, dst_buffer, dstOffset, stride,
                                           firstQuery, queryCount, flags);
      return;
   }

   /* TIMESTAMP writes a single 64-bit value at slot+0 plus a 32-bit fence at slot+8, which the CP
    * can serialise directly via WAIT_REG_MEM + CP DMA — no compute needed.
    */
   if (pool->type != VK_QUERY_TYPE_TIMESTAMP) {
      return;
   }

   bool const result_64 = (flags & VK_QUERY_RESULT_64_BIT) != 0;
   bool const wait = (flags & VK_QUERY_RESULT_WAIT_BIT) != 0;
   bool const with_availability = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   VkDeviceSize const element_size = result_64 ? 8u : 4u;

   /* with_availability without wait would need either a conditional write that converts the fence
    * sentinel into {0,1}, or a compute shader. Evergreen's CP has neither in a form we can plumb
    * here, so promote with_availability to wait: once the fence has landed, availability is
    * unambiguously 1, which is spec-compliant (the standard permits 0 only until the fence has
    * been written). The stall cost will go away once the compute reduction path lands.
    */
   bool const emit_wait = wait || with_availability;

   for (uint32_t index = 0; index < queryCount; ++index) {
      uint32_t const query_index = firstQuery + index;
      uint64_t const slot_va = pool->va + (uint64_t)query_index * pool->slot_size;
      uint64_t const fence_va = slot_va + 8;
      uint64_t const dst_va = dst_buffer->va + dstOffset + (uint64_t)index * stride;

      if (emit_wait) {
         terakan_query_emit_wait_reg_mem_fence(command_writer, pool->bo, fence_va);
      }

      terakan_cp_dma_copy(command_writer, pool->bo, slot_va, TERAKAN_BO_PRIORITY_QUERY,
                          dst_buffer->bo, dst_va, TERAKAN_BO_PRIORITY_QUERY, element_size);

      if (with_availability) {
         /* 32-bit fill replicates its 32-bit value across the destination, so a single fill of 8
          * bytes with value 1 would write 0x00000001_00000001 - not the required 64-bit 1. Split
          * into a low fill of value 1 and a high fill of value 0 for the 64-bit case.
          */
         terakan_cp_dma_fill(command_writer, dst_buffer->bo, dst_va + element_size,
                             TERAKAN_BO_PRIORITY_QUERY, 4, 1);
         if (result_64) {
            terakan_cp_dma_fill(command_writer, dst_buffer->bo, dst_va + element_size + 4,
                                TERAKAN_BO_PRIORITY_QUERY, 4, 0);
         }
      }
   }

   command_writer->post_buffer_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA;
}

/* Writes one result element into `dst` using the integer width implied by `flags`. */
static void
terakan_query_write_result_element(void * const dst, VkQueryResultFlags const flags,
                                   uint64_t const value)
{
   if (flags & VK_QUERY_RESULT_64_BIT) {
      uint64_t const value_64 = value;
      memcpy(dst, &value_64, sizeof(uint64_t));
   } else {
      uint32_t const value_32 = (uint32_t)value;
      memcpy(dst, &value_32, sizeof(uint32_t));
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_GetQueryPoolResults(VkDevice const deviceHandle, VkQueryPool const queryPool,
                            uint32_t const firstQuery, uint32_t const queryCount,
                            size_t const dataSize, void * const pData, VkDeviceSize const stride,
                            VkQueryResultFlags const flags)
{
   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_query_pool * const pool = terakan_query_pool_from_handle(queryPool);
   if (pool == NULL || pool->bo == NULL || queryCount == 0) {
      return VK_SUCCESS;
   }
   if (pool->type != VK_QUERY_TYPE_OCCLUSION && pool->type != VK_QUERY_TYPE_TIMESTAMP) {
      if (pData != NULL && dataSize != 0) {
         memset(pData, 0, dataSize);
      }
      return VK_SUCCESS;
   }

   uint8_t * const pool_mapping = terakan_bo_map(pool->bo);
   if (unlikely(pool_mapping == NULL)) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   size_t const element_size = (flags & VK_QUERY_RESULT_64_BIT) ? sizeof(uint64_t) : sizeof(uint32_t);
   bool const with_availability = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   bool const wait = (flags & VK_QUERY_RESULT_WAIT_BIT) != 0;
   bool const partial = (flags & VK_QUERY_RESULT_PARTIAL_BIT) != 0;

   /* The fence word sits right after the data payload. For TIMESTAMP it is at `slot + 8`; for
    * OCCLUSION HW writes `max_render_backends` 16-byte begin/end pairs, so the fence lives at
    * `slot + 16 * max_render_backends` (matching r600_query.c's `max_rb * 16 - 8` offset from the
    * post-End `va + 8` base).
    */
   size_t const fence_offset = pool->type == VK_QUERY_TYPE_OCCLUSION
                                  ? (size_t)16u * pool->max_render_backends
                                  : 8u;

   VkResult result = VK_SUCCESS;
   uint8_t * dst = pData;
   for (uint32_t index = 0; index < queryCount; ++index) {
      uint32_t const query_index = firstQuery + index;
      uint8_t const * const slot = pool_mapping + (size_t)query_index * pool->slot_size;
      uint32_t const * const fence_ptr = (uint32_t const *)(slot + fence_offset);

      bool available = p_atomic_read(fence_ptr) == TERAKAN_QUERY_FENCE_READY;
      if (!available && wait) {
         while (p_atomic_read(fence_ptr) != TERAKAN_QUERY_FENCE_READY) {
            sched_yield();
         }
         available = true;
      }

      if (available || (partial && pool->type == VK_QUERY_TYPE_OCCLUSION)) {
         uint64_t value;
         if (pool->type == VK_QUERY_TYPE_OCCLUSION) {
            /* Sum `end[i] - begin[i]` across every enabled render backend. Even for a partial read
             * each pair's 64-bit halves are updated atomically by the HW, so the arithmetic stays
             * self-consistent even when the fence has not yet landed.
             */
            uint64_t sum = 0;
            for (uint32_t rb = 0; rb < pool->max_render_backends; ++rb) {
               uint64_t begin;
               uint64_t end;
               memcpy(&begin, slot + (size_t)rb * 16u, sizeof(uint64_t));
               memcpy(&end, slot + (size_t)rb * 16u + 8u, sizeof(uint64_t));
               sum += end - begin;
            }
            value = sum;
         } else {
            memcpy(&value, slot, sizeof(uint64_t));
         }
         terakan_query_write_result_element(dst, flags, value);
      } else {
         result = VK_NOT_READY;
      }

      if (!available) {
         result = VK_NOT_READY;
      }

      if (with_availability) {
         terakan_query_write_result_element(dst + element_size, flags, available ? 1u : 0u);
      }

      dst += stride;
   }

   (void)dataSize;
   return result;
}
