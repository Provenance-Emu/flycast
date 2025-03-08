#pragma once

#include "types.h"
#include <vector>
#include <algorithm>
#include <cstring>

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// Simple time stretching class optimized for ARM64 NEON
class TimeStretcher
{
public:
    TimeStretcher(int channels = 2, int windowSize = 1024)
        : mChannels(channels), mWindowSize(windowSize), mStretchFactor(1.0f), mPhase(0.0f)
    {
    }

    // Set the stretch factor (1.0 = normal speed, 2.0 = half speed, 0.5 = double speed)
    void setStretchFactor(float factor)
    {
        mStretchFactor = factor;
    }

    // Process input samples and return the number of output samples
    int process(const s16* input, int inputFrames, s16* output, int maxOutputFrames)
    {
        // Fast path for normal speed
        if (mStretchFactor >= 0.98f && mStretchFactor <= 1.02f)
        {
            int framesToCopy = std::min(inputFrames, maxOutputFrames);
            memcpy(output, input, framesToCopy * mChannels * sizeof(s16));
            return framesToCopy;
        }

        // For unthrottled mode, we need to be very aggressive with sample dropping
        if (mStretchFactor < 0.5f)
        {
            // Calculate skip factor based on stretch factor
            // Lower stretch factor = more samples skipped
            int skipFactor = std::max(4, static_cast<int>(1.0f / mStretchFactor));

            int outputFrames = 0;
            for (int i = 0; i < inputFrames && outputFrames < maxOutputFrames; i += skipFactor)
            {
                // Add a tiny bit of noise to break up patterns
                static u32 noise_seed = 0x55555555;
                noise_seed = noise_seed * 1664525 + 1013904223;

                for (int ch = 0; ch < mChannels; ch++)
                {
                    int noise = ((noise_seed >> (ch * 8)) & 3) - 1; // -1, 0, or 1
                    output[outputFrames * mChannels + ch] = input[i * mChannels + ch] + noise;
                }
                outputFrames++;

                // Occasionally skip a different number of samples to break patterns
                if ((i & 0x3F) == 0)
                    i += (noise_seed >> 24) & 3;
            }

            return outputFrames;
        }

        // For other cases, use simple linear interpolation
        return processLinearInterpolation(input, inputFrames, output, maxOutputFrames);
    }

private:
    // Simple linear interpolation
    int processLinearInterpolation(const s16* input, int inputFrames, s16* output, int maxOutputFrames)
    {
        int outputFrames = 0;
        float phaseIncrement = 1.0f / mStretchFactor;

        // Reset phase if needed
        if (mPhase >= inputFrames)
            mPhase = 0.0f;

        while (outputFrames < maxOutputFrames)
        {
            // Calculate input position
            int inputIdx = static_cast<int>(mPhase);
            if (inputIdx >= inputFrames - 1)
                break;

            // Calculate fractional part for interpolation
            float frac = mPhase - inputIdx;

            // Linear interpolation for each channel
            for (int ch = 0; ch < mChannels; ch++)
            {
                float sample1 = static_cast<float>(input[inputIdx * mChannels + ch]);
                float sample2 = static_cast<float>(input[(inputIdx + 1) * mChannels + ch]);
                float interpolated = sample1 + frac * (sample2 - sample1);
                output[outputFrames * mChannels + ch] = static_cast<s16>(interpolated);
            }

            // Advance phase
            mPhase += phaseIncrement;
            outputFrames++;
        }

        return outputFrames;
    }

    int mChannels;
    int mWindowSize;
    float mStretchFactor;
    float mPhase;
};
