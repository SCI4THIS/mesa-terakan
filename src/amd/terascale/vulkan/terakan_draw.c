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

#include "terakan_draw.h"

#include "terakan_bo.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindIndexBuffer(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                           VkDeviceSize const offset, VkIndexType const indexType)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   uint32_t vgt_index_type;
   VkDeviceSize vgt_index_buffer_size = buffer->vk.size - offset;
   if (indexType == VK_INDEX_TYPE_UINT32) {
#if UTIL_ARCH_BIG_ENDIAN
      vgt_index_type = VGT_INDEX_32 | VGT_DMA_SWAP_32_BIT;
#else
      vgt_index_type = VGT_INDEX_32;
#endif
      vgt_index_buffer_size /= sizeof(uint32_t);
   } else {
      assert(indexType == VK_INDEX_TYPE_UINT16);
#if UTIL_ARCH_BIG_ENDIAN
      vgt_index_type = VGT_INDEX_16 | VGT_DMA_SWAP_16_BIT;
#else
      vgt_index_type = VGT_INDEX_16;
#endif
      vgt_index_buffer_size /= sizeof(uint16_t);
   }
   vgt_index_buffer_size = MIN2(vgt_index_buffer_size, UINT32_MAX);

   command_writer->state_draw.vgt_index_type = vgt_index_type;
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_TYPE);

   /* The index buffer is not needed by internal draws, modify hw_state_draw directly. */
   uint64_t const vgt_index_buffer_va = buffer->va + offset;
   bool const vgt_index_buffer_modified =
      command_writer->hw_state_draw.vgt_index_buffer.bo != buffer->bo ||
      command_writer->hw_state_draw.vgt_index_buffer.va != vgt_index_buffer_va ||
      command_writer->hw_state_draw.vgt_index_buffer.size != vgt_index_buffer_size;
   command_writer->hw_state_draw.vgt_index_buffer.bo = buffer->bo;
   command_writer->hw_state_draw.vgt_index_buffer.va = vgt_index_buffer_va;
   command_writer->hw_state_draw.vgt_index_buffer.size = vgt_index_buffer_size;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_BUFFER,
                                 vgt_index_buffer_modified);
}

static void
terakan_set_vertex_instance_offsets(struct terakan_gfx_command_writer * const command_writer,
                                    uint32_t const vertex_offset, uint32_t const instance_offset)
{
   command_writer->state_draw.vgt_index_offset = vertex_offset;
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_OFFSET);

   /* The instance offset is not needed by internal draws, modify hw_state_draw directly. */
   bool const sq_vtx_start_inst_loc_modified =
      command_writer->hw_state_draw.sq_vtx_start_inst_loc != instance_offset;
   command_writer->hw_state_draw.sq_vtx_start_inst_loc = instance_offset;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_SQ_VTX_START_INST_LOC,
                                 sq_vtx_start_inst_loc_modified);
}

void
terakan_before_hw_draw(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_state_draw_emit_modified(command_writer);

   /* Insert barriers after emitting the state changes, not before, so state changes are not blocked
    * by the barriers in the CP, and new work can begin as soon as possible.
    */
   terakan_barrier_emit_pending_actions(command_writer);
}

