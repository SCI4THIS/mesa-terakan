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

#include "terakan_shader.h"

#include "nir/terakan_nir.h"
#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_physical_device.h"

#include "compiler/glsl_types.h"
#include "gallium/drivers/r600/sfn/sfn_nir_lower_tex.h"
#include "spirv/nir_spirv.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "vk_nir.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* From gl_nir_linker.c. */
static void
terakan_nir_shared_type_info(struct glsl_type const * const type, unsigned * const size,
                             unsigned * const align)
{
   assert(glsl_type_is_vector_or_scalar(type));
   uint32_t const comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned const length = glsl_get_vector_elements(type);
   *size = comp_size * length;
   *align = comp_size * (length == 3 ? 4 : length);
}

nir_shader *
terakan_shader_spirv_to_nir(struct terakan_device * const device, size_t const spirv_size_bytes,
                            uint32_t const * const spirv, mesa_shader_stage const stage,
                            char const * const entrypoint,
                            VkSpecializationInfo const * const specialization_info)
{
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   static struct spirv_to_nir_options const spirv_options = {
      .environment = NIR_SPIRV_VULKAN,

      /* group_non_uniform_subgroup_size stays 0 for Vulkan 1.0: subgroup properties
       * (VkPhysicalDeviceSubgroupProperties.subgroupSize) are a 1.1 concept and this driver does
       * not expose them. SPIR-V code that uses the SubgroupSize built-in would require advertising
       * the Groups capability, which is also gated on 1.1 subgroup support. When 1.1 lands this
       * should become 64 (the Evergreen pixel wavefront size) and the physical device must start
       * reporting VkPhysicalDeviceSubgroupProperties.
       */
      .group_non_uniform_subgroup_size = 0,

      /* spirv_to_nir_options.capabilities is overwritten by vk_spirv_to_nir with the result of
       * vk_physical_device_get_spirv_capabilities(), which derives the set from the advertised
       * device features. Setting it here has no effect; new SPIR-V capabilities become usable by
       * advertising the matching Vulkan feature in terakan_physical_device.c (and, where
       * applicable, teaching SFN how to lower the corresponding NIR intrinsics).
       */

      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,

      .min_ubo_alignment = TERAKAN_KCACHE_HW_LINE_BYTES,
      .min_ssbo_alignment = sizeof(uint32_t),
   };

   nir_shader * nir =
      vk_spirv_to_nir(&device->vk, spirv, spirv_size_bytes, stage, entrypoint,
                      specialization_info, &spirv_options,
                      stage == MESA_SHADER_FRAGMENT ? &physical_device->nir_options_fs
                                                    : &physical_device->nir_options_non_fs,
                      false, NULL);

   /* SFN expects certain fragment shader system values to be accessed via load_input rather than
    * the system value load intrinsics, make sure that's the case before nir_lower_system_values is
    * done that would otherwise generate system value load intrinsics.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      struct nir_lower_sysvals_to_varyings_options const lower_sysvals_to_varyings_options = {
         .frag_coord = true,
         .front_face = true,
         .point_coord = true,
      };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &lower_sysvals_to_varyings_options);
   }

   /* Assign meanings and indices to variables in cases that don't depend on the actual executable
    * code once all variables are set up (including via nir_lower_sysvals_to_varyings).
    */

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      if (nir->info.stage == MESA_SHADER_VERTEX) {
         nir_foreach_shader_in_variable (var, nir) {
            assert(var->data.location >= VERT_ATTRIB_GENERIC0);
            var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
         }
      } else {
         nir_assign_io_var_locations(nir, nir_var_shader_in);
      }
      /* Fragment shader outputs are compacted in the end, not assigning locations here. */
      if (nir->info.stage != MESA_SHADER_FRAGMENT) {
         nir_assign_io_var_locations(nir, nir_var_shader_out);
      }
   }

   /* Make sure output writes are done only once, so they can be treated as exports, and also make
    * fragment inputs interpolated once.
    */

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      nir_variable_mode io_temp_modes = nir_var_shader_out;
      if (nir->info.stage == MESA_SHADER_FRAGMENT) {
         io_temp_modes |= nir_var_shader_in;
      }
      nir_lower_io_vars_to_temporaries(nir, nir_shader_get_entrypoint(nir), io_temp_modes);
   }

   /* Lower interface and binding derefs. */

   NIR_PASS(_, nir, nir_lower_system_values);

   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      /* NULL options: VK 1.0 does not expose `vkCmdDispatchBase` in core (1.1+ / device-group),
       * so `has_base_workgroup_id` stays false; `local_invocation_index` is cheaper to compute in
       * the shader from `local_invocation_id` than to pre-lower on Evergreen (SFN emits the muladd
       * sequence), so `lower_local_invocation_index` stays false too. Revisit if we ever advertise
       * 1.1 or `VK_KHR_device_group`.
       */
      NIR_PASS(_, nir, nir_lower_compute_system_values, NULL);
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
               terakan_nir_shared_type_info);
   }

   assert(spirv_options.ubo_addr_format == nir_address_format_32bit_index_offset);
   assert(spirv_options.ssbo_addr_format == nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   assert(spirv_options.push_const_addr_format == nir_address_format_32bit_offset);
   assert(spirv_options.shared_addr_format == nir_address_format_32bit_offset);
   NIR_PASS(
      _, nir, nir_lower_explicit_io,
      nir_var_mem_push_const | (nir->info.stage == MESA_SHADER_COMPUTE ? nir_var_mem_shared : 0),
      nir_address_format_32bit_offset);

   return nir;
}

