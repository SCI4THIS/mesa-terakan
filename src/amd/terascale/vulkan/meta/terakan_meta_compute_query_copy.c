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

#include "terakan_meta_compute_query_copy.h"

#include "terakan_barrier.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_hw_state.h"
#include "terakan_physical_device.h"
#include "terakan_push_constants.h"
#include "terakan_query.h"
#include "terakan_shader.h"

#include "compiler/glsl_types.h"
#include "compiler/nir/nir_builder.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_pipe.h"
#include "gallium/drivers/r600/r600_shader_common.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "util/ralloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Compute-mode routing bit on the PKT3 header — mirrors the private definition
 * in terakan_draw.c (`TERAKAN_PKT3_COMPUTE_MODE`). Duplicated here rather than
 * exported because it is a CP-packet-level concern, not a cross-module API.
 */
#define TERAKAN_META_QUERY_COPY_PKT3_COMPUTE_MODE 0x00000002u

/* Loads one scalar push-constant field as a deref load through the `push`
 * interface variable. The variable was created with a glsl_interface_type whose
 * field order matches struct terakan_meta_query_copy_push, so field indices
 * correspond directly.
 */
static nir_def *
load_push_u32(nir_builder * const b, nir_variable * const push, unsigned const field)
{
   nir_deref_instr * const deref =
      nir_build_deref_struct(b, nir_build_deref_var(b, push), field);
   return nir_load_deref(b, deref);
}

/* Reduce one query: load `max_rb` begin/end pairs from pool SSBO starting at
 * `slot_off` (byte offset within the pool buffer), sum `(end - begin)` into a
 * (sum_lo, sum_hi) vec2 (i64 in two u32 halves — shaderInt64 is unsupported on
 * R8xx), store result to dst SSBO at `dst_off`.
 */
