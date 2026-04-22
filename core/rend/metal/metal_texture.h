/*
    Copyright 2025 flyinghead

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
#include "rend/TexCache.h"
#include "metal_context.h"
#include "metal.h"

#include <unordered_set>
#include <Metal/Metal.h>

class MetalTexture final : public BaseTextureCacheData
{
public:
    MetalTexture(TSP tsp = {}, TCW tcw = {}) : BaseTextureCacheData(tsp, tcw) {}

    std::string GetId() override {
        // gpuResourceID was added in iOS 16 / macOS 13 / tvOS 16. Older
        // SDKs don't declare the selector at all, so an @available check
        // alone still fails to compile against an iOS 15 / macOS 12 SDK
        // (the protocol method does not exist for the compiler to resolve).
        // Gate the call on the SDK first, then keep the runtime check for
        // older OS versions on a newer SDK build.
        // The fallback uses (__bridge void *) so the cast is well-defined
        // under both ARC and MRR (raw `reinterpret_cast` on an `id` trips
        // -Warc-bridge-casts-disallowed-in-nonarc and ARC errors in some
        // configurations); we only need a stable, unique-for-lifetime
        // value, which the bridged void* hash provides.
#if (defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 160000) \
    || (defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000) \
    || (defined(__TV_OS_VERSION_MAX_ALLOWED) && __TV_OS_VERSION_MAX_ALLOWED >= 160000)
        if (@available(iOS 16.0, macOS 13.0, tvOS 16.0, *))
            return std::to_string([texture gpuResourceID]._impl);
#endif
        return std::to_string(reinterpret_cast<uintptr_t>((__bridge void *)texture));
    }
    id<MTLTexture> GetTexture() const { return texture; }
    void UploadToGPU(int width, int height, const u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
    void SetCommandBuffer(id<MTLCommandBuffer> commandBuffer) { this->commandBuffer = commandBuffer; }
    void SetTexture(id<MTLTexture> texture, u32 width, u32 height) {
        this->texture = texture;
        this->width = width;
        this->height = height;
    }
    void SetInFlight(bool inFlight) {
        this->isInFlight = inFlight;
    }
    void deferDeleteResource(MetalFlightManager *manager);
    id<MTLTexture> GetReadOnlyTexture() const { return readOnlyTexture ? readOnlyTexture : texture; }
    void CreateReadOnlyCopy(id<MTLCommandBuffer> commandBuffer);

private:
    void Init(u32 width, u32 height, MTLPixelFormat format, u32 dataSize, bool mipmapped, bool mipmapsIncluded);
    void SetImage(u32 srcSize, const void *srcData, bool genMipmaps);
    void GenerateMipmaps();

    MTLPixelFormat format = MTLPixelFormatInvalid;
    u32 width = 0;
    u32 height = 0;
    u32 mipmapLevels = 1;
    id<MTLCommandBuffer> commandBuffer = nil;
    id<MTLTexture> texture = nil;
    id<MTLTexture> readOnlyTexture = nil;
    bool isInFlight = false;

    friend class MetalTextureCache;
};

class MetalSamplers
{
public:
    explicit MetalSamplers();
    ~MetalSamplers();

    static const u32 TSP_Mask = 0x7ef00;

    void term() {
        samplers.clear();
        fallbackSampler = nil;
        fallbackFailedLogged = false;
    }

    id<MTLSamplerState> GetSampler(const PolyParam& poly, bool punchThrough, bool texture1 = false) {
        TSP tsp = texture1 ? poly.tsp1 : poly.tsp;
        if (poly.texture != nullptr && poly.texture->gpuPalette)
            tsp.FilterMode = 0;
        else if (config::TextureFiltering == 1)
            tsp.FilterMode = 0;
        else if (config::TextureFiltering == 2)
            tsp.FilterMode = 1;
        return GetSampler(tsp, punchThrough);
    }

    id<MTLSamplerState> GetSampler(TSP tsp, bool punchThrough = false) {
        const u32 hash = (tsp.full & TSP_Mask) | punchThrough;	// MipMapD, FilterMode, ClampU, ClampV, FlipU, FlipV
        id<MTLSamplerState> sampler = samplers[hash];

        if (!sampler) {
            auto desc = [[MTLSamplerDescriptor alloc] init];

            if (tsp.FilterMode != 0) {
                if (punchThrough) {
                    [desc setMinFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMagFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMipFilter:MTLSamplerMipFilterNearest];
                } else {
                    [desc setMinFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMagFilter:MTLSamplerMinMagFilterLinear];
                    [desc setMipFilter:MTLSamplerMipFilterLinear];
                }
            }
            else {
                [desc setMinFilter:MTLSamplerMinMagFilterNearest];
                [desc setMagFilter:MTLSamplerMinMagFilterNearest];
                [desc setMipFilter:MTLSamplerMipFilterNearest];
            }

            auto sRepeat = tsp.ClampU ? MTLSamplerAddressModeClampToEdge : tsp.FlipU ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
            auto tRepeat = tsp.ClampV ? MTLSamplerAddressModeClampToEdge : tsp.FlipV ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;

            [desc setSAddressMode:sRepeat];
            [desc setTAddressMode:tRepeat];
            [desc setRAddressMode:tRepeat];
            [desc setCompareFunction:MTLCompareFunctionNever];
            if (tsp.FilterMode == 1 && !punchThrough) {
                // Metal requires maxAnisotropy in [1, 16]. The user-facing
                // option is Option<int>, so clamp in *signed* space first:
                // a negative value would underflow when converted to u32
                // and incorrectly pin to 16 instead of 1.
                NSUInteger anisotropy = static_cast<NSUInteger>(
                    std::clamp<int>(config::AnisotropicFiltering, 1, 16));
                [desc setMaxAnisotropy:anisotropy];
            } else {
                [desc setMaxAnisotropy:1];
            }

            sampler = [MetalContext::Instance()->GetDevice() newSamplerStateWithDescriptor:desc];
            if (sampler == nil) {
                // newSamplerStateWithDescriptor can return nil if the
                // device can't satisfy the descriptor (e.g. anisotropy
                // on a software renderer, OOM, or unsupported combos).
                // Fall back to a single shared minimal-nearest sampler
                // so callers never receive nil and Metal validation
                // doesn't crash deep inside the encoder.
                sampler = GetOrCreateFallbackSampler();
            }
            // Only cache real samplers. Caching nil here would mean every
            // subsequent lookup for this hash short-circuits with nil and
            // bypasses the fallback path above.
            if (sampler != nil)
                samplers[hash] = sampler;
        }

        return sampler;
    }

private:
    id<MTLSamplerState> GetOrCreateFallbackSampler() {
        if (fallbackSampler != nil)
            return fallbackSampler;

        auto desc = [[MTLSamplerDescriptor alloc] init];
        [desc setMinFilter:MTLSamplerMinMagFilterNearest];
        [desc setMagFilter:MTLSamplerMinMagFilterNearest];
        [desc setMipFilter:MTLSamplerMipFilterNearest];
        fallbackSampler = [MetalContext::Instance()->GetDevice() newSamplerStateWithDescriptor:desc];

        if (fallbackSampler == nil) {
            // Log once. If the device can't produce even a default nearest
            // sampler something is very wrong, but spamming every frame
            // doesn't help diagnose it.
            if (!fallbackFailedLogged) {
                ERROR_LOG(RENDERER, "Sampler creation failed and fallback sampler is unavailable");
                fallbackFailedLogged = true;
            }
        } else if (!fallbackLogged) {
            ERROR_LOG(RENDERER, "Sampler creation failed; using shared nearest fallback");
            fallbackLogged = true;
        }

        return fallbackSampler;
    }

    std::unordered_map<u32, id<MTLSamplerState>> samplers;
    id<MTLSamplerState> fallbackSampler = nil;
    bool fallbackLogged = false;
    bool fallbackFailedLogged = false;
};

class MetalTextureCache final : public BaseTextureCache<MetalTexture>
{
public:
    MetalTextureCache() {}

    void SetCurrentIndex(int index)
    {
        if (index == (int)currentIndex)
            return;
        if (currentIndex < inFlightTextures.size())
            std::for_each(inFlightTextures[currentIndex].begin(), inFlightTextures[currentIndex].end(),
                          [](MetalTexture *texture) {
                texture->SetInFlight(false);
                texture->readOnlyTexture = nil;
            });
        currentIndex = index;
        EmptyTrash(inFlightTextures);
    }

    bool IsInFlight(MetalTexture *texture, bool previous)
    {
        for (u32 i = 0; i < inFlightTextures.size(); i++)
            if ((!previous || i != currentIndex)
                && inFlightTextures[i].find(texture) != inFlightTextures[i].end())
                return true;
        return false;
    }

    void SetInFlight(MetalTexture *texture)
    {
        texture->SetInFlight(true);
        inFlightTextures[currentIndex].insert(texture);
    }

    void Cleanup();

    void Clear()
    {
        for (auto& set : inFlightTextures)
        {
            for (MetalTexture *tex : set)
                tex->SetInFlight(false);
            set.clear();
        }
        BaseTextureCache::Clear();
    }

private:
    bool clearTexture(MetalTexture *tex)
    {
        for (auto& set : inFlightTextures)
            set.erase(tex);

        return tex->Delete();
    }

    template<typename T>
    void EmptyTrash(T& v)
    {
        if (v.size() < currentIndex + 1)
            v.resize(currentIndex + 1);
        else
            v[currentIndex].clear();
    }

    std::vector<std::unordered_set<MetalTexture *>> inFlightTextures;
    u32 currentIndex = ~0;
};