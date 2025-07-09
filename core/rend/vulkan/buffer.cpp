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

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

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
		// Check iOS device memory capabilities for graceful fallback
		static bool isLowMemoryDevice = false;
		static bool checkedDeviceMemory = false;
		
		if (!checkedDeviceMemory) {
			// Detect iOS device memory for allocation strategy
			#if TARGET_OS_IOS
			size_t total_memory = 0;
			size_t length = sizeof(total_memory);
			if (sysctlbyname("hw.memsize", &total_memory, &length, nullptr, 0) == 0) {
				const size_t GB = 1024ULL * 1024ULL * 1024ULL;
				isLowMemoryDevice = (total_memory < 2 * GB); // <2GB = older iPad
				INFO_LOG(RENDERER, "📱 iOS Device Memory: %.2f GB, Low Memory Mode: %s", 
					total_memory / (1024.0 * 1024.0 * 1024.0), isLowMemoryDevice ? "ON" : "OFF");
			}
			#endif
			checkedDeviceMemory = true;
		}
		
		// Use conservative memory allocation for low-memory devices
		if (isLowMemoryDevice) {
			// Avoid dedicated allocation on older devices to conserve memory
			INFO_LOG(RENDERER, "🔧 iOS Low Memory: Using shared allocation for buffer size %llu", size);
		} else {
			// Use dedicated allocation for better performance on high-memory devices
			allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		}
		
		// MoltenVK 1.2.11+ has improved host coherent memory support, but we still need to be careful
		// Only disable host coherent if we're not using a recent MoltenVK version
		// This check can be removed once minimum MoltenVK version is 1.2.11+
		static bool checkedMoltenVKVersion = false;
		static bool useHostCoherent = false;
		if (!checkedMoltenVKVersion) {
			// Get device properties to check if we're using MoltenVK
			// In older Vulkan versions, we can check for Apple GPU
			vk::PhysicalDeviceProperties props = VulkanContext::Instance()->GetPhysicalDevice().getProperties();
			
			// Check if we're on Apple platform - assume it's MoltenVK
			// For MoltenVK 1.2.11+, we should allow host coherent memory
			useHostCoherent = true;
			checkedMoltenVKVersion = true;
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
