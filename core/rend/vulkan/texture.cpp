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
#include "texture.h"

#include <algorithm>
#include <memory>

/// iOS MoltenVK texture streaming optimizations for FMV performance
#if defined(__APPLE__) && defined(TARGET_IPHONE)
// Use C++ compatible includes for iOS optimization
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <unistd.h>

/// iOS-specific constants for optimal texture streaming
#define IOS_TEXTURE_POOL_SIZE 32
#define IOS_STAGING_BUFFER_POOL_SIZE 16
#define IOS_CACHE_LINE_SIZE 64
#define IOS_MEMORY_ALIGNMENT 256
#define IOS_ASYNC_UPLOAD_THRESHOLD (512 * 512)  // 512x512 pixels
#define IOS_LARGE_TEXTURE_THRESHOLD (1024 * 1024)  // 1MB

/// iOS texture streaming performance metrics
struct IOSTextureMetrics {
	uint64_t total_uploads = 0;
	uint64_t async_uploads = 0;
	uint64_t pool_hits = 0;
	uint64_t pool_misses = 0;
	uint64_t staging_reuse = 0;
	double avg_upload_time_ms = 0.0;
	
	void logPerformanceStats() {
		if (total_uploads > 0) {
			INFO_LOG(RENDERER, "🚀 iOS Texture Streaming Stats: %llu uploads (%.1f%% async), %.1f%% pool hits, %.2fms avg",
			         total_uploads, 
			         (async_uploads * 100.0) / total_uploads,
			         (pool_hits * 100.0) / (pool_hits + pool_misses),
			         avg_upload_time_ms);
		}
	}
} g_ios_tex_metrics;

/// iOS texture pool for efficient reuse
struct IOSTexturePool {
	struct PooledTexture {
		vk::UniqueImage image;
		vk::UniqueImageView imageView;
		Allocation allocation;
		vk::Extent2D extent;
		vk::Format format;
		u32 mipmapLevels;
		bool inUse = false;
		uint64_t lastUsed = 0;
		
		bool matches(vk::Extent2D reqExtent, vk::Format reqFormat, u32 reqMipmaps) const {
			return extent.width == reqExtent.width && 
			       extent.height == reqExtent.height &&
			       format == reqFormat && 
			       mipmapLevels == reqMipmaps;
		}
	};
	
	std::vector<std::unique_ptr<PooledTexture>> textures;
	std::mutex poolMutex;
	
	PooledTexture* acquireTexture(vk::Extent2D extent, vk::Format format, u32 mipmapLevels) {
		std::lock_guard<std::mutex> lock(poolMutex);
		
		// Try to find matching unused texture
		for (auto& tex : textures) {
			if (!tex->inUse && tex->matches(extent, format, mipmapLevels)) {
				tex->inUse = true;
				tex->lastUsed = mach_absolute_time();
				g_ios_tex_metrics.pool_hits++;
				return tex.get();
			}
		}
		
		// Create new texture if pool not full
		if (textures.size() < IOS_TEXTURE_POOL_SIZE) {
			auto newTex = std::make_unique<PooledTexture>();
			newTex->extent = extent;
			newTex->format = format;
			newTex->mipmapLevels = mipmapLevels;
			newTex->inUse = true;
			newTex->lastUsed = mach_absolute_time();
			
			PooledTexture* result = newTex.get();
			textures.push_back(std::move(newTex));
			g_ios_tex_metrics.pool_misses++;
			return result;
		}
		
		// Pool full, reuse oldest texture
		PooledTexture* oldest = nullptr;
		uint64_t oldestTime = UINT64_MAX;
		for (auto& tex : textures) {
			if (!tex->inUse && tex->lastUsed < oldestTime) {
				oldest = tex.get();
				oldestTime = tex->lastUsed;
			}
		}
		
		if (oldest) {
			oldest->inUse = true;
			oldest->lastUsed = mach_absolute_time();
			oldest->extent = extent;
			oldest->format = format;
			oldest->mipmapLevels = mipmapLevels;
			// Will need to recreate image/view for this texture
			oldest->image.reset();
			oldest->imageView.reset();
			g_ios_tex_metrics.pool_misses++;
			return oldest;
		}
		
		g_ios_tex_metrics.pool_misses++;
		return nullptr;  // Fallback to regular allocation
	}
	
