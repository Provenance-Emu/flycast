/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "buffer.h"
#include "vulkan_context.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

void optimized_buffer_copy(void* dst, const void* src, size_t size)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    // Handle small copies directly
    if (size < 16)
    {
        for (size_t i = 0; i < size; i++)
            d[i] = s[i];
        return;
    }

    // Align to 16-byte boundary
    size_t pre = (16 - (size_t)d) & 15;
    if (pre > 0)
    {
        for (size_t i = 0; i < pre; i++)
            d[i] = s[i];
        d += pre;
        s += pre;
        size -= pre;
    }

    // Copy 16 bytes at a time
    size_t main = size & ~15;
    for (size_t i = 0; i < main; i += 16)
    {
        uint8x16_t v = vld1q_u8(s + i);
        vst1q_u8(d + i, v);
    }

    // Copy remaining bytes
    for (size_t i = main; i < size; i++)
        d[i] = s[i];
}
#endif

BufferData::BufferData(vk::DeviceSize size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags propertyFlags)
	: bufferSize(size), m_usage(usage)
{
	VulkanContext *context = VulkanContext::Instance();
	buffer = context->GetDevice().createBufferUnique(vk::BufferCreateInfo(vk::BufferCreateFlags(), size, usage));
	VmaAllocationCreateInfo allocInfo {};
	if (propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)
	{
		allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	}
	else
	{
		// FIXME VMA_ALLOCATION_CREATE_MAPPED_BIT ?
#ifdef __APPLE__
		// MoltenVK memory management improvements for 1.2.11+
		allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		
		// Check for Metal argument buffers configuration
		static bool checkedMoltenVKConfig = false;
		static bool useHostCoherent = false;
		static bool usingMetalArgumentBuffers = false;
		
		if (!checkedMoltenVKConfig) {
			// For MoltenVK 1.2.11+, we should allow host coherent memory
			useHostCoherent = true;
			
			// Check if we're using Metal argument buffers
			// This is detected in vulkan_context.cpp when initializing device extensions
			// We can detect it by checking for both VK_EXT_METAL_OBJECTS_EXTENSION_NAME and 
			// VK_KHR_portability_subset extensions
			const auto deviceExtensionProperties = VulkanContext::Instance()->GetPhysicalDevice().enumerateDeviceExtensionProperties();
			bool hasMetalObjects = false;
			bool hasPortabilitySubset = false;
			
			for (const auto& property : deviceExtensionProperties) {
				if (strcmp(property.extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME) == 0)
					hasMetalObjects = true;
				if (strcmp(property.extensionName, "VK_KHR_portability_subset") == 0)
					hasPortabilitySubset = true;
			}
			
			usingMetalArgumentBuffers = hasMetalObjects && hasPortabilitySubset;
			
			if (usingMetalArgumentBuffers) {
				// When using Metal argument buffers, we need to be more conservative
				// with memory allocations to prevent crashes during scene transitions
				INFO_LOG(RENDERER, "Using conservative memory allocation for MoltenVK with Metal argument buffers");
				
				// Use additional safety flags for Metal argument buffers
				allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
				
				// For larger buffers, use host visible memory to reduce GPU memory pressure
				if (size > 1024 * 1024) { // 1MB threshold
					allocInfo.preferredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
				}
			}
			
			checkedMoltenVKConfig = true;
		}
		
		if (!useHostCoherent) {
			// Disable host coherent for older MoltenVK versions
			propertyFlags &= ~vk::MemoryPropertyFlagBits::eHostCoherent;
		}
#endif
		if (propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)
		{
			allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
			if (propertyFlags & vk::MemoryPropertyFlagBits::eHostCached)
				allocInfo.preferredFlags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			if (propertyFlags & vk::MemoryPropertyFlagBits::eHostCoherent)
				allocInfo.preferredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		}
	}
	allocation = context->GetAllocator().AllocateForBuffer(*buffer, allocInfo);
}

BufferPacker::BufferPacker()
{
	uniformAlignment = VulkanContext::Instance()->GetUniformBufferAlignment();
	storageAlignment = VulkanContext::Instance()->GetStorageBufferAlignment();
}

void BufferData::upload(u32 size, const void *data, u32 bufOffset) const
{
    verify(bufOffset + size <= bufferSize);

    void* dataPtr = (u8 *)allocation.MapMemory() + bufOffset;
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    optimized_buffer_copy(dataPtr, data, size);
#else
    memcpy(dataPtr, data, size);
#endif
    allocation.UnmapMemory();
}
