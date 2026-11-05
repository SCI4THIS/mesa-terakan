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

#ifndef TERAKAN_QUERY_H
#define TERAKAN_QUERY_H

#include "terakan_bo.h"

#include "vk_object.h"

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-slot layout in the query pool BO:
 *
 * - VK_QUERY_TYPE_OCCLUSION: per enabled DB render backend, one 64-bit ZPASS_DONE begin counter at
 *   offset `16 * i` and one 64-bit end counter at offset `16 * i + 8`. The HW scatters results
 *   across the `max_render_backends * 16`-byte range; the visible occlusion result is
 *   `sum_i (end[i] - begin[i])`. A 32-bit availability fence follows the per-RB counters at
 *   offset `16 * max_render_backends` (16-byte-padded to keep slot alignment and to match
 *   r600_query.c:607). Total: `16 * max_render_backends + 16` bytes per slot.
 * - VK_QUERY_TYPE_TIMESTAMP: one 64-bit BOTTOM_OF_PIPE_TS value at offset 0, followed by a 32-bit
 *   availability fence at offset 8 (the top-bit-set sentinel `TERAKAN_QUERY_FENCE_READY` is written
 *   by a second EVENT_WRITE_EOP after the timestamp lands, so a host-side poll can tell the two
 *   writes apart from the zero-fill left by vkCmdResetQueryPool). 16 bytes per slot (8 timestamp +
 *   4 fence + 4 pad for 8-byte EOP alignment).
 *
 * The CP's EVENT_WRITE_EOP packet writes a 64-bit value to a device address, which must be
 * 8-byte aligned. The BO is zero-initialized by vkCmdResetQueryPool via CP DMA fill.
 */
#define TERAKAN_QUERY_SLOT_SIZE_TIMESTAMP 16u

static inline uint32_t
terakan_query_pool_slot_size_occlusion(uint32_t const max_render_backends)
{
   return 16u * max_render_backends + 16u;
}

/* Written by a VALUE_32BIT EVENT_WRITE_EOP after the TIMESTAMP EOP; the top bit distinguishes a
 * completed write from the zero-fill left by vkCmdResetQueryPool and matches the fence sentinel
 * used by the gallium r600 driver (see r600_query.c:r600_query_hw_do_emit_start).
 */
#define TERAKAN_QUERY_FENCE_READY 0x80000000u

struct terakan_query_pool {
   struct vk_object_base base;

   VkQueryType type;
   uint32_t query_count;
   uint32_t slot_size;
   /* Only populated for OCCLUSION; mirrors the `max_render_backends` the pool was sized against
    * so GetQueryPoolResults / Cmd*Query do not need to re-read the physical device. 0 for other
    * query types.
    */
   uint32_t max_render_backends;

   struct terakan_bo * bo;
   uint64_t va;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_query_pool, base, VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL)

static inline uint32_t
terakan_query_pool_slot_size_for_type(VkQueryType const type, uint32_t const max_render_backends)
{
   switch (type) {
   case VK_QUERY_TYPE_OCCLUSION:
      return max_render_backends != 0
                ? terakan_query_pool_slot_size_occlusion(max_render_backends)
                : 0;
   case VK_QUERY_TYPE_TIMESTAMP:
      return TERAKAN_QUERY_SLOT_SIZE_TIMESTAMP;
   default:
      return 0;
   }
}

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_QUERY_H */
