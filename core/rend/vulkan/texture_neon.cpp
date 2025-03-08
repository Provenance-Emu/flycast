#include "texture.h"
#include <algorithm>

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

// Optimized bilinear texture sampling using NEON
void BilinearSampleNeon(const u8* texture, int width, int height, float u, float v, u32* color)
{
    // Calculate texture coordinates
    float fx = u * width - 0.5f;
    float fy = v * height - 0.5f;

    int x = (int)fx;
    int y = (int)fy;

    // Calculate fractional parts
    float fracX = fx - x;
    float fracY = fy - y;

    // Clamp coordinates
    x = std::max(0, std::min(width - 1, x));
    y = std::max(0, std::min(height - 1, y));

    // Calculate next coordinates with wrapping
    int x1 = (x + 1) % width;
    int y1 = (y + 1) % height;

    // Load 4 texels (RGBA)
    uint8_t texels[16];

    // Copy the 4 texels into our array
    memcpy(&texels[0],  &texture[(y * width + x) * 4],   4);
    memcpy(&texels[4],  &texture[(y * width + x1) * 4],  4);
    memcpy(&texels[8],  &texture[(y1 * width + x) * 4],  4);
    memcpy(&texels[12], &texture[(y1 * width + x1) * 4], 4);

    // Load texels into NEON registers
    uint8x16_t texels_neon = vld1q_u8(texels);

    // Convert to 16-bit for interpolation
    uint16x8_t texels_low = vmovl_u8(vget_low_u8(texels_neon));
    uint16x8_t texels_high = vmovl_u8(vget_high_u8(texels_neon));

    // Calculate weights
    float weights[4] = {
        (1.0f - fracX) * (1.0f - fracY),
        fracX * (1.0f - fracY),
        (1.0f - fracX) * fracY,
        fracX * fracY
    };

    // Load weights into NEON register
    float32x4_t weights_neon = vld1q_f32(weights);

    // Manual extraction of texel components and interpolation
    // We need to handle each channel separately with constant indices

    // Red channel (0)
    {
        float32x4_t channel = {
            (float)vgetq_lane_u16(texels_low, 0),   // Top-left, red
            (float)vgetq_lane_u16(texels_low, 4),   // Top-right, red
            (float)vgetq_lane_u16(texels_high, 0),  // Bottom-left, red
            (float)vgetq_lane_u16(texels_high, 4)   // Bottom-right, red
        };

        float32x4_t weighted = vmulq_f32(channel, weights_neon);
        float sum = vgetq_lane_f32(weighted, 0) + vgetq_lane_f32(weighted, 1) +
                    vgetq_lane_f32(weighted, 2) + vgetq_lane_f32(weighted, 3);

        ((u8*)color)[0] = (u8)sum;
    }

    // Green channel (1)
    {
        float32x4_t channel = {
            (float)vgetq_lane_u16(texels_low, 1),   // Top-left, green
            (float)vgetq_lane_u16(texels_low, 5),   // Top-right, green
            (float)vgetq_lane_u16(texels_high, 1),  // Bottom-left, green
            (float)vgetq_lane_u16(texels_high, 5)   // Bottom-right, green
        };

        float32x4_t weighted = vmulq_f32(channel, weights_neon);
        float sum = vgetq_lane_f32(weighted, 0) + vgetq_lane_f32(weighted, 1) +
                    vgetq_lane_f32(weighted, 2) + vgetq_lane_f32(weighted, 3);

        ((u8*)color)[1] = (u8)sum;
    }

    // Blue channel (2)
    {
        float32x4_t channel = {
            (float)vgetq_lane_u16(texels_low, 2),   // Top-left, blue
            (float)vgetq_lane_u16(texels_low, 6),   // Top-right, blue
            (float)vgetq_lane_u16(texels_high, 2),  // Bottom-left, blue
            (float)vgetq_lane_u16(texels_high, 6)   // Bottom-right, blue
        };

        float32x4_t weighted = vmulq_f32(channel, weights_neon);
        float sum = vgetq_lane_f32(weighted, 0) + vgetq_lane_f32(weighted, 1) +
                    vgetq_lane_f32(weighted, 2) + vgetq_lane_f32(weighted, 3);

        ((u8*)color)[2] = (u8)sum;
    }

    // Alpha channel (3)
    {
        float32x4_t channel = {
            (float)vgetq_lane_u16(texels_low, 3),   // Top-left, alpha
            (float)vgetq_lane_u16(texels_low, 7),   // Top-right, alpha
            (float)vgetq_lane_u16(texels_high, 3),  // Bottom-left, alpha
            (float)vgetq_lane_u16(texels_high, 7)   // Bottom-right, alpha
        };

        float32x4_t weighted = vmulq_f32(channel, weights_neon);
        float sum = vgetq_lane_f32(weighted, 0) + vgetq_lane_f32(weighted, 1) +
                    vgetq_lane_f32(weighted, 2) + vgetq_lane_f32(weighted, 3);

        ((u8*)color)[3] = (u8)sum;
    }
}