	void releaseTexture(PooledTexture* texture) {
		std::lock_guard<std::mutex> lock(poolMutex);
		texture->inUse = false;
	}
	
	void cleanup() {
		std::lock_guard<std::mutex> lock(poolMutex);
		uint64_t currentTime = mach_absolute_time();
		static mach_timebase_info_data_t timebase = {};
		if (timebase.denom == 0) {
			mach_timebase_info(&timebase);
		}
		
		// Remove textures unused for more than 5 seconds
		const uint64_t maxAge = 5ULL * 1000000000ULL * timebase.denom / timebase.numer;
		
		textures.erase(
			std::remove_if(textures.begin(), textures.end(),
				[currentTime, maxAge](const auto& tex) {
					return !tex->inUse && (currentTime - tex->lastUsed) > maxAge;
				}),
			textures.end()
		);
	}
} g_ios_texture_pool;

/// iOS staging buffer pool for efficient memory reuse
struct IOSStagingBufferPool {
	struct PooledBuffer {
		std::unique_ptr<BufferData> bufferData;
		u32 size;
		bool inUse = false;
		uint64_t lastUsed = 0;
	};
	
	std::vector<std::unique_ptr<PooledBuffer>> buffers;
	std::mutex poolMutex;
	
	BufferData* acquireBuffer(u32 size) {
		std::lock_guard<std::mutex> lock(poolMutex);
		
		// Find best fit buffer (smallest that's >= required size)
		PooledBuffer* bestFit = nullptr;
		u32 bestSize = UINT32_MAX;
		
		for (auto& buf : buffers) {
			if (!buf->inUse && buf->size >= size && buf->size < bestSize) {
				bestFit = buf.get();
				bestSize = buf->size;
			}
		}
		
		if (bestFit) {
			bestFit->inUse = true;
			bestFit->lastUsed = mach_absolute_time();
			g_ios_tex_metrics.staging_reuse++;
			return bestFit->bufferData.get();
		}
		
		// Create new buffer if pool not full
		if (buffers.size() < IOS_STAGING_BUFFER_POOL_SIZE) {
			auto newBuf = std::make_unique<PooledBuffer>();
			newBuf->size = std::max(size, 1024u * 1024u);  // Minimum 1MB for efficiency
			newBuf->bufferData = std::make_unique<BufferData>(newBuf->size, vk::BufferUsageFlagBits::eTransferSrc);
			newBuf->inUse = true;
			newBuf->lastUsed = mach_absolute_time();
			
			BufferData* result = newBuf->bufferData.get();
			buffers.push_back(std::move(newBuf));
			return result;
		}
		
		return nullptr;  // Pool exhausted, fallback to regular allocation
	}
	
	void releaseBuffer(BufferData* buffer) {
		std::lock_guard<std::mutex> lock(poolMutex);
		for (auto& buf : buffers) {
			if (buf->bufferData.get() == buffer) {
				buf->inUse = false;
				break;
			}
		}
	}
} g_ios_staging_pool;

/// iOS async texture upload tracking
struct IOSAsyncUpload {
	vk::Fence fence;
	BufferData* stagingBuffer = nullptr;
	uint64_t startTime;
	u32 textureSize;
	
	bool isComplete() const {
		if (!fence) return true;
		VulkanContext* ctx = VulkanContext::Instance();
		return ctx->GetDevice().getFenceStatus(fence) == vk::Result::eSuccess;
	}
};

static std::vector<IOSAsyncUpload> g_pending_uploads;
static std::mutex g_async_mutex;

/// iOS unified memory optimization for texture data
static void OptimizeIOSTextureMemory(const void* data, u32 size) {
	if (!data || size == 0) return;
	
	// Prefetch data for Metal unified memory architecture
	const char* ptr = static_cast<const char*>(data);
	for (u32 i = 0; i < size; i += IOS_CACHE_LINE_SIZE) {
		__builtin_prefetch(ptr + i, 0, 3);  // High temporal locality for textures
	}
	
	// Memory barrier for coherency on iOS unified memory
	__sync_synchronize();
}

