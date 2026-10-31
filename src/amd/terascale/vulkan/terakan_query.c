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

/* Terakan does not implement real query pools. These no-op stubs exist so the
 * WSI code can look up vkDestroyQueryPool unconditionally — it calls it during
 * swapchain teardown even when no query pool was ever created (handle is
 * VK_NULL_HANDLE in that case, which per the Vulkan spec must be a no-op). */

#include "terakan_entrypoints.h"

#include <vulkan/vulkan_core.h>

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateQueryPool(VkDevice device,
                        const VkQueryPoolCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkQueryPool *pQueryPool)
{
   *pQueryPool = VK_NULL_HANDLE;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyQueryPool(VkDevice device,
                         VkQueryPool queryPool,
                         const VkAllocationCallbacks *pAllocator)
{
}