// Define texture types
enum class TextureFormat {
    RGB565,
    ARGB1555,
    ARGB4444,
    RGBA8888
};

// Optimized texture conversion from various formats to RGBA8888 using NEON
void ConvertTextureNeon(const u8* src, u8* dst, int width, int height, TextureFormat srcFormat)
{
    int pixels = width * height;

    switch (srcFormat)
    {
        case TextureFormat::RGB565:
        {
            // Convert RGB565 to RGBA8888
            const u16* src16 = (const u16*)src;

            for (int i = 0; i < pixels; i += 8)
            {
                int remaining = std::min(8, pixels - i);

                if (remaining == 8)
                {
                    // Load 8 RGB565 pixels
                    uint16x8_t rgb = vld1q_u16(src16 + i);

                    // Extract R, G, B components
                    // R: (rgb >> 11) & 0x1F
                    uint8x8_t r = vmovn_u16(vshrq_n_u16(rgb, 11));
                    // G: (rgb >> 5) & 0x3F
                    uint8x8_t g = vmovn_u16(vshrq_n_u16(vshlq_n_u16(vshrq_n_u16(rgb, 5), 10), 10));
                    // B: rgb & 0x1F
                    uint8x8_t b = vmovn_u16(vshrq_n_u16(vshlq_n_u16(rgb, 11), 11));

                    // Scale to full range
                    r = vmul_u8(r, vdup_n_u8(255/31));
                    g = vmul_u8(g, vdup_n_u8(255/63));
                    b = vmul_u8(b, vdup_n_u8(255/31));

                    // Interleave components with alpha (255)
                    uint8x8x4_t rgba;
                    rgba.val[0] = r;
                    rgba.val[1] = g;
                    rgba.val[2] = b;
                    rgba.val[3] = vdup_n_u8(255);

                    // Store interleaved result
                    vst4_u8(dst + i * 4, rgba);
                }
                else
                {
                    // Handle remaining pixels
                    for (int j = 0; j < remaining; j++)
                    {
                        u16 rgb = src16[i + j];
                        u8 r = ((rgb >> 11) & 0x1F) * 255 / 31;
                        u8 g = ((rgb >> 5) & 0x3F) * 255 / 63;
                        u8 b = (rgb & 0x1F) * 255 / 31;
                        dst[(i + j) * 4 + 0] = r;
                        dst[(i + j) * 4 + 1] = g;
                        dst[(i + j) * 4 + 2] = b;
                        dst[(i + j) * 4 + 3] = 255;
                    }
                }
            }
            break;
        }

        case TextureFormat::ARGB1555:
        {
            // Convert ARGB1555 to RGBA8888
            const u16* src16 = (const u16*)src;

            for (int i = 0; i < pixels; i += 8)
            {
                int remaining = std::min(8, pixels - i);

                if (remaining == 8)
                {
                    // Load 8 ARGB1555 pixels
                    uint16x8_t argb = vld1q_u16(src16 + i);

                    // Extract A, R, G, B components
                    // A: (argb >> 15) & 0x01
                    uint8x8_t a = vmovn_u16(vshrq_n_u16(argb, 15));
                    // R: (argb >> 10) & 0x1F
                    uint8x8_t r = vmovn_u16(vshrq_n_u16(vshlq_n_u16(vshrq_n_u16(argb, 10), 11), 11));
                    // G: (argb >> 5) & 0x1F
                    uint8x8_t g = vmovn_u16(vshrq_n_u16(vshlq_n_u16(vshrq_n_u16(argb, 5), 11), 11));
                    // B: argb & 0x1F
                    uint8x8_t b = vmovn_u16(vshrq_n_u16(vshlq_n_u16(argb, 11), 11));

                    // Scale to full range
                    a = vmul_u8(a, vdup_n_u8(255));
                    r = vmul_u8(r, vdup_n_u8(255/31));
                    g = vmul_u8(g, vdup_n_u8(255/31));
                    b = vmul_u8(b, vdup_n_u8(255/31));

                    // Interleave components
                    uint8x8x4_t rgba;
                    rgba.val[0] = r;
                    rgba.val[1] = g;
                    rgba.val[2] = b;
                    rgba.val[3] = a;

                    // Store interleaved result
                    vst4_u8(dst + i * 4, rgba);
                }
                else
                {
                    // Handle remaining pixels
                    for (int j = 0; j < remaining; j++)
                    {
                        u16 argb = src16[i + j];
                        u8 a = ((argb >> 15) & 0x01) * 255;
                        u8 r = ((argb >> 10) & 0x1F) * 255 / 31;
                        u8 g = ((argb >> 5) & 0x1F) * 255 / 31;
                        u8 b = (argb & 0x1F) * 255 / 31;
                        dst[(i + j) * 4 + 0] = r;
                        dst[(i + j) * 4 + 1] = g;
                        dst[(i + j) * 4 + 2] = b;
                        dst[(i + j) * 4 + 3] = a;
                    }
                }
            }
            break;
        }

        case TextureFormat::ARGB4444:
        {
            // Convert ARGB4444 to RGBA8888
            const u16* src16 = (const u16*)src;

            for (int i = 0; i < pixels; i += 8)
            {
                int remaining = std::min(8, pixels - i);

                if (remaining == 8)
                {
                    // Load 8 ARGB4444 pixels
                    uint16x8_t argb = vld1q_u16(src16 + i);

                    // Extract A, R, G, B components (each 4 bits)
                    uint8x8_t a = vmovn_u16(vshrq_n_u16(argb, 12));
                    uint8x8_t r = vmovn_u16(vshrq_n_u16(vshlq_n_u16(vshrq_n_u16(argb, 8), 12), 12));
                    uint8x8_t g = vmovn_u16(vshrq_n_u16(vshlq_n_u16(vshrq_n_u16(argb, 4), 12), 12));
                    uint8x8_t b = vmovn_u16(vshrq_n_u16(vshlq_n_u16(argb, 12), 12));

                    // Scale to full range (4 bits to 8 bits)
                    a = vmul_u8(a, vdup_n_u8(255/15));
                    r = vmul_u8(r, vdup_n_u8(255/15));
                    g = vmul_u8(g, vdup_n_u8(255/15));
                    b = vmul_u8(b, vdup_n_u8(255/15));

                    // Interleave components
                    uint8x8x4_t rgba;
                    rgba.val[0] = r;
                    rgba.val[1] = g;
                    rgba.val[2] = b;
                    rgba.val[3] = a;

                    // Store interleaved result
                    vst4_u8(dst + i * 4, rgba);
                }
                else
                {
                    // Handle remaining pixels
                    for (int j = 0; j < remaining; j++)
                    {
                        u16 argb = src16[i + j];
                        u8 a = ((argb >> 12) & 0x0F) * 255 / 15;
                        u8 r = ((argb >> 8) & 0x0F) * 255 / 15;
                        u8 g = ((argb >> 4) & 0x0F) * 255 / 15;
                        u8 b = (argb & 0x0F) * 255 / 15;
                        dst[(i + j) * 4 + 0] = r;
                        dst[(i + j) * 4 + 1] = g;
                        dst[(i + j) * 4 + 2] = b;
                        dst[(i + j) * 4 + 3] = a;
                    }
                }
            }
            break;
        }

        case TextureFormat::RGBA8888:
        {
            // Direct copy for RGBA8888
            memcpy(dst, src, pixels * 4);
            break;
        }
    }
}
#endif