/// Process completed async uploads and free resources
static void ProcessIOSAsyncUploads() {
	std::lock_guard<std::mutex> lock(g_async_mutex);
	
	for (auto it = g_pending_uploads.begin(); it != g_pending_uploads.end();) {
		if (it->isComplete()) {
			// Calculate upload time for metrics
			uint64_t endTime = mach_absolute_time();
			static mach_timebase_info_data_t timebase = {};
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			double uploadTimeMs = (double)(endTime - it->startTime) * timebase.numer / timebase.denom / 1000000.0;
			
			// Update running average
			g_ios_tex_metrics.avg_upload_time_ms = 
				(g_ios_tex_metrics.avg_upload_time_ms * g_ios_tex_metrics.total_uploads + uploadTimeMs) / 
				(g_ios_tex_metrics.total_uploads + 1);
			
			// Release staging buffer back to pool
			if (it->stagingBuffer) {
				g_ios_staging_pool.releaseBuffer(it->stagingBuffer);
			}
			
			// Clean up fence
			if (it->fence) {
				VulkanContext::Instance()->GetDevice().destroyFence(it->fence);
			}
			
			it = g_pending_uploads.erase(it);
		} else {
			++it;
		}
	}
}

/// iOS texture upload with advanced streaming optimizations
static bool UploadTextureIOSOptimized(Texture* texture, u32 srcSize, const void* srcData, 
                                       bool isNew, bool genMipmaps, vk::CommandBuffer commandBuffer) {
	if (!srcData || srcSize == 0) return false;
	
	uint64_t startTime = mach_absolute_time();
	g_ios_tex_metrics.total_uploads++;
	
	// Optimize memory layout for iOS
	OptimizeIOSTextureMemory(srcData, srcSize);
	
	// Determine if we should use async upload
	bool useAsync = srcSize >= IOS_ASYNC_UPLOAD_THRESHOLD && !genMipmaps;
	
	if (useAsync) {
		// Try to get staging buffer from pool
		BufferData* stagingBuffer = g_ios_staging_pool.acquireBuffer(srcSize);
		if (!stagingBuffer) {
			// Pool exhausted, fallback to sync upload
			useAsync = false;
		} else {
			// Async upload path
			void* mappedData = stagingBuffer->MapMemory();
			if (mappedData) {
				memcpy(mappedData, srcData, srcSize);
				stagingBuffer->UnmapMemory();
				
				// Create fence for async tracking
				vk::Device device = VulkanContext::Instance()->GetDevice();
				vk::Fence fence = device.createFence(vk::FenceCreateInfo());
				
				// Submit copy command
				vk::BufferImageCopy copyRegion(0, 0, 0,
					vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
					vk::Offset3D(0, 0, 0),
					                                        vk::Extent3D(texture->getSize(), 1));
				
				commandBuffer.copyBufferToImage(*stagingBuffer->buffer, 
					texture->GetImage(), vk::ImageLayout::eTransferDstOptimal, copyRegion);
				
				// Track async upload
				{
					std::lock_guard<std::mutex> lock(g_async_mutex);
					IOSAsyncUpload upload;
					upload.fence = fence;
					upload.stagingBuffer = stagingBuffer;
					upload.startTime = startTime;
					upload.textureSize = srcSize;
					g_pending_uploads.push_back(upload);
				}
				
				g_ios_tex_metrics.async_uploads++;
				return true;
			}
		}
	}
	
	// Sync upload path (fallback or small textures)
	return false;  // Let regular path handle it
}

/// Cleanup iOS texture streaming resources
static void CleanupIOSTextureStreaming() {
	// Process any remaining async uploads
	ProcessIOSAsyncUploads();
	
	// Cleanup pools
	g_ios_texture_pool.cleanup();
	
	// Log final performance stats
	g_ios_tex_metrics.logPerformanceStats();
}