static void
emit_per_query_reduction(nir_builder * const b, nir_variable * const push)
{
   nir_def * const tid = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);

   nir_def * const max_rb         = load_push_u32(b, push, 0);
   nir_def * const pool_slot_size = load_push_u32(b, push, 1);
   nir_def * const query_count    = load_push_u32(b, push, 3);
   nir_def * const flags          = load_push_u32(b, push, 4);
   nir_def * const elem_size      = load_push_u32(b, push, 5);
   nir_def * const pool_base      = load_push_u32(b, push, 6);
   nir_def * const dst_base       = load_push_u32(b, push, 7);
   nir_def * const dst_stride     = load_push_u32(b, push, 8);

   /* Early-out for the dispatch's dead tail. The grid is rounded up to the
    * workgroup size, so (query_count % workgroup_size) threads of the last
    * workgroup must bail. SFN rejects `return` mid-shader (sfn_shader.cpp:
    * "Jump instruction return not supported"), so wrap the whole body in an
    * `if (tid < query_count)` instead.
    */
   nir_push_if(b, nir_ult(b, tid, query_count));
   {

   /* slot_off = pool_base + tid * pool_slot_size */
   nir_def * const slot_off =
      nir_iadd(b, pool_base, nir_imul(b, tid, pool_slot_size));

   /* Fence word at slot_off + 16*max_rb; fence available when fence_word has the
    * READY sentinel in its low 32 bits (OCCLUSION writes a 32-bit sentinel, see
    * terakan_query.c EVENT_WRITE_EOP VALUE_32BIT).
    */
   nir_def * const fence_off = nir_iadd(b, slot_off, nir_imul_imm(b, max_rb, 16));
   nir_def * const fence_val =
      nir_load_ssbo(b, 1, 32, nir_imm_int(b, TERAKAN_META_QUERY_COPY_SSBO_BINDING_POOL),
                    fence_off);
   nir_def * const available =
      nir_ieq_imm(b, fence_val, (uint32_t)TERAKAN_QUERY_FENCE_READY);

   /* TODO: WAIT_BIT spin loop. For now, treat WAIT_BIT like PARTIAL_BIT —
    * always proceed, writing whatever the pool currently holds. This is wrong
    * for conformance but keeps the shader tractable for the initial landing;
    * the spin will be added once the loop + ssbo-load code emit is proven out
    * on the R8xx SFN path.
    */
   nir_def * const write_result =
      nir_ior(b,
              available,
              nir_ine_imm(b,
                          nir_iand_imm(b, flags,
                                       TERAKAN_META_QUERY_COPY_FLAG_WAIT_BIT |
                                          TERAKAN_META_QUERY_COPY_FLAG_PARTIAL),
                          0));

   /* Per-RB reduction: sum := sum_hi:sum_lo += end64[i] - begin64[i].
    * i64 subtract-with-borrow built on 32-bit primitives:
    *   diff_lo = end_lo - begin_lo
    *   borrow  = (end_lo < begin_lo) ? 1 : 0
    *   diff_hi = end_hi - begin_hi - borrow
    * Then 64-bit accumulate on (sum_lo, sum_hi).
    */
   nir_variable * const sum_lo_var =
      nir_local_variable_create(b->impl, glsl_uint_type(), "sum_lo");
   nir_variable * const sum_hi_var =
      nir_local_variable_create(b->impl, glsl_uint_type(), "sum_hi");
   nir_store_var(b, sum_lo_var, nir_imm_int(b, 0), 0x1);
   nir_store_var(b, sum_hi_var, nir_imm_int(b, 0), 0x1);

   nir_variable * const i_var =
      nir_local_variable_create(b->impl, glsl_uint_type(), "i");
   nir_store_var(b, i_var, nir_imm_int(b, 0), 0x1);

   nir_push_loop(b);
   {
      nir_def * const i = nir_load_var(b, i_var);
      nir_push_if(b, nir_uge(b, i, max_rb));
      {
         nir_jump(b, nir_jump_break);
      }
      nir_pop_if(b, NULL);

      nir_def * const rb_off = nir_iadd(b, slot_off, nir_imul_imm(b, i, 16));
      nir_def * const pool_idx =
         nir_imm_int(b, TERAKAN_META_QUERY_COPY_SSBO_BINDING_POOL);

      nir_def * const begin_lo =
         nir_load_ssbo(b, 1, 32, pool_idx, nir_iadd_imm(b, rb_off, 0));
      nir_def * const begin_hi =
         nir_load_ssbo(b, 1, 32, pool_idx, nir_iadd_imm(b, rb_off, 4));
      nir_def * const end_lo =
         nir_load_ssbo(b, 1, 32, pool_idx, nir_iadd_imm(b, rb_off, 8));
      nir_def * const end_hi =
         nir_load_ssbo(b, 1, 32, pool_idx, nir_iadd_imm(b, rb_off, 12));

      nir_def * const diff_lo = nir_isub(b, end_lo, begin_lo);
      nir_def * const borrow_sub =
         nir_b2i32(b, nir_ult(b, end_lo, begin_lo));
      nir_def * const diff_hi =
         nir_isub(b, nir_isub(b, end_hi, begin_hi), borrow_sub);

      nir_def * const sum_lo = nir_load_var(b, sum_lo_var);
      nir_def * const sum_hi = nir_load_var(b, sum_hi_var);

      nir_def * const new_sum_lo = nir_iadd(b, sum_lo, diff_lo);
      nir_def * const carry_add =
         nir_b2i32(b, nir_ult(b, new_sum_lo, sum_lo));
      nir_def * const new_sum_hi =
         nir_iadd(b, nir_iadd(b, sum_hi, diff_hi), carry_add);

      nir_store_var(b, sum_lo_var, new_sum_lo, 0x1);
      nir_store_var(b, sum_hi_var, new_sum_hi, 0x1);

      nir_store_var(b, i_var, nir_iadd_imm(b, i, 1), 0x1);
   }
   nir_pop_loop(b, NULL);

   /* Destination offset. */
   nir_def * const dst_off = nir_iadd(b, dst_base, nir_imul(b, tid, dst_stride));
   nir_def * const dst_idx =
      nir_imm_int(b, TERAKAN_META_QUERY_COPY_SSBO_BINDING_DST);

   nir_push_if(b, write_result);
   {
      nir_def * const sum_lo = nir_load_var(b, sum_lo_var);
      nir_def * const sum_hi = nir_load_var(b, sum_hi_var);

      nir_push_if(b,
                  nir_ine_imm(b,
                              nir_iand_imm(b, flags, TERAKAN_META_QUERY_COPY_FLAG_64_BIT),
                              0));
      {
         nir_store_ssbo(b, sum_lo, dst_idx, dst_off, .write_mask = 0x1,
                        .access = ACCESS_NON_READABLE, .align_mul = 4, .align_offset = 0);
         nir_store_ssbo(b, sum_hi, dst_idx, nir_iadd_imm(b, dst_off, 4),
                        .write_mask = 0x1, .access = ACCESS_NON_READABLE,
                        .align_mul = 4, .align_offset = 0);
      }
      nir_push_else(b, NULL);
      {
         nir_store_ssbo(b, sum_lo, dst_idx, dst_off, .write_mask = 0x1,
                        .access = ACCESS_NON_READABLE, .align_mul = 4, .align_offset = 0);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);

   /* Availability word. Written unconditionally when WITH_AVAILABILITY_BIT is
    * set — it must reflect the "was the query ready at the moment we checked"
    * state even if the result itself is stale (partial / not-yet-available).
    */
   nir_push_if(b,
               nir_ine_imm(b,
                           nir_iand_imm(b, flags,
                                        TERAKAN_META_QUERY_COPY_FLAG_WITH_AVAILABILITY),
                           0));
   {
      nir_def * const avail_word =
         nir_bcsel(b, available, nir_imm_int(b, 1), nir_imm_int(b, 0));
      nir_def * const avail_off = nir_iadd(b, dst_off, elem_size);
      nir_store_ssbo(b, avail_word, dst_idx, avail_off, .write_mask = 0x1,
                     .access = ACCESS_NON_READABLE, .align_mul = 4,
                     .align_offset = 0);
      /* For 64-bit availability, zero the high half. */
      nir_push_if(b,
                  nir_ine_imm(b,
                              nir_iand_imm(b, flags, TERAKAN_META_QUERY_COPY_FLAG_64_BIT),
                              0));
      {
         nir_store_ssbo(b, nir_imm_int(b, 0), dst_idx,
                        nir_iadd_imm(b, avail_off, 4), .write_mask = 0x1,
                        .access = ACCESS_NON_READABLE, .align_mul = 4,
                        .align_offset = 0);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);

   } /* if (tid < query_count) */
   nir_pop_if(b, NULL);
}

nir_shader *
terakan_meta_build_query_copy_nir(struct nir_shader_compiler_options const * const options)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                     "terakan-meta-query-copy");
   b.shader->info.workgroup_size[0] = TERAKAN_META_QUERY_COPY_WORKGROUP_SIZE;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   /* Bindings 0 (pool) and 1 (dst). SFN's ssbo_image_offset() is derived from
    * info.num_images (0 here), so SSBO 0/1 map to RAT 0/1 at emit time.
    */
   b.shader->info.num_ssbos = 2;

   /* Push-constants interface. Field order must match
    * struct terakan_meta_query_copy_push.
    */
   struct glsl_struct_field const push_fields[] = {
      { .type = glsl_uint_type(), .name = "max_rb",           .offset = 0  },
      { .type = glsl_uint_type(), .name = "pool_slot_size",   .offset = 4  },
      { .type = glsl_uint_type(), .name = "first_query",      .offset = 8  },
      { .type = glsl_uint_type(), .name = "query_count",      .offset = 12 },
      { .type = glsl_uint_type(), .name = "flags",            .offset = 16 },
      { .type = glsl_uint_type(), .name = "elem_size_bytes",  .offset = 20 },
      { .type = glsl_uint_type(), .name = "pool_byte_offset", .offset = 24 },
      { .type = glsl_uint_type(), .name = "dst_byte_offset",  .offset = 28 },
      { .type = glsl_uint_type(), .name = "dst_stride_bytes", .offset = 32 },
      { .type = glsl_uint_type(), .name = "_pad0",            .offset = 36 },
   };
   struct glsl_type const * const push_iface_type =
      glsl_interface_type(push_fields, ARRAY_SIZE(push_fields),
                          GLSL_INTERFACE_PACKING_STD140,
                          false /* row_major */, "push");

   nir_variable * const push =
      nir_variable_create(b.shader, nir_var_mem_push_const, push_iface_type, "push");

   emit_per_query_reduction(&b, push);

   return b.shader;
}

