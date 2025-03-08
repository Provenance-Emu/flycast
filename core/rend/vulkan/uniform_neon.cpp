#include "buffer.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

// Optimized uniform buffer update using NEON
void UpdateUniformBufferNeon(void* dst, const void* src, size_t size)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    // Process 16 bytes at a time
    size_t i = 0;
    for (; i + 16 <= size; i += 16)
    {
        uint8x16_t data = vld1q_u8(s + i);
        vst1q_u8(d + i, data);
    }

    // Handle remaining bytes
    for (; i < size; i++)
        d[i] = s[i];
}

// Optimized matrix transformation for uniform buffers
void UpdateMatrixUniformNeon(float* dst, const float* src, int count)
{
    for (int i = 0; i < count; i++)
    {
        // Load 4x4 matrix (16 floats)
        float32x4_t row0 = vld1q_f32(src + i * 16);
        float32x4_t row1 = vld1q_f32(src + i * 16 + 4);
        float32x4_t row2 = vld1q_f32(src + i * 16 + 8);
        float32x4_t row3 = vld1q_f32(src + i * 16 + 12);

        // Store matrix
        vst1q_f32(dst + i * 16, row0);
        vst1q_f32(dst + i * 16 + 4, row1);
        vst1q_f32(dst + i * 16 + 8, row2);
        vst1q_f32(dst + i * 16 + 12, row3);
    }
}
#endif
