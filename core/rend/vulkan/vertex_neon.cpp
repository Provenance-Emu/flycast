#include "vertex.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

// Optimized vertex transformation using NEON
void TransformVerticesNeon(const Vertex* src, Vertex* dst, int count, const glm::mat4& mvp)
{
    // Extract matrix components for NEON processing
    float32x4_t row0 = vld1q_f32(&mvp[0][0]);
    float32x4_t row1 = vld1q_f32(&mvp[1][0]);
    float32x4_t row2 = vld1q_f32(&mvp[2][0]);
    float32x4_t row3 = vld1q_f32(&mvp[3][0]);

    for (int i = 0; i < count; i++)
    {
        // Load vertex position
        float32x4_t pos = {src[i].x, src[i].y, src[i].z, 1.0f};

        // Transform position with matrix
        float32x4_t result;
        result = vmulq_lane_f32(row0, vget_low_f32(pos), 0);
        result = vmlaq_lane_f32(result, row1, vget_low_f32(pos), 1);
        result = vmlaq_lane_f32(result, row2, vget_high_f32(pos), 0);
        result = vmlaq_lane_f32(result, row3, vget_high_f32(pos), 1);

        // Store transformed position
        dst[i].x = vgetq_lane_f32(result, 0);
        dst[i].y = vgetq_lane_f32(result, 1);
        dst[i].z = vgetq_lane_f32(result, 2);
        float w = vgetq_lane_f32(result, 3);

        // Perspective division
        if (w != 1.0f && w != 0.0f)
        {
            float inv_w = 1.0f / w;
            dst[i].x *= inv_w;
            dst[i].y *= inv_w;
            dst[i].z *= inv_w;
        }

        // Copy other vertex attributes
        dst[i].col = src[i].col;
        dst[i].spc = src[i].spc;
        dst[i].u = src[i].u;
        dst[i].v = src[i].v;
    }
}

// Optimized batch vertex color processing using NEON
void ProcessVertexColorsNeon(Vertex* vertices, int count, const float colorScale[4])
{
    float32x4_t scale = vld1q_f32(colorScale);

    for (int i = 0; i < count; i += 4)
    {
        int remaining = std::min(4, count - i);

        for (int j = 0; j < remaining; j++)
        {
            // Load color as 8-bit values
            uint8x8_t color = vld1_u8((uint8_t*)&vertices[i+j].col);

            // Convert to float
            float32x4_t fcolor = vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(color))));

            // Scale color
            fcolor = vmulq_f32(fcolor, scale);

            // Convert back to 8-bit
            uint16x4_t color16 = vqmovn_u32(vcvtq_u32_f32(fcolor));
            uint8x8_t color8 = vqmovn_u16(vcombine_u16(color16, vdup_n_u16(0)));

            // Store back
            vst1_lane_u32((uint32_t*)&vertices[i+j].col, vreinterpret_u32_u8(color8), 0);
        }
    }
}
#endif
