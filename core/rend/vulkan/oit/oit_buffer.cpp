#include "oit_buffer.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
void optimized_clear_abuffer(uint32_t* buffer, size_t size)
{
    // Clear 16 pixels (64 bytes) at a time
    uint32x4_t zero = vdupq_n_u32(0);

    size_t i = 0;
    for (; i + 16 <= size; i += 16)
    {
        vst1q_u32(buffer + i, zero);
        vst1q_u32(buffer + i + 4, zero);
        vst1q_u32(buffer + i + 8, zero);
        vst1q_u32(buffer + i + 12, zero);
    }

    // Clear remaining pixels
    for (; i < size; i++)
        buffer[i] = 0;
}
#endif