#endif

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
void optimized_texture_upload(void* dst, const void* src, int width, int height, int stride)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	for (int y = 0; y < height; y++)
	{
		const uint8_t *src_line = s + y * stride;
		uint8_t *dst_line = d + y * width * 4;

		// Process 4 pixels (16 bytes) at a time
		for (int x = 0; x < width; x += 4)
		{
			if (x + 4 <= width)
			{
				uint8x16_t pixels = vld1q_u8(src_line + x * 4);
				vst1q_u8(dst_line + x * 4, pixels);
			}
			else
			{
				// Handle remaining pixels
				for (int i = 0; i < width - x; i++)
				{
					dst_line[x * 4 + i * 4 + 0] = src_line[x * 4 + i * 4 + 0];
					dst_line[x * 4 + i * 4 + 1] = src_line[x * 4 + i * 4 + 1];
					dst_line[x * 4 + i * 4 + 2] = src_line[x * 4 + i * 4 + 2];
					dst_line[x * 4 + i * 4 + 3] = src_line[x * 4 + i * 4 + 3];
				}
			}
		}
	}
}
#endif

void setImageLayout(vk::CommandBuffer const& commandBuffer, vk::Image image, vk::Format format, u32 mipmapLevels, vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout)
{
	static const float scopeColor[4] = { 0.75f, 0.75f, 0.0f, 1.0f };
	CommandBufferDebugScope _(commandBuffer, "setImageLayout", scopeColor);

	vk::AccessFlags sourceAccessMask;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eTransferDstOptimal:
		sourceAccessMask = vk::AccessFlagBits::eTransferWrite;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		sourceAccessMask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::ePreinitialized:
		sourceAccessMask = vk::AccessFlagBits::eHostWrite;
		break;
	case vk::ImageLayout::eGeneral:     // sourceAccessMask is empty
	case vk::ImageLayout::eUndefined:
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		sourceAccessMask = vk::AccessFlagBits::eShaderRead;
		break;
	default:
		verify(false);
		break;
	}

	vk::PipelineStageFlags sourceStage;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eGeneral:
	case vk::ImageLayout::ePreinitialized:
		sourceStage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		sourceStage = vk::PipelineStageFlagBits::eTransfer;
		break;
	case vk::ImageLayout::eUndefined:
		sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		sourceStage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	default:
		verify(false);
		break;
	}

	vk::AccessFlags destinationAccessMask;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		destinationAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destinationAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		break;
	case vk::ImageLayout::eGeneral:   // empty destinationAccessMask
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destinationAccessMask = vk::AccessFlagBits::eShaderRead;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		destinationAccessMask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
		destinationAccessMask = vk::AccessFlagBits::eTransferWrite;
		break;
	case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		destinationAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead;
		break;
	default:
		verify(false);
		break;
	}

	vk::PipelineStageFlags destinationStage;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		destinationStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
		break;
	case vk::ImageLayout::eGeneral:
		destinationStage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		destinationStage = vk::PipelineStageFlagBits::eTransfer;
		break;
	case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
		break;
	default:
		verify(false);
		break;
	}

	vk::ImageAspectFlags aspectMask;
	if (newImageLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal || newImageLayout == vk::ImageLayout::eDepthStencilReadOnlyOptimal)
	{
		aspectMask = vk::ImageAspectFlagBits::eDepth;
		if (format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD16UnormS8Uint)
		{
			aspectMask |= vk::ImageAspectFlagBits::eStencil;
		}
	}
	else
	{
		aspectMask = vk::ImageAspectFlagBits::eColor;
	}

	vk::ImageSubresourceRange imageSubresourceRange(aspectMask, 0, mipmapLevels, 0, 1);
	vk::ImageMemoryBarrier imageMemoryBarrier(sourceAccessMask, destinationAccessMask, oldImageLayout, newImageLayout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, imageSubresourceRange);
	commandBuffer.pipelineBarrier(sourceStage, destinationStage, {}, nullptr, nullptr, imageMemoryBarrier);
}

