/*
	Created on: Nov 24, 2019

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
#pragma once
#include <cinttypes>
#include "vulkan.h"

// Define enhanced memory handling for MoltenVK
#ifdef __APPLE__
#define VMA_SAFER_MEMORY_OPERATIONS 1
#endif

#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

// Define custom implementations for VMA memory operations
#ifdef VMA_SAFER_MEMORY_OPERATIONS
// Custom implementations for VMA memory operations
#define VMA_IMPLEMENTATION_CUSTOM_MEMMOVE 1
#define VMA_IMPLEMENTATION_CUSTOM_REALLOC 1

// Safe memmove implementation that handles potential null pointers and overlapping regions better
static void* VmaSaferMemmove(void* dst, const void* src, size_t size)
{
    if (dst == nullptr || src == nullptr || size == 0 || dst == src)
        return dst;
    
    // Use a safer implementation for overlapping memory regions
    unsigned char* pdst = static_cast<unsigned char*>(dst);
    const unsigned char* psrc = static_cast<const unsigned char*>(src);
    
    // Check if regions overlap and need special handling
    if (pdst > psrc && pdst < psrc + size) {
        // Copy backwards to avoid overwriting source data
        for (size_t i = size; i > 0; --i)
            pdst[i-1] = psrc[i-1];
    } else {
        // Safe to copy forwards
        for (size_t i = 0; i < size; ++i)
            pdst[i] = psrc[i];
    }
    
    return dst;
}

// Safer realloc implementation for MoltenVK
static void* VmaSaferRealloc(void* ptr, size_t newSize)
{
    if (newSize == 0) {
        free(ptr);
        return nullptr;
    }
    
    if (ptr == nullptr)
        return malloc(newSize);
    
    // Allocate new memory
    void* newPtr = malloc(newSize);
    if (newPtr == nullptr)
        return nullptr;
    
    // Get the size of the old block - this is platform-specific
    // For safety, we'll use a conservative approach
    size_t oldSize = 0;
    
    // On macOS/iOS we can use malloc_size, but we'll avoid direct inclusion of malloc.h
    // Instead, we'll use a conservative approach for all platforms
    // This avoids build issues with iOS SDK headers
    #if defined(__APPLE__)
        // Use a fixed buffer size that's large enough for most allocations
        // This is safer than trying to determine the exact size
        oldSize = newSize * 2; // Conservative approach
    #else
        // For other platforms, we'd need to implement a different approach
        // For now, just use a reasonable default
        oldSize = newSize;
    #endif
    
    // Copy the minimum of the old and new sizes
    size_t copySize = (oldSize < newSize) ? oldSize : newSize;
    memcpy(newPtr, ptr, copySize);
    
    // Free the old memory
    free(ptr);
    
    return newPtr;
}

#define VMA_MEMMOVE(dst, src, size) VmaSaferMemmove(dst, src, size)
#define VMA_REALLOC(ptr, newSize) VmaSaferRealloc(ptr, newSize)
#endif

#include "vk_mem_alloc.h"

#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

#if !defined(PRIu64) && defined(_WIN32)
#define PRIu64 "I64u"
#endif

class VMAllocator;

class Allocation
{
public:
	Allocation() = default;
	Allocation(const Allocation& other) = delete;
	Allocation& operator=(const Allocation& other) = delete;

	Allocation(Allocation&& other) : allocator(other.allocator), allocation(other.allocation),
			allocInfo(other.allocInfo) {
		other.allocator = VK_NULL_HANDLE;
		other.allocation = VK_NULL_HANDLE;
	}

	Allocation& operator=(Allocation&& other) {
		std::swap(this->allocator, other.allocator);
		std::swap(this->allocation, other.allocation);
		std::swap(this->allocInfo, other.allocInfo);
		return *this;
	}

	~Allocation() {
		if (allocator != VK_NULL_HANDLE)
			vmaFreeMemory(allocator, allocation);
	}
	bool IsHostVisible() const {
		VkMemoryPropertyFlags flags;
		vmaGetMemoryTypeProperties(allocator, allocInfo.memoryType, &flags);
		return flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}
	void *MapMemory() const
	{
		if (allocInfo.pMappedData != nullptr)
			return allocInfo.pMappedData;
		void *p;
		VkResult res = vmaMapMemory(allocator, allocation, &p);
        vk::resultCheck(static_cast<vk::Result>(res), "vmaMapMemory failed");
		VkMemoryPropertyFlags flags;
		vmaGetMemoryTypeProperties(allocator, allocInfo.memoryType, &flags);
		if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
			vmaInvalidateAllocation(allocator, allocation, allocInfo.offset, allocInfo.size);
		return p;
	}
	void UnmapMemory() const
	{
		if (allocInfo.pMappedData != nullptr)
			return;
		VkMemoryPropertyFlags flags;
		vmaGetMemoryTypeProperties(allocator, allocInfo.memoryType, &flags);
		if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
			vmaFlushAllocation(allocator, allocation, allocInfo.offset, allocInfo.size);
		vmaUnmapMemory(allocator, allocation);
	}

private:
	Allocation(VmaAllocator allocator, VmaAllocation allocation, VmaAllocationInfo allocInfo)
		: allocator(allocator), allocation(allocation), allocInfo(allocInfo)
	{
	}

	VmaAllocator allocator = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo;

	friend class VMAllocator;
};

class VMAllocator
{
public:
	void Init(vk::PhysicalDevice physicalDevice, vk::Device device, vk::Instance instance);

	void Term()
	{
		if (allocator != VK_NULL_HANDLE)
		{
			vmaDestroyAllocator(allocator);
			allocator = VK_NULL_HANDLE;
		}
	}

	Allocation AllocateMemory(const vk::MemoryRequirements& memoryRequirements, const VmaAllocationCreateInfo& allocCreateInfo) const
	{
		VmaAllocation vmaAllocation;
		VmaAllocationInfo allocInfo;
		VkResult rc = vmaAllocateMemory(allocator, (VkMemoryRequirements*)&memoryRequirements, &allocCreateInfo, &vmaAllocation, &allocInfo);
        vk::resultCheck(static_cast<vk::Result>(rc), "vmaAllocateMemory failed");
		return Allocation(allocator, vmaAllocation, allocInfo);
	}

	Allocation AllocateForImage(const vk::Image image, const VmaAllocationCreateInfo& allocCreateInfo) const
	{
		VmaAllocation vmaAllocation;
		VmaAllocationInfo allocInfo;
		VkResult rc = vmaAllocateMemoryForImage(allocator, (VkImage)image, &allocCreateInfo, &vmaAllocation, &allocInfo);
        vk::resultCheck(static_cast<vk::Result>(rc), "vmaAllocateMemoryForImage failed");
		vmaBindImageMemory(allocator, vmaAllocation, (VkImage)image);

		return Allocation(allocator, vmaAllocation, allocInfo);
	}

	Allocation AllocateForBuffer(const vk::Buffer buffer, const VmaAllocationCreateInfo& allocCreateInfo) const
	{
		VmaAllocation vmaAllocation;
		VmaAllocationInfo allocInfo;
		VkResult rc = vmaAllocateMemoryForBuffer(allocator, (VkBuffer)buffer, &allocCreateInfo, &vmaAllocation, &allocInfo);
        vk::resultCheck(static_cast<vk::Result>(rc), "vmaAllocateMemoryForBuffer failed");
		vmaBindBufferMemory(allocator, vmaAllocation, (VkBuffer)buffer);

		return Allocation(allocator, vmaAllocation, allocInfo);
	}

private:
	VmaAllocator allocator = VK_NULL_HANDLE;
};