static void
terakan_before_draw(struct terakan_gfx_command_writer * const command_writer)
{
   /* Close the current IB and emit the leaving-compute cache flushes if the previous command was a
    * dispatch. Early-returns when `is_compute_active == false`, so zero overhead on pure-graphics
    * command buffers. Dormant today — the flag never flips because `vkCreateComputePipelines`
    * returns `VK_ERROR_FEATURE_NOT_PRESENT`. active-vulkan-1.0.md task #10 sub-step (a.3).
    */
   terakan_gfx_command_writer_split_for_draw_compute_switch(command_writer, false);

   terakan_state_draw_apply_pending(command_writer);

   terakan_push_constants_apply(command_writer, false);

   terakan_before_hw_draw(command_writer);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDraw(VkCommandBuffer const commandBuffer, uint32_t const vertexCount,
                uint32_t const instanceCount, uint32_t const firstVertex,
                uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* VGT_NUM_INSTANCES 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_set_vertex_instance_offsets(command_writer, firstVertex, firstInstance);

   terakan_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 3, false);
   if (unlikely(packet == NULL)) {
      return;
   }

   /* NUM_INSTANCES in the same indirect buffer as the draw. */
   *packet++ = PKT3(PKT3_NUM_INSTANCES, 0, 0);
   *packet++ = instanceCount;

   *packet++ = PKT3(PKT3_DRAW_INDEX_AUTO, 3 - 2, 0);
   *packet++ = vertexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndexed(VkCommandBuffer const commandBuffer, uint32_t const indexCount,
                       uint32_t const instanceCount, uint32_t const firstIndex,
                       int32_t const vertexOffset, uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* VGT_NUM_INSTANCES 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_set_vertex_instance_offsets(command_writer, (uint32_t)vertexOffset, firstInstance);

   terakan_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 4, false);
   if (unlikely(packet == NULL)) {
      return;
   }

   /* NUM_INSTANCES in the same indirect buffer as the draw. */
   *packet++ = PKT3(PKT3_NUM_INSTANCES, 0, 0);
   *packet++ = instanceCount;

   *packet++ = PKT3(EG_PKT3_DRAW_INDEX_OFFSET, 4 - 2, 0);
   *packet++ = firstIndex;
   *packet++ = indexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

/* Executed by HW on DRAW_INDIRECT / DRAW_INDEX_INDIRECT: HW reads vertexCount, instanceCount,
 * firstVertex, firstInstance from the indirect buffer and writes them into the hardware's
 * VGT_NUM_INSTANCES, VGT_INDX_OFFSET, and SQ_VTX_START_INST_LOC registers directly, bypassing the
 * CP's normal register-write path. The shadow state in terakan_hw_state_draw is therefore stale
 * after an indirect draw; invalidate it so the next non-indirect draw unconditionally re-emits.
 *
 * VGT_NUM_INSTANCES has no shadow — CmdDraw / CmdDrawIndexed emit it inline on each draw — so only
 * VGT_INDX_OFFSET and SQ_VTX_START_INST_LOC need invalidation.
 */
static void
terakan_invalidate_vertex_instance_offsets_after_indirect(
   struct terakan_gfx_command_writer * const command_writer)
{
   command_writer->hw_state_draw.vgt_index_offset = UINT32_MAX;
   command_writer->hw_state_draw.sq_vtx_start_inst_loc = UINT32_MAX;
}

/* Common tail for CmdDrawIndirect / CmdDrawIndexedIndirect: emits one SET_BASE for the indirect
 * buffer followed by drawCount DRAW_[INDEX_]INDIRECT packets at offset + i * stride. SET_BASE sets
 * the base VA and each draw packet supplies the offset from that base, so one SET_BASE serves the
 * entire batch.
 */
static void
terakan_emit_draw_indirect(struct terakan_gfx_command_writer * const command_writer,
                           struct terakan_buffer const * const buffer,
                           VkDeviceSize const offset, uint32_t const drawCount,
                           uint32_t const stride, uint32_t const indirect_opcode,
                           uint32_t const source_select)
{
   uint64_t const buffer_va = buffer->va;

   /* SET_BASE (4 dwords) + drawCount * DRAW_[INDEX_]INDIRECT (3 dwords). */
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, 4 + 3 * drawCount, false, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }

   /* SET_BASE: EG_DRAW_INDEX_INDIRECT_PATCH_TABLE_BASE, va_lo, va_hi.
    * HW adds the per-draw offset to this base when fetching VkDrawIndirectCommand.
    */
   *packet++ = PKT3(EG_PKT3_SET_BASE, 3 - 1, 0);
   *packet++ = EG_DRAW_INDEX_INDIRECT_PATCH_TABLE_BASE;
   uint32_t const * const packet_set_base_va = packet;
   *packet++ = (uint32_t)buffer_va;
   *packet++ = (buffer_va >> 32) & 0xFF;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_set_base_va, packet_set_base_va + 1,
      TERASCALE_WDDM_PATCH_IDS_SET_BASE_LO, TERASCALE_WDDM_PATCH_IDS_SET_BASE_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                buffer->bo, true, false,
                                                TERAKAN_BO_PRIORITY_DRAW_INDIRECT));

   for (uint32_t i = 0; i < drawCount; ++i) {
      *packet++ = PKT3(indirect_opcode, 2 - 1, 0);
      *packet++ = (uint32_t)offset + i * stride;
      *packet++ = S_0287F0_SOURCE_SELECT(source_select);
   }

   terakan_gfx_command_writer_emit_done(command_writer, packet);

   terakan_invalidate_vertex_instance_offsets_after_indirect(command_writer);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndirect(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                        VkDeviceSize const offset, uint32_t const drawCount,
                        uint32_t const stride)
{
   if (unlikely(drawCount == 0)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   terakan_before_draw(command_writer);

   terakan_emit_draw_indirect(command_writer, buffer, offset, drawCount, stride,
                              EG_PKT3_DRAW_INDIRECT, V_0287F0_DI_SRC_SEL_AUTO_INDEX);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndexedIndirect(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                               VkDeviceSize const offset, uint32_t const drawCount,
                               uint32_t const stride)
{
   if (unlikely(drawCount == 0)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   terakan_before_draw(command_writer);

   terakan_emit_draw_indirect(command_writer, buffer, offset, drawCount, stride,
                              EG_PKT3_DRAW_INDEX_INDIRECT, V_0287F0_DI_SRC_SEL_DMA);
}

/* Compute equivalent of PKT3's compute-mode routing bit (bit 1 of the PKT3 header). Evergreen's CP
 * splits graphics vs compute dispatch queues on this bit; DISPATCH_DIRECT must set it so the
 * packet is routed through the compute dispatcher. Mirrors `PKT3C(PKT3_DISPATCH_DIRECT, …)` in
 * `evergreen_compute.c:270,277` and `RADEON_CP_PACKET3_COMPUTE_MODE` in `r600_pipe.h:917`.
 */
#define TERAKAN_PKT3_COMPUTE_MODE 0x00000002

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatch(VkCommandBuffer const commandBuffer, uint32_t const groupCountX,
                    uint32_t const groupCountY, uint32_t const groupCountZ)
{
   if (unlikely(groupCountX == 0 || groupCountY == 0 || groupCountZ == 0)) {
      /* Vulkan spec: a dispatch with any zero grid dimension is a no-op. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_gfx_command_writer_before_app_dispatch(command_writer);

   /* Flushes the compute pipeline change if any (SQ_PGM_START_LS + SPI_COMPUTE_NUM_THREAD +
    * SQ_LDS_ALLOC + VGT_NUM_INDICES + VGT_COMPUTE_THREAD_GROUP_SIZE in `_emit_sq_pgm_cs`).
    */
   terakan_hw_state_draw_emit_modified(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 5, false);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | TERAKAN_PKT3_COMPUTE_MODE;
   *packet++ = groupCountX;
   *packet++ = groupCountY;
   *packet++ = groupCountZ;
   /* VGT_DISPATCH_INITIATOR = COMPUTE_SHADER_EN (bit 0). Matches `evergreen_compute.c:282`. */
   *packet++ = 1;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatchIndirect(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                            VkDeviceSize const offset)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   terakan_gfx_command_writer_before_app_dispatch(command_writer);

   terakan_hw_state_draw_emit_modified(command_writer);

   /* SET_BASE (4 dwords, index 1 shared with draw indirect — Evergreen only exposes one
    * indirect-buffer base slot and the compute-mode bit on the SET_BASE packet routes the write
    * to the CP's compute dispatcher) + DISPATCH_INDIRECT (3 dwords: offset + initiator).
    */
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(command_writer, 4 + 3, false, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }

   uint64_t const buffer_va = buffer->va;

   *packet++ = PKT3(EG_PKT3_SET_BASE, 3 - 1, 0) | TERAKAN_PKT3_COMPUTE_MODE;
   *packet++ = EG_DRAW_INDEX_INDIRECT_PATCH_TABLE_BASE;
   uint32_t const * const packet_set_base_va = packet;
   *packet++ = (uint32_t)buffer_va;
   *packet++ = (buffer_va >> 32) & 0xFF;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_set_base_va, packet_set_base_va + 1,
      TERASCALE_WDDM_PATCH_IDS_SET_BASE_LO, TERASCALE_WDDM_PATCH_IDS_SET_BASE_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                buffer->bo, true, false,
                                                TERAKAN_BO_PRIORITY_DRAW_INDIRECT));

   *packet++ = PKT3(PKT3_DISPATCH_INDIRECT, 2 - 1, 0) | TERAKAN_PKT3_COMPUTE_MODE;
   *packet++ = (uint32_t)offset;
   /* VGT_DISPATCH_INITIATOR = COMPUTE_SHADER_EN. */
   *packet++ = 1;

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}