void Texture::UploadToGPU(int width, int height, const u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	vk::Format format = vk::Format::eUndefined;
	u32 dataSize = width * height * 2;
	switch (tex_type)
	{
	case TextureType::_5551:
		format = vk::Format::eR5G5B5A1UnormPack16;
		break;
	case TextureType::_565:
		format = vk::Format::eR5G6B5UnormPack16;
		break;
	case TextureType::_4444:
		format = vk::Format::eR4G4B4A4UnormPack16;
		break;
	case TextureType::_8888:
		format = vk::Format::eR8G8B8A8Unorm;
		dataSize *= 2;
		break;
	case TextureType::_8:
		format = vk::Format::eR8Unorm;
		dataSize /= 2;
		break;
	}
	if (mipmapsIncluded)
	{
		int w = width / 2;
		u32 size = dataSize / 4;
		while (w)
		{
			dataSize += ((size + 3) >> 2) << 2;		// offset must be a multiple of 4
			size /= 4;
			w /= 2;
		}
	}
	bool isNew = true;
	if (width != (int)extent.width || height != (int)extent.height
			|| format != this->format || !this->image)
		Init(width, height, format, dataSize, mipmapped, mipmapsIncluded);
	else
		isNew = false;
	SetImage(dataSize, data, isNew, mipmapped && !mipmapsIncluded);
}

void Texture::Init(u32 width, u32 height, vk::Format format, u32 dataSize, bool mipmapped, bool mipmapsIncluded)
{
	this->extent = vk::Extent2D(width, height);
	this->format = format;
	mipmapLevels = 1;
	if (mipmapped)
		mipmapLevels += floor(log2(std::max(width, height)));

#if defined(__APPLE__) && defined(TARGET_IPHONE)
	// Try to acquire texture from iOS pool for FMV performance
	auto* pooledTexture = g_ios_texture_pool.acquireTexture(extent, format, mipmapLevels);
	if (pooledTexture && pooledTexture->image) {
		// Reuse existing pooled texture
		image = std::move(pooledTexture->image);
		imageView = std::move(pooledTexture->imageView);
		allocation = std::move(pooledTexture->allocation);
		
		// Configure for reuse
		needsStaging = true;  // Most pooled textures use staging
		if (!stagingBufferData) {
			                        stagingBufferData.reset(g_ios_staging_pool.acquireBuffer(dataSize));
			if (!stagingBufferData) {
				stagingBufferData = std::make_unique<BufferData>(dataSize, vk::BufferUsageFlagBits::eTransferSrc);
			}
		}
		return;
	}
	// Fall through to regular allocation if pool miss or needs recreation
#endif

	vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(format);

	vk::ImageTiling imageTiling = (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
			== vk::FormatFeatureFlagBits::eSampledImage
			? vk::ImageTiling::eOptimal
			: vk::ImageTiling::eLinear;
// Check if we can use linear tiling for small textures (performance improvement)
#ifdef __APPLE__
	// MoltenVK 1.2.11+ has improved texture handling
	static bool checkedMoltenVKVersion = false;
	static bool canUseLinearTiling = false;
	if (!checkedMoltenVKVersion) {
		// For MoltenVK 1.2.11+, we can use linear tiling for small textures
		// This is a performance improvement but was causing corruption in older versions
		// Since we're targeting MoltenVK 1.2.11+, enable this optimization
		canUseLinearTiling = true;
		checkedMoltenVKVersion = true;
	}
	
	if (canUseLinearTiling && height <= 32
			&& dataSize / height <= 64
			&& !mipmapped
			&& (formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) == vk::FormatFeatureFlagBits::eSampledImage)
		imageTiling = vk::ImageTiling::eLinear;
#else
	// Performance improvement on other platforms
	if (height <= 32
			&& dataSize / height <= 64
			&& !mipmapped
			&& (formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) == vk::FormatFeatureFlagBits::eSampledImage)
		imageTiling = vk::ImageTiling::eLinear;
#endif
	needsStaging = imageTiling != vk::ImageTiling::eLinear;
	vk::ImageLayout initialLayout;
	vk::ImageUsageFlags usageFlags = vk::ImageUsageFlagBits::eSampled;
	if (needsStaging)
	{
		stagingBufferData = std::make_unique<BufferData>(dataSize, vk::BufferUsageFlagBits::eTransferSrc);
		usageFlags |= vk::ImageUsageFlagBits::eTransferDst;
		initialLayout = vk::ImageLayout::eUndefined;
	}
	else
	{
		verify((formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) == vk::FormatFeatureFlagBits::eSampledImage);
		initialLayout = vk::ImageLayout::ePreinitialized;
	}
	if (mipmapped && !mipmapsIncluded)
		usageFlags |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	CreateImage(imageTiling, usageFlags, initialLayout, vk::ImageAspectFlagBits::eColor);
}

void Texture::CreateImage(vk::ImageTiling tiling, vk::ImageUsageFlags usage, vk::ImageLayout initialLayout,
		vk::ImageAspectFlags aspectMask)
{
	this->usageFlags = usage;
	vk::ImageCreateInfo imageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, format, vk::Extent3D(extent, 1), mipmapLevels, 1,
										vk::SampleCountFlagBits::e1, tiling, usage,
										vk::SharingMode::eExclusive, nullptr, initialLayout);
	image = device.createImageUnique(imageCreateInfo);

	VmaAllocationCreateInfo allocCreateInfo = { VmaAllocationCreateFlags(), needsStaging ? VmaMemoryUsage::VMA_MEMORY_USAGE_GPU_ONLY : VmaMemoryUsage::VMA_MEMORY_USAGE_CPU_TO_GPU };
#ifndef __APPLE__
	if (!needsStaging)
		allocCreateInfo.flags = VmaAllocationCreateFlagBits::VMA_ALLOCATION_CREATE_MAPPED_BIT;
#endif
	allocation = VulkanContext::Instance()->GetAllocator().AllocateForImage(*image, allocCreateInfo);

	vk::ImageViewCreateInfo imageViewCreateInfo(vk::ImageViewCreateFlags(), image.get(), vk::ImageViewType::e2D, format, vk::ComponentMapping(),
			vk::ImageSubresourceRange(aspectMask, 0, mipmapLevels, 0, 1));
	imageView = device.createImageViewUnique(imageViewCreateInfo);
#ifdef VK_DEBUG
	char name[128];
	sprintf(name, "texture @ %x", startAddress);
	VulkanContext::Instance()->setObjectName(image.get(), name);
	VulkanContext::Instance()->setObjectName(imageView.get(), name);
#endif
}

