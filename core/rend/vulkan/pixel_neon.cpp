#include "types.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

// Optimized pixel blending using NEON
void BlendPixelsNeon(u32* dst, const u32* src, int count, float alpha)
{
    uint16x8_t alpha_u16 = vdupq_n_u16((u16)(alpha * 256));

    for (int i = 0; i < count; i += 4)
    {
        int remaining = std::min(4, count - i);

        if (remaining == 4)
        {
            // Load 4 pixels from source and destination
            uint8x16_t src_pixels = vld1q_u8((const u8*)(src + i));
            uint8x16_t dst_pixels = vld1q_u8((const u8*)(dst + i));

            // Split into 16-bit values for higher precision math
            uint16x8_t src_low = vmovl_u8(vget_low_u8(src_pixels));
            uint16x8_t src_high = vmovl_u8(vget_high_u8(src_pixels));
            uint16x8_t dst_low = vmovl_u8(vget_low_u8(dst_pixels));
            uint16x8_t dst_high = vmovl_u8(vget_high_u8(dst_pixels));

            // Blend: dst = src * alpha + dst * (1 - alpha)
            uint16x8_t inv_alpha = vsubq_u16(vdupq_n_u16(256), alpha_u16);

            uint16x8_t result_low = vshrq_n_u16(
                vaddq_u16(
                    vmulq_u16(src_low, alpha_u16),
                    vmulq_u16(dst_low, inv_alpha)
                ),
                8
            );

            uint16x8_t result_high = vshrq_n_u16(
                vaddq_u16(
                    vmulq_u16(src_high, alpha_u16),
                    vmulq_u16(dst_high, inv_alpha)
                ),
                8
            );

            // Combine and store result
            uint8x16_t result = vcombine_u8(
                vmovn_u16(result_low),
                vmovn_u16(result_high)
            );

            vst1q_u8((u8*)(dst + i), result);
        }
        else
        {
            // Handle remaining pixels
            for (int j = 0; j < remaining; j++)
            {
                u8* s = (u8*)(src + i + j);
                u8* d = (u8*)(dst + i + j);

                for (int k = 0; k < 4; k++)
                {
                    d[k] = (s[k] * alpha + d[k] * (1 - alpha));
                }
            }
        }
    }
}

// Optimized pixel format conversion using NEON
void ConvertPixelFormatNeon(const u8* src, u8* dst, int width, int height, int src_format, int dst_format)
{
    int pixels = width * height;

    // Example: Convert RGB565 to RGBA8888
    if (src_format == 0 && dst_format == 1)
    {
        const u16* src16 = (const u16*)src;
        u32* dst32 = (u32*)dst;

        for (int i = 0; i < pixels; i += 8)
        {
            int remaining = std::min(8, pixels - i);

            if (remaining == 8)
            {
                // Load 8 RGB565 pixels
                uint16x8_t rgb = vld1q_u16(src16 + i);

                // Extract R, G, B components and expand to 8 bits
                // Use a different approach to avoid vshrn_n_u16 with values > 8

                // R: (rgb >> 11) & 0x1F
                uint16x8_t r16 = vshrq_n_u16(rgb, 11);
                uint8x8_t r = vmovn_u16(r16);

                // G: (rgb >> 5) & 0x3F
                uint16x8_t g16 = vshrq_n_u16(rgb, 5);
                g16 = vandq_u16(g16, vdupq_n_u16(0x3F));
                uint8x8_t g = vmovn_u16(g16);

                // B: rgb & 0x1F
                uint16x8_t b16 = vandq_u16(rgb, vdupq_n_u16(0x1F));
                uint8x8_t b = vmovn_u16(b16);

                // Scale to full range
                r = vmul_u8(r, vdup_n_u8(255/31));
                g = vmul_u8(g, vdup_n_u8(255/63));
                b = vmul_u8(b, vdup_n_u8(255/31));

                // Interleave with alpha (255)
                uint8x8x4_t rgba;
                rgba.val[0] = r;
                rgba.val[1] = g;
                rgba.val[2] = b;
                rgba.val[3] = vdup_n_u8(255);

                // Store interleaved result
                vst4_u8((u8*)(dst32 + i), rgba);
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
                    dst32[i + j] = (255 << 24) | (b << 16) | (g << 8) | r;
                }
            }
        }
    }

    // Add more format conversions as needed
}
#endif