VkResult
terakan_device_query_copy_shader_init(struct terakan_device * const device)
{
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);
   nir_shader * const nir =
      terakan_meta_build_query_copy_nir(&physical_device->nir_options_non_fs);
   if (nir == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   /* Lowerings that `terakan_shader_spirv_to_nir` normally runs. Our hand-built
    * shader bypassed that path (there's no SPIR-V), so invoke them here before
    * `terakan_shader_lower_and_optimize_post_link`.
    *
    * Unlike the SPIR-V path which can pass NULL for `nir_lower_compute_system_values`
    * (its shaders typically avoid `gl_GlobalInvocationID`), this builder emits
    * `load_global_invocation_id` directly. With `nir_options.has_cs_global_id`
    * unset, NIR lowers that to a form that reads `load_base_workgroup_id`; a
    * NULL lowering-options arg then leaves that intrinsic unresolved and SFN
    * rejects it. Pass an explicit `has_base_workgroup_id = false` so the
    * residual `load_base_workgroup_id` folds to `imm_zero(3, 32)` — correct
    * for VK 1.0 without `VK_KHR_device_group` / `vkCmdDispatchBase`.
    */
   NIR_PASS(_, nir, nir_lower_system_values);
   struct nir_lower_compute_system_values_options const cs_sysval_opts = {
      .has_base_workgroup_id = false,
      .has_base_global_invocation_id = false,
      .has_global_size = false,
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_sysval_opts);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);

   struct terakan_shader_impl * const shader = &device->query_copy_shader;
   memset(shader, 0, sizeof(*shader));

   /* `terakan_nir_lower_bindings` reads app push-constant extents from the
    * pipeline layout; with layout = NULL (see rationale below) it leaves the
    * field alone. Set it to our 40-byte push block size so VTX_FETCH loads at
    * `TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES + offset` resolve within the
    * kcache window the command writer allocates.
    */
   shader->push_constants_usage.app_extent_bytes =
      sizeof(struct terakan_meta_query_copy_push);

   /* NULL pipeline_layout: this shader emits only `load_push_constant` and
    * `load_ssbo` / `store_ssbo` with bare-constant buffer indices.
    * `terakan_nir_lower_bindings_instr` only rewrites `tex`, `load_ubo`, and
    * `load_push_constant`; it never touches SSBO ops nor walks a pipeline
    * layout for them. See iter-66 code audit in active-vulkan-1.0.md.
    */
   terakan_shader_lower_and_optimize_post_link(
      nir, NULL, shader->resources_needed, &shader->samplers_needed,
      &shader->push_constants_usage.driver_constants, NULL);

   union r600_shader_key const shader_key = {0};
   VkResult const result =
      terakan_shader_impl_compile(shader, device, &shader_key, nir, NULL);
   ralloc_free(nir);
   if (result != VK_SUCCESS) {
      memset(shader, 0, sizeof(*shader));
      return result;
   }
   return VK_SUCCESS;
}