void Texture::SetImage(u32 srcSize, const void *srcData, bool isNew, bool genMipmaps)
{
	verify((bool)commandBuffer);

	static const float scopeColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
	CommandBufferDebugScope _(commandBuffer, "SetImage", scopeColor);

#if defined(__APPLE__) && defined(TARGET_IPHONE)
	// Process any completed async uploads first
	ProcessIOSAsyncUploads();
	
	// Try iOS optimized texture upload for FMV performance
	if (UploadTextureIOSOptimized(this, srcSize, srcData, isNew, genMipmaps, commandBuffer)) {
		// iOS async upload initiated, return early
		if (!isNew && !needsStaging)
			setImageLayout(commandBuffer, image.get(), format, mipmapLevels, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
		return;
	}
	// Fall through to regular upload if iOS optimization not used
#endif

	if (!isNew && !needsStaging)
		setImageLayout(commandBuffer, image.get(), format, mipmapLevels, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral);

	void* data;
	if (needsStaging)
	{
		if (!stagingBufferData)
			// This can happen if a texture is first created for RTT, then later updated
			stagingBufferData = std::make_unique<BufferData>(srcSize, vk::BufferUsageFlagBits::eTransferSrc);
		data = stagingBufferData->MapMemory();
	}
	else
		data = allocation.MapMemory();
	verify(data != nullptr);

	if (mipmapLevels > 1 && !genMipmaps && tex_type != TextureType::_8888)
	{
		// Each mipmap level must start at a 4-byte boundary
		u8 *src = (u8 *)srcData;
		u8 *dst = (u8 *)data;
		for (u32 i = 0; i < mipmapLevels; i++)
		{
			const u32 size = (1 << (2 * i)) * 2;
			memcpy(dst, src, size);
			dst += ((size + 3) >> 2) << 2;
			src += size;
		}
	}
	else if (!needsStaging)
	{
		vk::SubresourceLayout layout = device.getImageSubresourceLayout(*image, vk::ImageSubresource(vk::ImageAspectFlagBits::eColor));
		if (layout.size != srcSize)
		{
			u8 *src = (u8 *)srcData;
			u8 *dst = (u8 *)data;
			u32 srcSz = extent.width * 2;
			if (tex_type == TextureType::_8888)
				srcSz *= 2;
			else if (tex_type == TextureType::_8)
				srcSz /= 2;
			u8 * const srcEnd = src + srcSz * extent.height;
			for (; src < srcEnd; src += srcSz, dst += layout.rowPitch)
				memcpy(dst, src, srcSz);
		}
		else
			memcpy(data, srcData, srcSize);
		allocation.UnmapMemory();
	}
	else
		memcpy(data, srcData, srcSize);

	if (needsStaging)
	{
		stagingBufferData->UnmapMemory();
		// Since we're going to blit to the texture image, set its layout to eTransferDstOptimal
		setImageLayout(commandBuffer, image.get(), format, mipmapLevels, isNew ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
				vk::ImageLayout::eTransferDstOptimal);

		if (mipmapLevels > 1 && !genMipmaps)
		{
			vk::DeviceSize bufferOffset = 0;
			for (u32 i = 0; i < mipmapLevels; i++)
			{
				vk::BufferImageCopy copyRegion(bufferOffset, 1 << i, 1 << i, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, mipmapLevels - i - 1, 0, 1),
						vk::Offset3D(0, 0, 0), vk::Extent3D(1 << i, 1 << i, 1));
				commandBuffer.copyBufferToImage(stagingBufferData->buffer.get(), image.get(), vk::ImageLayout::eTransferDstOptimal, copyRegion);
				const u32 size = (1 << (2 * i)) * (tex_type == TextureType::_8888 ? 4 : 2);
				bufferOffset += ((size + 3) >> 2) << 2;
			}
		}
		else
		{
			vk::BufferImageCopy copyRegion(0, extent.width, extent.height, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
					vk::Offset3D(0, 0, 0), vk::Extent3D(extent, 1));
			commandBuffer.copyBufferToImage(stagingBufferData->buffer.get(), image.get(), vk::ImageLayout::eTransferDstOptimal, copyRegion);
			if (mipmapLevels > 1)
				GenerateMipmaps();
		}
		// Set the layout for the texture image from eTransferDstOptimal to SHADER_READ_ONLY
		setImageLayout(commandBuffer, image.get(), format, mipmapLevels, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	}
	else
	{
		if (mipmapLevels > 1)
			GenerateMipmaps();
		else
			// If we can use the linear tiled image as a texture, just do it
			setImageLayout(commandBuffer, image.get(), format, mipmapLevels, isNew ? vk::ImageLayout::ePreinitialized : vk::ImageLayout::eGeneral,
					vk::ImageLayout::eShaderReadOnlyOptimal);
	}
}

void Texture::GenerateMipmaps()
{
	static const float scopeColor[4] = { 0.75f, 0.75f, 0.0f, 1.0f };
	CommandBufferDebugScope _(commandBuffer, "GenerateMipmaps", scopeColor);

	u32 mipWidth = extent.width;
	u32 mipHeight = extent.height;
	vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			*image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

	for (u32 i = 1; i < mipmapLevels; i++)
	{
		// Transition previous mipmap level from dst optimal/preinit to src optimal
		barrier.subresourceRange.baseMipLevel = i - 1;
		if (i == 1 && !needsStaging)
		{
			barrier.oldLayout = vk::ImageLayout::ePreinitialized;
			barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
		}
		else
		{
			barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
			barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		}
		barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, barrier);

		// Blit previous mipmap level on current
		vk::ImageBlit blit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i - 1, 0, 1),
				 { { vk::Offset3D(0, 0, 0), vk::Offset3D(mipWidth, mipHeight, 1) } },
				 vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i, 0, 1),
				 { { vk::Offset3D(0, 0, 0), vk::Offset3D(std::max(mipWidth / 2, 1u), std::max(mipHeight / 2, 1u), 1) } });
		commandBuffer.blitImage(*image, vk::ImageLayout::eTransferSrcOptimal, *image, vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

		// Transition previous mipmap level from src optimal to shader read-only optimal
		barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
		barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, barrier);

		mipWidth = std::max(mipWidth / 2, 1u);
		mipHeight = std::max(mipHeight / 2, 1u);
	}
	// Transition last mipmap level from dst optimal to shader read-only optimal
	barrier.subresourceRange.baseMipLevel = mipmapLevels - 1;
	barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, barrier);
}

