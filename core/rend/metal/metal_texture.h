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
        // gpuResourceID was added in iOS 16 / macOS 13. Fall back to the
        // Objective-C object pointer for older deployment targets so the
        // backend still builds and runs against iOS 15 / macOS 12 SDKs.
        if (@available(iOS 16.0, macOS 13.0, tvOS 16.0, *))
            return std::to_string([texture gpuResourceID]._impl);
        else
            return std::to_string(reinterpret_cast<uintptr_t>(texture));
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
                // option goes higher than 16 in some setups; clamp instead
                // of letting Metal raise NSInvalidArgumentException.
                NSUInteger anisotropy = std::clamp<u32>(config::AnisotropicFiltering, 1, 16);
                [desc setMaxAnisotropy:anisotropy];
            } else {
                [desc setMaxAnisotropy:1];
            }

            sampler = [MetalContext::Instance()->GetDevice() newSamplerStateWithDescriptor:desc];
            if (sampler == nil) {
                // newSamplerStateWithDescriptor can return nil if the
                // device can't satisfy the descriptor (e.g. anisotropy
                // on a software renderer). Fall back to a minimal nearest
                // sampler so callers don't propagate nil into the encoder.
                ERROR_LOG(RENDERER, "Sampler creation failed; using fallback");
                auto fallback = [[MTLSamplerDescriptor alloc] init];
                [fallback setMinFilter:MTLSamplerMinMagFilterNearest];
                [fallback setMagFilter:MTLSamplerMinMagFilterNearest];
                [fallback setMipFilter:MTLSamplerMipFilterNearest];
                sampler = [MetalContext::Instance()->GetDevice() newSamplerStateWithDescriptor:fallback];
            }
            samplers[hash] = sampler;
        }

        return sampler;
    }

private:
    std::unordered_map<u32, id<MTLSamplerState>> samplers;
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