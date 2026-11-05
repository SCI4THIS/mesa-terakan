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

#include "terakan_meta.h"

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_entrypoints.h"
#include "terakan_format.h"
#include "terakan_image.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
   /* In blocks, signed. */
   TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_X,
   TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y,

   TERAKAN_META_COPY_IMAGE_CONSTS_COUNT,
};

static uint32_t const terakan_meta_copy_image_ps_r8xx[] = {
   /* Control flow. */

   /* 0: Address offsetting. */

   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(4 - 3) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color and end the program. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* ALU clause. */

   /* Apply the address offset.
    *
    * 3:     R0.X = ADD_INT R0.X, CB[push].texture_minus_screen_position_x
    * 4: (v) R0.Y = ADD_INT R0.Y, CB[push].texture_minus_screen_position_y
    * Cycle 0: X = R0, Y = R0
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_X),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   /* 5 (alignment padding), 6, 7: Fetch from the source texture to R0. */

   0,
   0,

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

static uint32_t const terakan_meta_copy_image_ps_r9xx[] = {
   /* Control flow. */

   /* 0: Address offsetting. */

   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(5 - 4) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */

   0,
   S_SQ_CF_WORD1_BARRIER(1) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END,

   /* ALU clause. */

   /* Apply the address offset.
    *
    * 4: R0.X = ADD_INT R0.X, CB[push].texture_minus_screen_position_x
    * 5: R0.Y = ADD_INT R0.Y, CB[push].texture_minus_screen_position_y
    * Cycle 0: X = R0, Y = R0
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_X),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   /* 6, 7: Fetch from the source texture to R0. */

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

struct terakan_meta_shader const terakan_meta_copy_image_ps =
   {
      .r8xx =
         {
            .program = terakan_meta_copy_image_ps_r8xx,
            .program_size_bytes = sizeof(terakan_meta_copy_image_ps_r8xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .r9xx =
         {
            .program = terakan_meta_copy_image_ps_r9xx,
            .program_size_bytes = sizeof(terakan_meta_copy_image_ps_r9xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .kcache_needed = (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      .resources_needed =
         {
            [BITSET_BITWORD(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META)] =
               BITSET_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
         },
      .stage =
         {
            .ps =
               {
                  .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_DEFAULT,
               },
         },
};

/* COPY_IMAGE_MSAA_PS: like COPY_IMAGE_PS, but targets MSAA source images. Reads a single
 * sample from the MSAA source per invocation; the host loops over sample indices and uses
 * PA_SC_AA_MASK to constrain which dst sample gets written on each draw.
 *
 * Per the r600 ISA (sfn_instr_tex.cpp `lower_txf_ms_direct`), MSAA LD takes the sample index
 * in the W component of the source GPR. This shader:
 *   - ALU group: ADD_INT R0.x/y with push-constant texture offsets (same as COPY_IMAGE_PS),
 *     plus MOV R0.w = CB[push].sample_index (3 ops in one group across slots X/Y/W).
 *   - TEX: LD R0 → R0 with SRC_SEL_W = SWIZZLE_W so the sample index flows into the fetch.
 */
enum {
   /* In blocks, signed; same semantics as the non-MSAA path. */
   TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_X,
   TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y,
   TERAKAN_META_COPY_IMAGE_MSAA_CONST_SAMPLE_INDEX,

   TERAKAN_META_COPY_IMAGE_MSAA_CONSTS_COUNT,
};

static uint32_t const terakan_meta_copy_image_msaa_ps_r8xx[] = {
   /* Control flow. */

   /* 0: ALU clause. */

   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(3 - 1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch. */

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export and end. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* ALU clause (one group of three slots). Starts at dword 6.
    *
    * 3: X = ADD_INT R0.x, CB.offset_x    -> R0.x
    * 4: Y = ADD_INT R0.y, CB.offset_y    -> R0.y
    * 5: W = MOV CB.sample_index          -> R0.w   (LAST=1)
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_X),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_LAST(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC0(TERAKAN_META_COPY_IMAGE_MSAA_CONST_SAMPLE_INDEX) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(3) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   /* TEX clause — starts at dword 12 (ALU ended at dword 11, already 4-dword aligned). */

   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_LD) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_W),
   0,
};

static uint32_t const terakan_meta_copy_image_msaa_ps_r9xx[] = {
   /* Control flow. */

   /* 0: ALU clause. */

   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(3 - 1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch. */

   S_SQ_CF_WORD0_ADDR(8),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End. */

   0,
   S_SQ_CF_WORD1_BARRIER(1) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END,

   /* ALU clause — same 3 instructions as r8xx; starts at dword 8. */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_X),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(
         TERAKAN_META_COPY_IMAGE_MSAA_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_LAST(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC0(TERAKAN_META_COPY_IMAGE_MSAA_CONST_SAMPLE_INDEX) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(3) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   /* TEX clause — starts at dword 16 (ALU ended at dword 13; pad to 128-bit boundary). */

   0,
   0,

   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_LD) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_W),
   0,
};

struct terakan_meta_shader const terakan_meta_copy_image_msaa_ps =
   {
      .r8xx =
         {
            .program = terakan_meta_copy_image_msaa_ps_r8xx,
            .program_size_bytes = sizeof(terakan_meta_copy_image_msaa_ps_r8xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .r9xx =
         {
            .program = terakan_meta_copy_image_msaa_ps_r9xx,
            .program_size_bytes = sizeof(terakan_meta_copy_image_msaa_ps_r9xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .kcache_needed = (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      .resources_needed =
         {
            [BITSET_BITWORD(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META)] =
               BITSET_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
         },
      .stage =
         {
            .ps =
               {
                  .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_DEFAULT,
               },
         },
};

/* BLIT_IMAGE_NEAREST_PS: like COPY_IMAGE_PS, but the source coordinate is computed from the
 * fragment's fixed-point screen position via a float-space (scale, offset) transform. The host sets
 * scale = src_extent / dst_extent and offset = srcOffset - dstOffset * scale + 0.5 * scale, so the
 * shader computes:
 *
 *   src_coord_f = screen_pos_i * scale + offset
 *   src_coord_i = floor(src_coord_f)
 *
 * which is the VK_FILTER_NEAREST sampling formula for the center-of-dst-pixel in src space.
 * The 0.5 shift is folded into offset by the host. FLT_TO_INT_FLOOR gives true floor semantics,
 * including for negative offsets introduced by mirrored blits.
 */
enum {
   TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X,   /* f32 */
   TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_Y,   /* f32 */
   TERAKAN_META_BLIT_IMAGE_CONST_SCALE_X,    /* f32 */
   TERAKAN_META_BLIT_IMAGE_CONST_SCALE_Y,    /* f32 */

   TERAKAN_META_BLIT_IMAGE_CONSTS_COUNT,
};

static uint32_t const terakan_meta_blit_image_nearest_ps_r8xx[] = {
   /* Control flow. */

   /* 0: ALU clause — compute the source integer coord from the screen position. */

   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(8 - 1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */

   S_SQ_CF_WORD0_ADDR(12),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color and end the program. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* ALU clause.
    *
    *  3: (T) R1.X = INT_TO_FLT R0.X                      ; screen pos X -> float
    *  4: (T) R1.Y = INT_TO_FLT R0.Y                      ; screen pos Y -> float
    *  5:     R1.X = MUL R1.X, CB[push].scale_x
    *  6: (V) R1.Y = MUL R1.Y, CB[push].scale_y
    *  7:     R1.X = ADD R1.X, CB[push].offset_x
    *  8: (V) R1.Y = ADD R1.Y, CB[push].offset_y
    *  9: (T) R0.X = FLT_TO_INT_FLOOR R1.X                ; floor to int
    * 10: (T) R0.Y = FLT_TO_INT_FLOOR R1.Y
    */

   /* 3: INT_TO_FLT R0.x -> R1.x */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   /* 4: INT_TO_FLT R0.y -> R1.y */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   /* 5: MUL R1.x, kc.scale_x -> R1.x (X slot) */
   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   /* 6: MUL R1.y, kc.scale_y -> R1.y (Y slot, LAST=1 ends group) */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   /* 7: ADD R1.x, kc.offset_x -> R1.x (X slot) */
   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   /* 8: ADD R1.y, kc.offset_y -> R1.y (Y slot, LAST=1 ends group) */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   /* 9: FLT_TO_INT_FLOOR R1.x -> R0.x */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLT_TO_INT_FLOOR),

   /* 10: FLT_TO_INT_FLOOR R1.y -> R0.y */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLT_TO_INT_FLOOR),

   /* Alignment padding to 128-bit TEX boundary. ALU clause ended at dword 21 (= 6 + 8*2 - 1);
    * next 4-dword-aligned offset is dword 24. */
   0,
   0,

   /* TEX clause — LD at R0.xy to R0.rgba. */

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

static uint32_t const terakan_meta_blit_image_nearest_ps_r9xx[] = {
   /* Control flow. */

   /* 0: ALU clause. */

   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(8 - 1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */

   S_SQ_CF_WORD0_ADDR(12),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */

   0,
   S_SQ_CF_WORD1_BARRIER(1) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END,

   /* ALU clause — same 8 instructions as r8xx; starts at dword 8. */

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLT_TO_INT_FLOOR),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLT_TO_INT_FLOOR),

   /* ALU ends at dword 23; next 4-dword boundary is dword 24, so no padding. */

   /* TEX clause. */

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

struct terakan_meta_shader const terakan_meta_blit_image_nearest_ps =
   {
      .r8xx =
         {
            .program = terakan_meta_blit_image_nearest_ps_r8xx,
            .program_size_bytes = sizeof(terakan_meta_blit_image_nearest_ps_r8xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .r9xx =
         {
            .program = terakan_meta_blit_image_nearest_ps_r9xx,
            .program_size_bytes = sizeof(terakan_meta_blit_image_nearest_ps_r9xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .kcache_needed = (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      .resources_needed =
         {
            [BITSET_BITWORD(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META)] =
               BITSET_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
         },
      .stage =
         {
            .ps =
               {
                  .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_DEFAULT,
               },
         },
};

/* BLIT_IMAGE_LINEAR_PS: same (scale, offset) coord transform as BLIT_IMAGE_NEAREST_PS, but
 * leaves the result in R1.xy as float and fetches via SAMPLE (opcode 0x10) with
 * COORD_TYPE_X/Y = unnormalized. The sampler at index 0 is a static linear-filter sampler
 * with CLAMP_TO_LAST_TEXEL — see TERAKAN_META_BLIT_IMAGE_LINEAR_SAMPLER below. The host
 * binds that sampler before each draw. Reuses the same push-constant layout as the NEAREST
 * variant, so CmdBlitImage2 only has to swap the PS index.
 */
static uint32_t const terakan_meta_blit_image_linear_ps_r8xx[] = {
   /* 0: ALU clause. */
   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(6 - 1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: TEX clause (SAMPLE at R1.xy). */
   S_SQ_CF_WORD0_ADDR(10),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export color and end. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* ALU clause — 6 instructions, result in R1.xy. */

   /* 3: INT_TO_FLT R0.x -> R1.x */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   /* 4: INT_TO_FLT R0.y -> R1.y */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   /* 5: MUL R1.x, kc.scale_x -> R1.x (X slot) */
   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   /* 6: MUL R1.y, kc.scale_y -> R1.y (Y slot, LAST) */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   /* 7: ADD R1.x, kc.offset_x -> R1.x (X slot) */
   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   /* 8: ADD R1.y, kc.offset_y -> R1.y (Y slot, LAST) */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   /* Pad to 128-bit TEX boundary — ALU clause ends at dword 17, TEX starts at dword 20. */
   0,
   0,

   /* TEX clause — SAMPLE at R1.xy with unnormalized coords, sampler 0. */
   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_SAMPLE) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(1),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_TEX_WORD1_COORD_TYPE_X(1) | S_SQ_TEX_WORD1_COORD_TYPE_Y(1) |
      S_SQ_TEX_WORD1_COORD_TYPE_Z(1) | S_SQ_TEX_WORD1_COORD_TYPE_W(1),
   S_SQ_TEX_WORD2_SAMPLER_ID(0) |
      S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

static uint32_t const terakan_meta_blit_image_linear_ps_r9xx[] = {
   /* 0: ALU clause. */
   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(6 - 1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: TEX clause. */
   S_SQ_CF_WORD0_ADDR(10),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export color. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */
   0,
   S_SQ_CF_WORD1_BARRIER(1) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END,

   /* ALU clause — 6 instructions. */
   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_SCALE_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL),

   S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(1) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_DWORD_WORD0_SRC1(TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_Y),
   S_SQ_ALU_WORD1_DST_GPR(1) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD),

   /* ALU ends at dword 19; TEX starts at dword 20, aligned. */

   /* TEX clause. */
   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_SAMPLE) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(1),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_TEX_WORD1_COORD_TYPE_X(1) | S_SQ_TEX_WORD1_COORD_TYPE_Y(1) |
      S_SQ_TEX_WORD1_COORD_TYPE_Z(1) | S_SQ_TEX_WORD1_COORD_TYPE_W(1),
   S_SQ_TEX_WORD2_SAMPLER_ID(0) |
      S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

struct terakan_meta_shader const terakan_meta_blit_image_linear_ps =
   {
      .r8xx =
         {
            .program = terakan_meta_blit_image_linear_ps_r8xx,
            .program_size_bytes = sizeof(terakan_meta_blit_image_linear_ps_r8xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .r9xx =
         {
            .program = terakan_meta_blit_image_linear_ps_r9xx,
            .program_size_bytes = sizeof(terakan_meta_blit_image_linear_ps_r9xx),
            .static_registers =
               {
                  .sq_pgm_resources =
                     {
                        S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                     },
                  .stage =
                     {
                        .ps =
                           {
                              .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                              .spi_ps_in_control =
                                 {
                                    S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                    S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                       S_0286D0_FIXED_PT_POSITION_ADDR(0),
                                 },
                              .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                              .cb_shader_mask = 0xF,
                           },
                     },
               },
         },
      .kcache_needed = (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      .resources_needed =
         {
            [BITSET_BITWORD(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META)] =
               BITSET_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
         },
      .samplers_needed = (uint32_t)1 << 0,
      .stage =
         {
            .ps =
               {
                  .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_DEFAULT,
               },
         },
};

/* Static sampler bound to FS slot 0 for BLIT_IMAGE_LINEAR_PS. Linear min/mag, clamp-to-edge
 * on every axis, no mipmap LOD, Z filter NONE. Coord unnormalized-ness comes from the TEX
 * instruction's COORD_TYPE_X/Y/Z/W bits in the shader itself (R8xx has no FORCE_UNNORMALIZED
 * in the sampler — only R9xx). */
static uint32_t const terakan_meta_blit_image_linear_sampler[3] = {
   S_03C000_CLAMP_X(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
      S_03C000_CLAMP_Y(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
      S_03C000_CLAMP_Z(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
      S_03C000_XY_MAG_FILTER(V_03C000_SQ_TEX_XY_FILTER_BILINEAR) |
      S_03C000_XY_MIN_FILTER(V_03C000_SQ_TEX_XY_FILTER_BILINEAR) |
      S_03C000_Z_FILTER(V_03C000_SQ_TEX_Z_FILTER_NONE) |
      S_03C000_MIP_FILTER(V_03C000_SQ_TEX_Z_FILTER_NONE),
   0,
   S_03C008_TYPE(1),
};

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyImage2(VkCommandBuffer const commandBuffer,
                      VkCopyImageInfo2 const * const pCopyImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const src_image =
      terakan_image_from_handle(pCopyImageInfo->srcImage);

   if (terakan_format_is_expand_3x(src_image->surface.aspects[0].bytes_per_block)) {
      terakan_meta_copy_expand_3x_image(command_writer, pCopyImageInfo);
      return;
   }

   struct terakan_image const * const dst_image =
      terakan_image_from_handle(pCopyImageInfo->dstImage);

   /* MSAA: Vulkan requires equal sample counts on src and dst for image→image copy.
    * For samples>1 the per-sample path (COPY_IMAGE_MSAA_PS) copies one src sample per draw,
    * constrained to the matching dst sample via PA_SC_AA_MASK. FMask (fast-clear metadata)
    * isn't supported by this HW config, so no FMask-copy path. Depth/stencil MSAA copy is
    * deferred — dropped silently here.
    */
   if (src_image->vk.samples != dst_image->vk.samples) {
      return;
   }
   uint32_t const num_samples = (uint32_t)src_image->vk.samples;
   uint32_t const num_samples_log2 = util_logbase2(num_samples);
   bool const is_msaa = num_samples > 1;
   enum terakan_meta_shader_index const ps_shader =
      is_msaa ? TERAKAN_META_SHADER_COPY_IMAGE_MSAA_PS : TERAKAN_META_SHADER_COPY_IMAGE_PS;

   struct terakan_image_descriptor_create_info src_descriptor_create_info = {
      .image = src_image,
      .view_type = terakan_meta_transfer_image_view_type(src_image->vk.image_type),
      .force_little_endian = true,
      .level_count = 1,
   };
   struct terakan_image_descriptor_create_info dst_descriptor_create_info = {
      .image = dst_image,
      .view_type = terakan_meta_transfer_image_view_type(dst_image->vk.image_type),
      .force_little_endian = true,
      .level_count = 1,
   };

   unsigned const src_block_width = vk_format_get_blockwidth(src_image->vk.format);
   unsigned const src_block_height = vk_format_get_blockheight(src_image->vk.format);
   unsigned const dst_block_width = vk_format_get_blockwidth(dst_image->vk.format);
   unsigned const dst_block_height = vk_format_get_blockheight(dst_image->vk.format);

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true,
                                         num_samples_log2);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, ps_shader, false);

   terakan_meta_begin_cb(command_writer, V_028808_CB_NORMAL, 0xF, 0b1);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   /* Sized to the larger (MSAA = 3 dwords) layout so a single buffer serves both paths. For
    * the non-MSAA path the trailing dword is unused and ignored by the shader.
    */
   uint32_t constants[TERAKAN_META_COPY_IMAGE_MSAA_CONSTS_COUNT] = {};
   size_t const constants_size = is_msaa
                                    ? sizeof(uint32_t) * TERAKAN_META_COPY_IMAGE_MSAA_CONSTS_COUNT
                                    : sizeof(uint32_t) * TERAKAN_META_COPY_IMAGE_CONSTS_COUNT;
   struct terakan_bo const * constants_bo = NULL;
   uint32_t constants_va_lines;

   for (uint32_t region_index = 0; region_index < pCopyImageInfo->regionCount; ++region_index) {
      VkImageCopy2 const * const region = &pCopyImageInfo->pRegions[region_index];

      /* Section 49.1.7. "Format Compatibility Classes" of the Vulkan 1.3.283 specification says:
       *
       *     "Copy operations are able to copy between size-compatible formats in different
       *     resources to enable manipulation of data in different formats. The extent used in these
       *     copy operations always matches the source image, and is resized to the expectations of
       *     the block extents noted above for the destination image."
       *
       * The offset must be block-aligned, but offset + extent is limited to the extent of the
       * subresource, which is not block-aligned.
       */
      VkRect2D const rect = {
         .offset =
            {
               .x = region->dstOffset.x / dst_block_width,
               .y = region->dstOffset.y / dst_block_height,
            },
         .extent =
            {
               .width = DIV_ROUND_UP(region->extent.width, src_block_width),
               .height = DIV_ROUND_UP(region->extent.height, src_block_height),
            },
      };
      int32_t const texture_minus_screen_position_x =
         region->srcOffset.x / src_block_width - rect.offset.x;
      int32_t const texture_minus_screen_position_y =
         region->srcOffset.y / src_block_height - rect.offset.y;
      if (constants[TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_X] !=
             (uint32_t)texture_minus_screen_position_x ||
          constants[TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y] !=
             (uint32_t)texture_minus_screen_position_y) {
         constants_bo = NULL;
         constants[TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_X] =
            (uint32_t)texture_minus_screen_position_x;
         constants[TERAKAN_META_COPY_IMAGE_CONST_TEXTURE_MINUS_SCREEN_POSITION_Y] =
            (uint32_t)texture_minus_screen_position_y;
      }

      /* Non-MSAA: push kcache once per region (if constants changed). MSAA pushes
       * per-sample inside the inner loop below, because sample_index varies.
       */
      if (!is_msaa && constants_bo == NULL) {
         void * const constants_mapping = terakan_push_buffer_allocate_kcache(
            command_writer->base.command_buffer, constants_size, &constants_bo,
            &constants_va_lines);
         if (unlikely(constants_mapping == NULL)) {
            return;
         }
         memcpy(constants_mapping, constants, constants_size);
         terakan_hw_state_draw_set_sq_kcache_fs(
            &command_writer->hw_state_draw, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            DIV_ROUND_UP(constants_size, TERAKAN_KCACHE_HW_LINE_BYTES), constants_bo,
            constants_va_lines);
      }

      src_descriptor_create_info.base_mip_level = region->srcSubresource.mipLevel;
      if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         src_descriptor_create_info.base_array_layer = (uint32_t)region->srcOffset.z;
         src_descriptor_create_info.layer_count = region->extent.depth;
      } else {
         src_descriptor_create_info.base_array_layer = region->srcSubresource.baseArrayLayer;
         src_descriptor_create_info.layer_count =
            vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource);
      }

      dst_descriptor_create_info.base_mip_level = region->dstSubresource.mipLevel;
      uint32_t const dst_base_layer = dst_image->vk.image_type == VK_IMAGE_TYPE_3D
                                         ? (uint32_t)region->dstOffset.z
                                         : region->dstSubresource.baseArrayLayer;

      unsigned dst_aspect_mask_remaining = (unsigned)region->dstSubresource.aspectMask;
      u_foreach_bit (src_aspect_index, region->srcSubresource.aspectMask) {
         src_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
            src_image->format_info.aspect_map, (VkImageAspectFlags)1 << src_aspect_index, 0);
         dst_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
            dst_image->format_info.aspect_map,
            (VkImageAspectFlags)1 << u_bit_scan(&dst_aspect_mask_remaining), 0);

         src_descriptor_create_info.view_format = terakan_meta_transfer_image_block_format_info(
            src_image->surface.aspects[src_descriptor_create_info.image_aspect_index]
               .bytes_per_block);
         dst_descriptor_create_info.view_format = terakan_meta_transfer_image_block_format_info(
            dst_image->surface.aspects[dst_descriptor_create_info.image_aspect_index]
               .bytes_per_block);

         dst_descriptor_create_info.base_array_layer = dst_base_layer;
         dst_descriptor_create_info.layer_count = src_descriptor_create_info.layer_count;

         uint32_t src_resource[8];
         VkComponentMapping const identity_component_mapping = {};
         if (unlikely(!terakan_image_create_resource_descriptor(
                &src_descriptor_create_info, &identity_component_mapping, src_resource))) {
            assert(!"Invalid source image view create info");
            return;
         }

         while (dst_descriptor_create_info.layer_count > 0) {
            terakan_hw_state_draw_set_sq_resource_fs(
               &command_writer->hw_state_draw,
               TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META, src_image->bo, src_resource);

            struct terakan_color_descriptor dst_color;
            struct terakan_color_meta_descriptor dst_color_meta;
            uint32_t const dst_color_layer_count = terakan_image_create_color_descriptor(
               &dst_descriptor_create_info, &dst_color, &dst_color_meta);
            terakan_color_descriptor_image_view_to_color_attachment(&dst_color);
            terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_image->bo,
                                               &dst_color, &dst_color_meta, false);
            terakan_meta_set_db_shader_control_with_rtv(
               command_writer, terakan_meta_copy_image_ps.stage.ps.db_shader_control,
               dst_color.info);

            /* One draw per sample for MSAA; a single pass otherwise. Each MSAA pass copies
             * only sample s of the src to sample s of the dst, gated by PA_SC_AA_MASK.
             */
            for (uint32_t sample_index = 0; sample_index < MAX2(num_samples, 1u);
                 ++sample_index) {
               if (is_msaa) {
                  /* Push a fresh kcache line with the sample index written in. */
                  constants[TERAKAN_META_COPY_IMAGE_MSAA_CONST_SAMPLE_INDEX] = sample_index;
                  void * const constants_mapping = terakan_push_buffer_allocate_kcache(
                     command_writer->base.command_buffer, constants_size, &constants_bo,
                     &constants_va_lines);
                  if (unlikely(constants_mapping == NULL)) {
                     return;
                  }
                  memcpy(constants_mapping, constants, constants_size);
                  terakan_hw_state_draw_set_sq_kcache_fs(
                     &command_writer->hw_state_draw, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
                     DIV_ROUND_UP(constants_size, TERAKAN_KCACHE_HW_LINE_BYTES), constants_bo,
                     constants_va_lines);

                  uint16_t const sample_mask = (uint16_t)1 << sample_index;
                  bool const aa_mask_modified =
                     command_writer->hw_state_draw.pa_sc_aa_mask != sample_mask;
                  command_writer->hw_state_draw.pa_sc_aa_mask = sample_mask;
                  terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                                TERAKAN_HW_STATE_DRAW_INDEX_PA_SC_AA_MASK,
                                                aa_mask_modified);
               }

               terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect,
                                                      dst_color_layer_count);
            }

            src_resource[5] =
               (src_resource[5] & C_030014_BASE_ARRAY) |
               S_030014_BASE_ARRAY(G_030014_BASE_ARRAY(src_resource[5]) + dst_color_layer_count);
            dst_descriptor_create_info.base_array_layer += dst_color_layer_count;
            dst_descriptor_create_info.layer_count -= dst_color_layer_count;
         }
      }
   }
}

/* Image blit via BLIT_IMAGE_{NEAREST,LINEAR}_PS. Scope: 2D/1D images only, no MSAA, source
 * and destination format identical. Mirror blits are supported (dst rect normalized to
 * positive extent; signed scale drives the sampled src coord). Regions outside the supported
 * scope are silently skipped — a follow-up iter will add 3D and MSAA-via-resolve fallbacks.
 */
VKAPI_ATTR void VKAPI_CALL
terakan_CmdBlitImage2(VkCommandBuffer const commandBuffer,
                      VkBlitImageInfo2 const * const pBlitImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const src_image =
      terakan_image_from_handle(pBlitImageInfo->srcImage);
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(pBlitImageInfo->dstImage);

   bool const is_linear = pBlitImageInfo->filter == VK_FILTER_LINEAR;
   enum terakan_meta_shader_index const ps_shader = is_linear
      ? TERAKAN_META_SHADER_BLIT_IMAGE_LINEAR_PS
      : TERAKAN_META_SHADER_BLIT_IMAGE_NEAREST_PS;

   if (src_image->vk.image_type == VK_IMAGE_TYPE_3D ||
       dst_image->vk.image_type == VK_IMAGE_TYPE_3D) {
      return;
   }
   if (src_image->vk.samples != 1 || dst_image->vk.samples != 1) {
      return;
   }
   if (src_image->vk.format != dst_image->vk.format) {
      return;
   }
   if (terakan_format_is_expand_3x(src_image->surface.aspects[0].bytes_per_block)) {
      return;
   }

   struct terakan_image_descriptor_create_info src_descriptor_create_info = {
      .image = src_image,
      .view_type = terakan_meta_transfer_image_view_type(src_image->vk.image_type),
      .force_little_endian = true,
      .level_count = 1,
   };
   struct terakan_image_descriptor_create_info dst_descriptor_create_info = {
      .image = dst_image,
      .view_type = terakan_meta_transfer_image_view_type(dst_image->vk.image_type),
      .force_little_endian = true,
      .level_count = 1,
   };

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true, 0);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, ps_shader, false);

   if (is_linear) {
      terakan_hw_state_draw_set_sq_sampler_fs(&command_writer->hw_state_draw, 0,
                                              terakan_meta_blit_image_linear_sampler,
                                              (float[4]){0.0f, 0.0f, 0.0f, 0.0f});
   }

   terakan_meta_begin_cb(command_writer, V_028808_CB_NORMAL, 0xF, 0b1);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   uint32_t constants[TERAKAN_META_BLIT_IMAGE_CONSTS_COUNT] = {};
   struct terakan_bo const * constants_bo = NULL;
   uint32_t constants_va_lines;

   for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
      VkImageBlit2 const * const region = &pBlitImageInfo->pRegions[region_index];

      int32_t const src_x0 = region->srcOffsets[0].x;
      int32_t const src_y0 = region->srcOffsets[0].y;
      int32_t const src_x1 = region->srcOffsets[1].x;
      int32_t const src_y1 = region->srcOffsets[1].y;
      int32_t const dst_x0 = region->dstOffsets[0].x;
      int32_t const dst_y0 = region->dstOffsets[0].y;
      int32_t const dst_x1 = region->dstOffsets[1].x;
      int32_t const dst_y1 = region->dstOffsets[1].y;

      /* Reject degenerate (zero-extent) regions. Mirror blits (negative extent on either axis)
       * are supported: the dst rect is normalized to positive-extent for rasterization, while
       * `scale` uses the signed deltas so the per-fragment src coord lands on the mirrored
       * texel. FLT_TO_INT_FLOOR in the shader handles negative floats correctly.
       */
      if (src_x0 == src_x1 || src_y0 == src_y1 ||
          dst_x0 == dst_x1 || dst_y0 == dst_y1) {
         continue;
      }

      int32_t const rect_x = dst_x0 < dst_x1 ? dst_x0 : dst_x1;
      int32_t const rect_y = dst_y0 < dst_y1 ? dst_y0 : dst_y1;
      uint32_t const rect_w = (uint32_t)(dst_x0 < dst_x1 ? dst_x1 - dst_x0 : dst_x0 - dst_x1);
      uint32_t const rect_h = (uint32_t)(dst_y0 < dst_y1 ? dst_y1 - dst_y0 : dst_y0 - dst_y1);

      VkRect2D const rect = {
         .offset = {.x = rect_x, .y = rect_y},
         .extent = {.width = rect_w, .height = rect_h},
      };

      float const scale_x = (float)(src_x1 - src_x0) / (float)(dst_x1 - dst_x0);
      float const scale_y = (float)(src_y1 - src_y0) / (float)(dst_y1 - dst_y0);
      float const offset_x = (float)src_x0 - (float)dst_x0 * scale_x + 0.5f * scale_x;
      float const offset_y = (float)src_y0 - (float)dst_y0 * scale_y + 0.5f * scale_y;

      uint32_t const new_constants[TERAKAN_META_BLIT_IMAGE_CONSTS_COUNT] = {
         [TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_X] = fui(offset_x),
         [TERAKAN_META_BLIT_IMAGE_CONST_OFFSET_Y] = fui(offset_y),
         [TERAKAN_META_BLIT_IMAGE_CONST_SCALE_X] = fui(scale_x),
         [TERAKAN_META_BLIT_IMAGE_CONST_SCALE_Y] = fui(scale_y),
      };
      if (memcmp(constants, new_constants, sizeof(constants)) != 0) {
         constants_bo = NULL;
         memcpy(constants, new_constants, sizeof(constants));
      }

      if (constants_bo == NULL) {
         void * const constants_mapping = terakan_push_buffer_allocate_kcache(
            command_writer->base.command_buffer, sizeof(constants), &constants_bo,
            &constants_va_lines);
         if (unlikely(constants_mapping == NULL)) {
            return;
         }
         memcpy(constants_mapping, constants, sizeof(constants));
         terakan_hw_state_draw_set_sq_kcache_fs(
            &command_writer->hw_state_draw, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            DIV_ROUND_UP(sizeof(constants), TERAKAN_KCACHE_HW_LINE_BYTES), constants_bo,
            constants_va_lines);
      }

      src_descriptor_create_info.base_mip_level = region->srcSubresource.mipLevel;
      src_descriptor_create_info.base_array_layer = region->srcSubresource.baseArrayLayer;
      src_descriptor_create_info.layer_count =
         vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource);

      dst_descriptor_create_info.base_mip_level = region->dstSubresource.mipLevel;
      uint32_t const dst_base_layer = region->dstSubresource.baseArrayLayer;

      unsigned dst_aspect_mask_remaining = (unsigned)region->dstSubresource.aspectMask;
      u_foreach_bit (src_aspect_index, region->srcSubresource.aspectMask) {
         src_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
            src_image->format_info.aspect_map, (VkImageAspectFlags)1 << src_aspect_index, 0);
         dst_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
            dst_image->format_info.aspect_map,
            (VkImageAspectFlags)1 << u_bit_scan(&dst_aspect_mask_remaining), 0);

         src_descriptor_create_info.view_format =
            src_image->format_info.aspect_formats[src_descriptor_create_info.image_aspect_index];
         dst_descriptor_create_info.view_format =
            dst_image->format_info.aspect_formats[dst_descriptor_create_info.image_aspect_index];

         dst_descriptor_create_info.base_array_layer = dst_base_layer;
         dst_descriptor_create_info.layer_count = src_descriptor_create_info.layer_count;

         uint32_t src_resource[8];
         VkComponentMapping const identity_component_mapping = {};
         if (unlikely(!terakan_image_create_resource_descriptor(
                &src_descriptor_create_info, &identity_component_mapping, src_resource))) {
            assert(!"Invalid source image view create info");
            return;
         }

         while (dst_descriptor_create_info.layer_count > 0) {
            terakan_hw_state_draw_set_sq_resource_fs(
               &command_writer->hw_state_draw,
               TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META, src_image->bo, src_resource);

            struct terakan_color_descriptor dst_color;
            struct terakan_color_meta_descriptor dst_color_meta;
            uint32_t const dst_color_layer_count = terakan_image_create_color_descriptor(
               &dst_descriptor_create_info, &dst_color, &dst_color_meta);
            terakan_color_descriptor_image_view_to_color_attachment(&dst_color);
            terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_image->bo,
                                               &dst_color, &dst_color_meta, false);
            terakan_meta_set_db_shader_control_with_rtv(
               command_writer, terakan_meta_blit_image_nearest_ps.stage.ps.db_shader_control,
               dst_color.info);

            terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, dst_color_layer_count);

            src_resource[5] =
               (src_resource[5] & C_030014_BASE_ARRAY) |
               S_030014_BASE_ARRAY(G_030014_BASE_ARRAY(src_resource[5]) + dst_color_layer_count);
            dst_descriptor_create_info.base_array_layer += dst_color_layer_count;
            dst_descriptor_create_info.layer_count -= dst_color_layer_count;
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearColorImage(VkCommandBuffer const commandBuffer, VkImage const image,
                           UNUSED VkImageLayout const imageLayout,
                           VkClearColorValue const * const pColor, uint32_t const rangeCount,
                           VkImageSubresourceRange const * const pRanges)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const dst_image = terakan_image_from_handle(image);

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true, 0);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_CLEAR_COLOR_PS, false);

   terakan_meta_begin_cb(command_writer, V_028808_CB_NORMAL, 0xF, 0b1);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   /* The CLEAR_COLOR_PS reads the clear value from TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS as four
    * uint32s and exports them as the pixel. The CB applies the format conversion based on
    * CB_COLOR0_INFO::NUMBER_TYPE, which comes from the per-level/per-layer color descriptor
    * written below — so the shader is agnostic to the application's VkClearColorValue union
    * selector (float / int32 / uint32), matching the register layout contract shared with
    * terakan_meta_clear_attachments.c.
    */
   uint32_t const clear_value_size_bytes = sizeof(uint32_t) * 4;
   struct terakan_bo const * constants_bo;
   uint32_t constants_va_lines;
   void * const constants_mapping = terakan_push_buffer_allocate_kcache(
      command_writer->base.command_buffer, clear_value_size_bytes, &constants_bo,
      &constants_va_lines);
   if (unlikely(constants_mapping == NULL)) {
      return;
   }
   memcpy(constants_mapping, pColor->uint32, clear_value_size_bytes);
   terakan_hw_state_draw_set_sq_kcache_fs(
      &command_writer->hw_state_draw, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      DIV_ROUND_UP(clear_value_size_bytes, TERAKAN_KCACHE_HW_LINE_BYTES), constants_bo,
      constants_va_lines);

   struct terakan_image_descriptor_create_info descriptor_create_info = {
      .image = dst_image,
      .view_type = terakan_meta_transfer_image_view_type(dst_image->vk.image_type),
      .force_little_endian = true,
      .view_format = dst_image->format_info.aspect_formats[0],
      .image_aspect_index = 0,
      .level_count = 1,
   };

   for (uint32_t range_index = 0; range_index < rangeCount; ++range_index) {
      VkImageSubresourceRange const * const range = &pRanges[range_index];
      uint32_t const level_count = vk_image_subresource_level_count(&dst_image->vk, range);

      for (uint32_t level_offset = 0; level_offset < level_count; ++level_offset) {
         uint32_t const level = range->baseMipLevel + level_offset;
         descriptor_create_info.base_mip_level = level;

         VkExtent3D const level_extent = vk_image_mip_level_extent(&dst_image->vk, level);
         VkRect2D const rect = {
            .offset = {0, 0},
            .extent = {level_extent.width, level_extent.height},
         };

         if (dst_image->vk.image_type == VK_IMAGE_TYPE_3D) {
            descriptor_create_info.base_array_layer = 0;
            descriptor_create_info.layer_count = level_extent.depth;
         } else {
            descriptor_create_info.base_array_layer = range->baseArrayLayer;
            descriptor_create_info.layer_count =
               vk_image_subresource_layer_count(&dst_image->vk, range);
         }

         while (descriptor_create_info.layer_count > 0) {
            struct terakan_color_descriptor color_descriptor;
            struct terakan_color_meta_descriptor color_meta_descriptor;
            uint32_t const color_layer_count = terakan_image_create_color_descriptor(
               &descriptor_create_info, &color_descriptor, &color_meta_descriptor);
            terakan_color_descriptor_image_view_to_color_attachment(&color_descriptor);
            terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_image->bo,
                                               &color_descriptor, &color_meta_descriptor, false);
            terakan_meta_set_db_shader_control_with_rtv(
               command_writer, terakan_meta_clear_color_ps.stage.ps.db_shader_control,
               color_descriptor.info);

            terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, color_layer_count);

            descriptor_create_info.base_array_layer += color_layer_count;
            descriptor_create_info.layer_count -= color_layer_count;
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearDepthStencilImage(VkCommandBuffer const commandBuffer, VkImage const image,
                                  UNUSED VkImageLayout const imageLayout,
                                  VkClearDepthStencilValue const * const pDepthStencil,
                                  uint32_t const rangeCount,
                                  VkImageSubresourceRange const * const pRanges)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const dst_image = terakan_image_from_handle(image);

   /* terakan_image_create_depth_stencil_descriptor rejects images without
    * VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT because non-attachment DS images may be laid out
    * in a way that is incompatible with the DB (e.g. linear). The spec allows
    * vkCmdClearDepthStencilImage on any DS image with VK_IMAGE_USAGE_TRANSFER_DST_BIT, so this is
    * a conformance gap — fall back to a compute path once compute is wired up.
    */
   if (!(dst_image->vk.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      return;
   }

   command_writer->post_depth_stencil_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA | TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   float depth_value = pDepthStencil->depth;
   /* Guard against a NaN vertex Z (which would be discarded) and flush denormals so the value is
    * identical in every pipeline stage.
    */
   if (unlikely(isnan(depth_value) || fabsf(depth_value) < 0x1.0p-126)) {
      depth_value = 0.0f;
   }

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, false, 0);
   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_EMPTY_OPAQUE_PS, true);
   terakan_meta_begin_cb(command_writer, V_028808_CB_DISABLE, 0x0, 0b0);

   /* Viewport Z offset — reused for every range that clears depth. */
   terakan_state_draw_set_pending(&command_writer->state_draw, TERAKAN_STATE_DRAW_INDEX_VIEWPORT);
   struct terakan_hw_state_draw_viewport * const viewport =
      &command_writer->hw_state_draw.viewports[0];
   bool viewport_z_modified = false;
   if (memcmp(&viewport->pa_cl_vport_xyz_scale_offset[2][1], &depth_value, sizeof(float)) != 0) {
      command_writer->state_draw.viewport.viewports_pending.pa_cl_vport_z_scale_offset |=
         BITFIELD_BIT(0);
      viewport_z_modified = true;
      viewport->pa_cl_vport_xyz_scale_offset[2][1] = depth_value;
   }
   if (memcmp(&viewport->pa_sc_vport_z_min_max[0], &depth_value, sizeof(float)) != 0 ||
       memcmp(&viewport->pa_sc_vport_z_min_max[1], &depth_value, sizeof(float)) != 0) {
      command_writer->state_draw.viewport.viewports_pending.pa_sc_vport_z_min_max |=
         BITFIELD_BIT(0);
      viewport_z_modified = true;
      viewport->pa_sc_vport_z_min_max[0] = depth_value;
      viewport->pa_sc_vport_z_min_max[1] = depth_value;
   }
   terakan_hw_state_draw_update_viewports(
      &command_writer->hw_state_draw, 1,
      viewport_z_modified ? 0 : ARRAY_SIZE(command_writer->hw_state_draw.viewports),
      ARRAY_SIZE(command_writer->hw_state_draw.viewports));

   /* Stencil ref/mask — reused for every range that clears stencil. */
   uint32_t const db_stencilrefmask =
      S_028430_STENCILREF(pDepthStencil->stencil) | S_028430_STENCILWRITEMASK(UINT8_MAX);
   bool const db_stencilrefmask_modified =
      command_writer->hw_state_draw.db_stencilrefmask_front_back[0] != db_stencilrefmask ||
      command_writer->hw_state_draw.db_stencilrefmask_front_back[1] != db_stencilrefmask;
   if (db_stencilrefmask_modified) {
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_DB_STENCILREFMASK);
      command_writer->hw_state_draw.db_stencilrefmask_front_back[0] = db_stencilrefmask;
      command_writer->hw_state_draw.db_stencilrefmask_front_back[1] = db_stencilrefmask;
   }
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_DB_STENCILREFMASK,
                                 db_stencilrefmask_modified);

   VkImageViewCreateInfo view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = terakan_meta_transfer_image_view_type(dst_image->vk.image_type),
      .format = dst_image->vk.format,
      .subresourceRange =
         {
            .baseArrayLayer = 0,
         },
   };

   for (uint32_t range_index = 0; range_index < rangeCount; ++range_index) {
      VkImageSubresourceRange const * const range = &pRanges[range_index];
      VkImageAspectFlags const range_aspects =
         range->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
      if (range_aspects == 0) {
         continue;
      }

      uint32_t db_depth_control = 0;
      if (range_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         db_depth_control |= S_028800_Z_ENABLE(1) | S_028800_Z_WRITE_ENABLE(1) |
                             S_028800_ZFUNC((uint32_t)VK_COMPARE_OP_ALWAYS);
      }
      if (range_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         db_depth_control |= S_028800_STENCIL_ENABLE(1) |
                             S_028800_STENCILFUNC(V_028800_STENCILFUNC_ALWAYS) |
                             S_028800_STENCILFAIL(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZPASS(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZFAIL(V_028800_STENCIL_REPLACE);
      }
      terakan_meta_set_db_depth_control(command_writer, db_depth_control);
      terakan_meta_set_pa_cl_vte_cntl(
         command_writer,
         TERAKAN_META_PA_CL_VTE_CNTL_2D |
            ((range_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? S_028818_VPORT_Z_OFFSET_ENA(1) : 0));

      view_create_info.subresourceRange.aspectMask = range_aspects;

      uint32_t const level_count = vk_image_subresource_level_count(&dst_image->vk, range);
      for (uint32_t level_offset = 0; level_offset < level_count; ++level_offset) {
         uint32_t const level = range->baseMipLevel + level_offset;
         view_create_info.subresourceRange.baseMipLevel = level;
         view_create_info.subresourceRange.levelCount = 1;

         VkExtent3D const level_extent = vk_image_mip_level_extent(&dst_image->vk, level);

         uint32_t layer_count;
         if (dst_image->vk.image_type == VK_IMAGE_TYPE_3D) {
            view_create_info.subresourceRange.baseArrayLayer = 0;
            view_create_info.subresourceRange.layerCount = level_extent.depth;
            layer_count = level_extent.depth;
         } else {
            view_create_info.subresourceRange.baseArrayLayer = range->baseArrayLayer;
            view_create_info.subresourceRange.layerCount =
               vk_image_subresource_layer_count(&dst_image->vk, range);
            layer_count = view_create_info.subresourceRange.layerCount;
         }

         struct terakan_depth_stencil_descriptor depth_stencil_descriptor;
         if (!terakan_image_create_depth_stencil_descriptor(&view_create_info,
                                                            &depth_stencil_descriptor)) {
            continue;
         }

         /* Mask off aspects this range isn't clearing so the DB only touches the requested ones. */
         if (!(range_aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
            depth_stencil_descriptor.z_info =
               (depth_stencil_descriptor.z_info & C_028040_FORMAT) |
               S_028040_FORMAT(V_028040_Z_INVALID);
         }
         if (!(range_aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            depth_stencil_descriptor.stencil_info =
               (depth_stencil_descriptor.stencil_info & C_028044_FORMAT) |
               S_028044_FORMAT(V_028044_STENCIL_INVALID);
         }

         command_writer->hw_state_draw.db_depth_stencil_buffer.bo = dst_image->bo;
         command_writer->hw_state_draw.db_depth_stencil_buffer.descriptor =
            depth_stencil_descriptor;
         terakan_state_draw_set_pending(&command_writer->state_draw,
                                        TERAKAN_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER);
         terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                       TERAKAN_HW_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER, true);

         VkRect2D const rect = {
            .offset = {0, 0},
            .extent = {level_extent.width, level_extent.height},
         };
         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, layer_count);
      }
   }
}

/* HW CB_RESOLVE path: Evergreen's color backend can average MSAA samples into a 1x destination
 * in a single pass. CB[0] = MSAA src, CB[1] = 1x dst, CB_COLOR_CONTROL::MODE = V_028808_CB_RESOLVE
 * (`gallium/drivers/r600/evergreend.h:790-795`). The PS output is ignored by CB_RESOLVE mode —
 * EMPTY_OPAQUE_PS is used only to satisfy the SQ "every draw exports at least one color" rule.
 * The matching render-pass end-of-subpass resolve stub lives at terakan_render_pass.c:148.
 *
 * Gating (mirrors r600_blit.c:1034-1040 for the HW subset):
 *   - src samples > 1, dst samples == 1 (spec also requires this)
 *   - !vk_format_is_pure_integer (CB averager can't average integer formats)
 *   - !vk_format_is_depth_or_stencil (DB-based depth resolve is a separate future path)
 *   - per-region: offsets == 0, extent == mip extent, src and dst extents match
 *
 * Non-HW cases (arbitrary offset/extent, integer formats, DS): silent no-op for now. r600g's
 * fallback (resolve-to-scratch + blit, r600_blit.c:1075-1112) depends on CmdBlitImage2
 * (task #16) landing first. 8x MSAA + integer formats need a shader-based resolve; that is
 * deferred past v1.0 since advertising VK_SAMPLE_COUNT_8_BIT at
 * framebufferColorSampleCounts without a working resolve is itself a conformance gap — if a
 * future conformance run flags it, retract the 8x advertise instead of writing the shader.
 */
VKAPI_ATTR void VKAPI_CALL
terakan_CmdResolveImage2(VkCommandBuffer const commandBuffer,
                         VkResolveImageInfo2 const * const pResolveImageInfo)
{
   struct terakan_image const * const src_image =
      terakan_image_from_handle(pResolveImageInfo->srcImage);
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(pResolveImageInfo->dstImage);

   if (src_image->vk.samples <= 1 || dst_image->vk.samples != 1) {
      return;
   }
   enum pipe_format const src_pipe_format = vk_format_to_pipe_format(src_image->vk.format);
   if (util_format_is_pure_integer(src_pipe_format) ||
       util_format_is_depth_or_stencil(src_pipe_format)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   uint32_t const num_samples_log2 = util_logbase2((uint32_t)src_image->vk.samples);

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true,
                                         num_samples_log2);
   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_EMPTY_OPAQUE_PS, false);

   /* target_mask: 4-bit RGBA nibble per CB slot; 0xFF enables RGBA on slots 0 and 1.
    * disable_blend_for_targets: 0b11 disables blend for both CB slots (blend must not touch the
    * CB_RESOLVE path).
    */
   terakan_meta_begin_cb(command_writer, V_028808_CB_RESOLVE, 0xFF, 0b11);

   struct terakan_image_descriptor_create_info src_descriptor_create_info = {
      .image = src_image,
      .view_type = terakan_meta_transfer_image_view_type(src_image->vk.image_type),
      .force_little_endian = true,
      .view_format = src_image->format_info.aspect_formats[0],
      .image_aspect_index = 0,
      .level_count = 1,
   };
   struct terakan_image_descriptor_create_info dst_descriptor_create_info = {
      .image = dst_image,
      .view_type = terakan_meta_transfer_image_view_type(dst_image->vk.image_type),
      .force_little_endian = true,
      .view_format = dst_image->format_info.aspect_formats[0],
      .image_aspect_index = 0,
      .level_count = 1,
   };

   for (uint32_t region_index = 0; region_index < pResolveImageInfo->regionCount; ++region_index) {
      VkImageResolve2 const * const region = &pResolveImageInfo->pRegions[region_index];

      VkExtent3D const src_mip_extent =
         vk_image_mip_level_extent(&src_image->vk, region->srcSubresource.mipLevel);
      VkExtent3D const dst_mip_extent =
         vk_image_mip_level_extent(&dst_image->vk, region->dstSubresource.mipLevel);

      /* CB_RESOLVE HW subset: full-extent 1:1 copy only. */
      if (region->srcOffset.x != 0 || region->srcOffset.y != 0 || region->srcOffset.z != 0 ||
          region->dstOffset.x != 0 || region->dstOffset.y != 0 || region->dstOffset.z != 0 ||
          region->extent.width != src_mip_extent.width ||
          region->extent.height != src_mip_extent.height ||
          region->extent.width != dst_mip_extent.width ||
          region->extent.height != dst_mip_extent.height) {
         continue;
      }

      VkRect2D const rect = {
         .offset = {0, 0},
         .extent = {region->extent.width, region->extent.height},
      };

      src_descriptor_create_info.base_mip_level = region->srcSubresource.mipLevel;
      src_descriptor_create_info.base_array_layer = region->srcSubresource.baseArrayLayer;
      src_descriptor_create_info.layer_count =
         vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource);

      dst_descriptor_create_info.base_mip_level = region->dstSubresource.mipLevel;
      dst_descriptor_create_info.base_array_layer = region->dstSubresource.baseArrayLayer;
      dst_descriptor_create_info.layer_count =
         vk_image_subresource_layer_count(&dst_image->vk, &region->dstSubresource);

      while (dst_descriptor_create_info.layer_count > 0) {
         struct terakan_color_descriptor src_color;
         struct terakan_color_meta_descriptor src_color_meta;
         uint32_t const src_layer_count = terakan_image_create_color_descriptor(
            &src_descriptor_create_info, &src_color, &src_color_meta);
         terakan_color_descriptor_image_view_to_color_attachment(&src_color);

         struct terakan_color_descriptor dst_color;
         struct terakan_color_meta_descriptor dst_color_meta;
         uint32_t const dst_layer_count = terakan_image_create_color_descriptor(
            &dst_descriptor_create_info, &dst_color, &dst_color_meta);
         terakan_color_descriptor_image_view_to_color_attachment(&dst_color);

         uint32_t const layer_count = MIN2(src_layer_count, dst_layer_count);

         terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, src_image->bo,
                                            &src_color, &src_color_meta, false);
         terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 1, dst_image->bo,
                                            &dst_color, &dst_color_meta, false);
         terakan_meta_set_db_shader_control_with_rtv(
            command_writer, TERAKAN_META_DB_SHADER_CONTROL_DEFAULT, dst_color.info);

         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, layer_count);

         src_descriptor_create_info.base_array_layer += layer_count;
         src_descriptor_create_info.layer_count -= layer_count;
         dst_descriptor_create_info.base_array_layer += layer_count;
         dst_descriptor_create_info.layer_count -= layer_count;
      }
   }
}