void Texture::deferDeleteResource(FlightManager *manager)
{
	class ResourceDeleter : public Deletable
	{
	public:
		ResourceDeleter(Texture *texture)
		{
			std::swap(image, texture->image);
			std::swap(imageView, texture->imageView);
			std::swap(bufferData, texture->stagingBufferData);
			std::swap(allocation, texture->allocation);
		}

	private:
		vk::UniqueImage image;
		vk::UniqueImageView imageView;
		std::unique_ptr<BufferData> bufferData;
		Allocation allocation;
	};
	manager->addToFlight(new ResourceDeleter(this));
}

void FramebufferAttachment::Init(u32 width, u32 height, vk::Format format, const vk::ImageUsageFlags& usage, const std::string& name)
{
	this->format = format;
	this->extent = vk::Extent2D { width, height };
	bool depth = format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD16UnormS8Uint;

	if (usage & vk::ImageUsageFlagBits::eTransferSrc)
	{
		stagingBufferData = std::make_unique<BufferData>(width * height * 4,
				vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached | vk::MemoryPropertyFlagBits::eHostCoherent);
	}
	vk::ImageCreateInfo imageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, format, vk::Extent3D(extent, 1), 1, 1, vk::SampleCountFlagBits::e1,
			vk::ImageTiling::eOptimal, usage,
			vk::SharingMode::eExclusive, nullptr, vk::ImageLayout::eUndefined);
	image = device.createImageUnique(imageCreateInfo);
