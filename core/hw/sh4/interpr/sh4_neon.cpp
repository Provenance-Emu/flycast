#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

// Optimized memory copy for SH4 interpreter
void sh4_neon_memcpy(void* dst, const void* src, size_t size)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    // Process 64 bytes at a time
    size_t i = 0;
    for (; i + 64 <= size; i += 64)
    {
        uint8x16_t block1 = vld1q_u8(s + i);
        uint8x16_t block2 = vld1q_u8(s + i + 16);
        uint8x16_t block3 = vld1q_u8(s + i + 32);
        uint8x16_t block4 = vld1q_u8(s + i + 48);

        vst1q_u8(d + i, block1);
        vst1q_u8(d + i + 16, block2);
        vst1q_u8(d + i + 32, block3);
        vst1q_u8(d + i + 48, block4);
    }

    // Process 16 bytes at a time
    for (; i + 16 <= size; i += 16)
    {
        uint8x16_t block = vld1q_u8(s + i);
        vst1q_u8(d + i, block);
    }

    // Handle remaining bytes
    for (; i < size; i++)
        d[i] = s[i];
}

// Optimized vector operations for SH4 interpreter
void sh4_neon_vector_add(float* dst, const float* src1, const float* src2, int count)
{
    for (int i = 0; i < count; i += 4)
    {
        int remaining = std::min(4, count - i);

        if (remaining == 4)
        {
            // Process 4 floats at once
            float32x4_t v1 = vld1q_f32(src1 + i);
            float32x4_t v2 = vld1q_f32(src2 + i);
            float32x4_t result = vaddq_f32(v1, v2);
            vst1q_f32(dst + i, result);
        }
        else
        {
            // Handle remaining elements
            for (int j = 0; j < remaining; j++)
                dst[i + j] = src1[i + j] + src2[i + j];
        }
    }
}

// Optimized matrix multiplication for SH4 interpreter
void sh4_neon_matrix_mul(float* dst, const float* src1, const float* src2, int rows, int common, int cols)
{
    for (int i = 0; i < rows; i++)
    {
        for (int j = 0; j < cols; j++)
        {
            float32x4_t sum = vdupq_n_f32(0);

            // Process 4 elements at a time
            int k = 0;
            for (; k + 4 <= common; k += 4)
            {
                float32x4_t a = vld1q_f32(src1 + i * common + k);
                float32x4_t b = { src2[k * cols + j], src2[(k + 1) * cols + j],
                                  src2[(k + 2) * cols + j], src2[(k + 3) * cols + j] };

                // Multiply and accumulate
                sum = vmlaq_f32(sum, a, b);
            }

            // Sum the 4 partial results
            float result = vaddvq_f32(sum);

            // Handle remaining elements
            for (; k < common; k++)
                result += src1[i * common + k] * src2[k * cols + j];

            dst[i * cols + j] = result;
        }
    }
}
#endif