void
terakan_device_query_copy_shader_finish(struct terakan_device * const device)
{
   if (device->query_copy_shader.static_state.program_bo == NULL) {
      return;
   }
   terakan_shader_impl_finish(&device->query_copy_shader, NULL);
}

/* Translate VK_QUERY_RESULT_* flags to the shader's flag bits. The shader uses
 * its own flag layout (see TERAKAN_META_QUERY_COPY_FLAG_* in the header) so
 * that the push block does not mirror VkQueryResultFlagBits — which is a
 * platform-signed `int` typedef and would propagate signed-shift lint noise
 * into the shader's integer arithmetic.
 */
static uint32_t
terakan_meta_query_copy_translate_flags(VkQueryResultFlags const flags)
{
   uint32_t out = 0;
   if (flags & VK_QUERY_RESULT_64_BIT) {
      out |= TERAKAN_META_QUERY_COPY_FLAG_64_BIT;
   }
   if (flags & VK_QUERY_RESULT_WAIT_BIT) {
      out |= TERAKAN_META_QUERY_COPY_FLAG_WAIT_BIT;
   }
   if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
      out |= TERAKAN_META_QUERY_COPY_FLAG_WITH_AVAILABILITY;
   }
   if (flags & VK_QUERY_RESULT_PARTIAL_BIT) {
      out |= TERAKAN_META_QUERY_COPY_FLAG_PARTIAL;
   }
   return out;
}