static bool
terakan_nir_should_vectorize_load_store(unsigned const align_mul, unsigned const align_offset,
                                        unsigned const bit_size, unsigned const num_components,
                                        int64_t hole_size,
                                        nir_intrinsic_instr * const low,
                                        nir_intrinsic_instr * const high, UNUSED void * data)
{
   if (num_components > 4) {
      return false;
   }

   /* Alignment is intentionally not inspected: this callback is only wired to
    * `nir_var_mem_ubo | nir_var_mem_push_const` (see `load_store_vectorize_options.modes` below),
    * and the UBO/push-constant lowering in `nir/terakan_nir_lower_bindings.c` emits VTX_FETCH
    * resource loads whose address is already a runtime-computed dword offset — any natural
    * alignment the vectorizer produces for the fused load is representable without an alignment
    * repair. The per-call `align_mul`/`align_offset` arguments are the caller's promises about
    * the resulting load, not a constraint the backend needs to re-validate.
    */

   return true;
}

void
terakan_shader_lower_and_optimize_post_link(
   nir_shader * const nir, struct terakan_pipeline_layout const * const pipeline_layout,
   BITSET_WORD * const resources_needed, uint32_t * const samplers_needed,
   uint32_t * const driver_constants_needed,
   uint8_t * const fragment_data_uncompacted_locations_out)
{
   bool progress;

   /* Finally eliminate all dead code that may have effect on lowerings below and on analysis within
    * SFN, so that the demands of the shader can be estimated as accurately as possible.
    *
    * SFN also needs SSA, local variables need to be lowered to SSA, and the stores left after the
    * lowering need to be cleaned up, at some point.
    * Do that as part of the DCE loop, so that DCE works accurately through variable access, and can
    * provide feedback to dead variable removal.
    * Note that while the shader has functions inlined as part of SPIR-V to NIR conversion,
    * nir_var_shader_temp may be generated by passes like nir_lower_io_to_temporaries. They must be
    * lowered to nir_var_function_temp for this cleanup to work.
    */

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
   } while (progress);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* For fragment data location compaction. */
      NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in, NULL);
   }

   /* Compact fragment shader output locations.
    * See the description of terakan_nir_compact_fragment_data_locations.
    */

   uint8_t fragment_data_uncompacted_locations = 0b0;
   NIR_PASS(_, nir, terakan_nir_compact_fragment_data_locations,
            &fragment_data_uncompacted_locations);
   if (fragment_data_uncompacted_locations_out != NULL) {
      *fragment_data_uncompacted_locations_out = fragment_data_uncompacted_locations;
   }

   /* Assign gl_frag_result values to variables after the fragment data location compaction has
    * remapped the locations to the hardware values.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      nir_assign_io_var_locations(nir, nir_var_shader_out);
   }

   /* Lower texture operations. */

   NIR_PASS(_, nir, r600_nir_lower_cube_to_2darray);

   /* Vectorize loads that will be lowered to typed buffer load (vertex fetch) instructions. */

   nir_load_store_vectorize_options const load_store_vectorize_options = {
      .callback = terakan_nir_should_vectorize_load_store,
      .modes = nir_var_mem_ubo | nir_var_mem_push_const,
      /* Perf (non-v1.0): SSBO loads could also be folded into the vectorizer's modes, but not for
       * accesses that reach the UAV path; unvectorized SSBO loads remain correct today.
       */
      /* TODO(Triang3l): Robust access variable modes. */
   };
   NIR_PASS(_, nir, nir_opt_load_store_vectorize, &load_store_vectorize_options);

   /* Lower bindings according to the pipeline layout.
    * In fragment shaders, this is done after compacting the fragment data output locations as UAVs
    * must be placed above color attachments.
    */

   NIR_PASS(_, nir, terakan_nir_lower_bindings, pipeline_layout, resources_needed, samplers_needed);

   /* R8xx has no FORCE_UNNORMALIZED in the sampler constant (Cayman-only, evergreend.h:1628), so
    * the shader divides sample coordinates by textureSize() for operations that may observe a
    * sampler created with unnormalizedCoordinates. The per-stage mask lives in
    * terakan_push_constants_driver::sampler_unnormalized, populated by
    * terakan_CmdBindDescriptorSets. Must run after terakan_nir_lower_bindings because it emits
    * a push-constant load through the resource slot that pass assigns.
    */
   NIR_PASS(_, nir, terakan_nir_lower_unnormalized_coordinates, resources_needed,
            driver_constants_needed);

   /* Perform lowerings on the level of basic building blocks after the interface has been set up.
    */

   /* TODO(Triang3l): Invoke nir_lower_fragcoord_wtrans when r600_lower_and_optimize_nir is removed.
    */

   assert(nir->options->lower_to_scalar);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, nir->options->lower_to_scalar_filter, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, NULL, NULL);

   /* Everything lowered by nir_lower_alu is supported natively as of this writing. */

   NIR_PASS(_, nir, nir_lower_pack);

   nir_lower_idiv_options lower_idiv_options = {};
   NIR_PASS(_, nir, nir_lower_idiv, &lower_idiv_options);

   /* Includes both mandatory lowerings and optimizations. */
   NIR_PASS(_, nir, nir_opt_algebraic);

   if (!nir->info.flrp_lowered) {
      assert(nir->options->lower_flrp16 && nir->options->lower_flrp32 &&
             nir->options->lower_flrp64);
      bool lower_flrp_progress = false;
      NIR_PASS(lower_flrp_progress, nir, nir_lower_flrp, 16 | 32 | 64, false);
      if (lower_flrp_progress) {
         NIR_PASS(_, nir, nir_opt_constant_folding);
      }
      /* Nothing should rematerialize any flrps, so we only need to do this lowering once. */
      nir->info.flrp_lowered = true;
   }
}

void
terakan_shader_impl_finish(struct terakan_shader_impl * const shader,
                           VkAllocationCallbacks const * const allocator)
{
   if (shader->shader.arrays != NULL) {
      free(shader->shader.arrays);
   }

   terakan_bo_free(shader->static_state.program_bo, allocator);
}