#ifdef VK_DEBUG
	if (!name.empty())
		VulkanContext::Instance()->setObjectName(image.get(), name);
#endif

	VmaAllocationCreateInfo allocCreateInfo = { VmaAllocationCreateFlags(), VmaMemoryUsage::VMA_MEMORY_USAGE_GPU_ONLY };
	if (usage & vk::ImageUsageFlagBits::eTransientAttachment)
		allocCreateInfo.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
	allocation = VulkanContext::Instance()->GetAllocator().AllocateForImage(*image, allocCreateInfo);

	if ((usage & vk::ImageUsageFlagBits::eColorAttachment) || (usage & vk::ImageUsageFlagBits::eDepthStencilAttachment))
	{
		vk::ImageViewCreateInfo imageViewCreateInfo(vk::ImageViewCreateFlags(), image.get(), vk::ImageViewType::e2D,
				format, vk::ComponentMapping(),	vk::ImageSubresourceRange(depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
		imageView = device.createImageViewUnique(imageViewCreateInfo);
#ifdef VK_DEBUG
		if (!name.empty())
			VulkanContext::Instance()->setObjectName(imageView.get(), name);
#endif

		if ((usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) && (usage & vk::ImageUsageFlagBits::eInputAttachment))
		{
			// Also create an imageView for the stencil
			imageViewCreateInfo.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1);
			stencilView = device.createImageViewUnique(imageViewCreateInfo);
#ifdef VK_DEBUG
			if (!name.empty())
				VulkanContext::Instance()->setObjectName(stencilView.get(), name);
#endif
		}
	}
}

void TextureCache::Cleanup()
{
	std::vector<u64> list;

	u32 TargetFrame = std::max((u32)120, FrameCount) - 120;

	for (const auto& [id, texture] : cache)
	{
		if (texture.dirty && texture.dirty < TargetFrame)
			list.push_back(id);

		if (list.size() > 5)
			break;
	}

	for (u64 id : list)
	{
		if (clearTexture(&cache[id]))
			cache.erase(id);
	}
}