void
terakan_meta_compute_query_copy_emit(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_query_pool const * const pool,
   struct terakan_buffer const * const dst_buffer, VkDeviceSize const dst_offset,
   VkDeviceSize const dst_stride, uint32_t const first_query,
   uint32_t const query_count, VkQueryResultFlags const flags)
{
   assert(pool != NULL && pool->bo != NULL);
   assert(pool->type == VK_QUERY_TYPE_OCCLUSION);
   assert(pool->max_render_backends > 0);
   assert(dst_buffer != NULL && dst_buffer->bo != NULL);
   assert(query_count > 0);

   struct terakan_command_buffer * const command_buffer = command_writer->base.command_buffer;
   struct terakan_device * const device =
      (struct terakan_device *)command_buffer->vk.base.device;
   struct terakan_hw_state_draw * const state = &command_writer->hw_state_draw;

   /* --- 1. Build and stage push-constant block. --------------------------- */

   bool const result_64 = (flags & VK_QUERY_RESULT_64_BIT) != 0;
   uint32_t const elem_size_bytes = result_64 ? 8u : 4u;

   struct terakan_meta_query_copy_push const push_struct = {
      .max_rb = pool->max_render_backends,
      .pool_slot_size = pool->slot_size,
      .first_query = first_query,
      .query_count = query_count,
      .flags = terakan_meta_query_copy_translate_flags(flags),
      .elem_size_bytes = elem_size_bytes,
      /* Shader's `pool_base` is a byte offset within the pool UAV (which starts
       * at pool->va). The shader never applies first_query itself (push field 2
       * is intentionally not loaded in emit_per_query_reduction), so bake the
       * starting slot in here.
       */
      .pool_byte_offset = first_query * pool->slot_size,
      .dst_byte_offset = (uint32_t)dst_offset,
      .dst_stride_bytes = (uint32_t)dst_stride,
      ._pad0 = 0,
   };

   /* Allocate a kcache-aligned push-constant staging buffer sized for
    * `driver_constants[0..APP_BASE_BYTES) || push_struct`. The shader
    * references constants at absolute offsets `APP_BASE_BYTES + field_offset`
    * (terakan_nir_lower_bindings `load_push_constant` case) so the driver
    * constants prefix must be present even though it is unused here — zero it.
    */
   uint32_t const push_bytes =
      TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES + (uint32_t)sizeof(push_struct);
   uint32_t const push_lines = DIV_ROUND_UP(push_bytes, TERAKAN_KCACHE_HW_LINE_BYTES);

   struct terakan_bo const * pc_bo;
   uint32_t pc_va_kcache_lines;
   void * const pc_mapping = terakan_push_buffer_allocate_kcache(
      command_buffer, TERAKAN_KCACHE_HW_LINE_BYTES * push_lines, &pc_bo,
      &pc_va_kcache_lines);
   if (unlikely(pc_mapping == NULL)) {
      return;
   }
   memset(pc_mapping, 0, TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES);
   memcpy((char *)pc_mapping + TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES, &push_struct,
          sizeof(push_struct));

   uint32_t pc_resource[8];
   uint64_t const pc_va = (uint64_t)pc_va_kcache_lines << TERAKAN_KCACHE_HW_LINE_BYTES_LOG2;
   if (unlikely(!terakan_descriptor_create_for_uniform_buffer(
          pc_bo, pc_va, TERAKAN_KCACHE_HW_LINE_BYTES * (VkDeviceSize)push_lines,
          pc_resource))) {
      return;
   }

   /* --- 2. Build pool read SSBO descriptor (VTX_FETCH). ------------------- */

   unsigned const tile_pipe_interleave_bytes_log2 =
      terakan_gfx_command_writer_physical_device(command_writer)
         ->tiling_info.pipe_interleave_bytes_log2;

   uint32_t pool_resource[8];
   struct terakan_color_descriptor pool_color_unused;
   VkDeviceSize const pool_range = (VkDeviceSize)pool->slot_size * pool->query_count;
   if (unlikely(!terakan_descriptor_create_for_storage_buffer(
          pool->bo, pool->va, pool_range, tile_pipe_interleave_bytes_log2, pool_resource,
          &pool_color_unused))) {
      return;
   }

   /* --- 3. Build dst write UAV color descriptor (RAT 1). ------------------ */

   uint32_t dst_resource_unused[8];
   struct terakan_color_descriptor dst_uav;
   if (unlikely(!terakan_descriptor_create_for_storage_buffer(
          dst_buffer->bo, dst_buffer->va, dst_buffer->vk.size,
          tile_pipe_interleave_bytes_log2, dst_resource_unused, &dst_uav))) {
      return;
   }

   /* --- 4. Enter compute-dispatch state. ---------------------------------- */

   terakan_gfx_command_writer_before_app_dispatch(command_writer);

   /* --- 5. Bind shader program. ------------------------------------------- */

   struct terakan_shader_static const * const cs_static =
      &device->query_copy_shader.static_state;
   if (state->sq_pgm_cs != cs_static) {
      state->sq_pgm_cs = cs_static;
      BITSET_SET(state->state_modified, TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_CS);
      BITSET_SET(state->state_ever_written, TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_CS);
   }

   /* --- 6. Bind push constants (kcache slot + VTX_FETCH resource slot). --- */

   terakan_hw_state_draw_set_sq_kcache_cs(state, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
                                          push_lines, pc_bo, pc_va_kcache_lines);
   terakan_hw_state_draw_set_sq_resource_cs(state, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS,
                                            pc_bo, pc_resource);

   /* --- 7. Bind pool SSBO (read). ----------------------------------------- *
    * SFN emits SSBO-load VTX_FETCH with
    *   res_id = R600_IMAGE_REAL_RESOURCE_OFFSET + ssbo_binding + shader.ssbo_image_offset()
    * (sfn_instr_mem.cpp emit_ssbo_load). This shader has num_images=0 so
    * ssbo_image_offset()=0; pool is SSBO binding 0 → slot 168.
    */
   terakan_hw_state_draw_set_sq_resource_cs(state, R600_IMAGE_REAL_RESOURCE_OFFSET + 0,
                                            pool->bo, pool_resource);

   /* --- 8. Bind dst as RAT 1 (CB_COLOR#1 in UAV mode). -------------------- *
    * SFN emits SSBO-store RAT STORE_TYPED with
    *   rat_id = ssbo_binding + shader.ssbo_image_offset()
    * (sfn_instr_mem.cpp emit_ssbo_store). dst is SSBO binding 1 → RAT 1 →
    * CB_COLOR#1.
    */
   terakan_hw_state_draw_set_cb_color(state, 1u, dst_buffer->bo, &dst_uav, NULL, true);

   /* --- 9. Set CB meta state: enable RGBA writes to slot 1 only. ---------- *
    * cb_target_mask has a 4-bit RGBA nibble per CB slot; shift by
    * 4 * color_index = 4 to light up slot 1. disable_blend bitmask has one bit
    * per slot (bit 1 for slot 1); blending is bypassed for UAVs regardless but
    * following the meta_begin_cb contract keeps state consistent.
    */
   terakan_meta_begin_cb(command_writer, V_028808_CB_NORMAL, (uint32_t)(0xFu << 4),
                         (uint8_t)(1u << 1));

   /* --- 10. Mark constants needed by CS. ---------------------------------- *
    * kcache bit for push_constants, resource bits for push_constants (slot 1)
    * and pool (slot 168). No samplers.
    */
   BITSET_DECLARE(resources_needed, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   BITSET_ZERO(resources_needed);
   BITSET_SET(resources_needed, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
   BITSET_SET(resources_needed, R600_IMAGE_REAL_RESOURCE_OFFSET + 0);
   terakan_hw_state_draw_set_sq_constants_needed_by_cs(
      state, (uint16_t)1u << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS, resources_needed, 0u);

   /* --- 11. Invalidate app push-constant binding tracking. ---------------- *
    * We just overwrote the CS kcache push-constants slot with our meta data.
    * The app's push constants in `state->app_constants` are unchanged, but the
    * HW binding no longer reflects them — clear the compute bit so the next
    * CmdDispatch's push_constants_apply re-binds state->allocation.bo.
    */
   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~(VkShaderStageFlags)VK_SHADER_STAGE_COMPUTE_BIT;

   /* --- 12. Flush pending state, emit dispatch. --------------------------- */

   terakan_hw_state_draw_emit_modified(command_writer);

   uint32_t const group_count_x =
      DIV_ROUND_UP(query_count, (uint32_t)TERAKAN_META_QUERY_COPY_WORKGROUP_SIZE);
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 5, false);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | TERAKAN_META_QUERY_COPY_PKT3_COMPUTE_MODE;
   *packet++ = group_count_x;
   *packet++ = 1u;
   *packet++ = 1u;
   *packet++ = 1u; /* VGT_DISPATCH_INITIATOR = COMPUTE_SHADER_EN */
   terakan_gfx_command_writer_emit_done(command_writer, packet);

   /* --- 13. Restore graphics CB state; mark post-dispatch barriers. ------- */

   terakan_hw_state_draw_restore_cb_state_after_compute(state);

   /* The dst UAV write must be flushed out of CB (RAT path) and the CS pipeline
    * drained before any subsequent reader can see the results. The caller
    * (vkCmdCopyQueryPoolResults) also ORs in SYNC_ME_TO_CP_DMA for TIMESTAMP's
    * CP-DMA path, but that is unrelated to this RAT write.
    */
   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS;
}
