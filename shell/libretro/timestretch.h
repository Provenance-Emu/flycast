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

        // Simple implementation for fast-forward: just drop samples
        if (mStretchFactor < 0.5f)
        {
            int outputFrames = 0;
            int skipFactor = 2;  // Skip every other sample

            for (int i = 0; i < inputFrames && outputFrames < maxOutputFrames; i += skipFactor)
            {
                for (int ch = 0; ch < mChannels; ch++)
                    output[outputFrames * mChannels + ch] = input[i * mChannels + ch];
                outputFrames++;
            }

            return outputFrames;
        }

        // For slow motion, duplicate samples
        if (mStretchFactor > 1.5f)
        {
            int outputFrames = 0;
            int dupFactor = 2;  // Duplicate each sample

            for (int i = 0; i < inputFrames && outputFrames < maxOutputFrames; i++)
            {
                for (int dup = 0; dup < dupFactor && outputFrames < maxOutputFrames; dup++)
                {
                    for (int ch = 0; ch < mChannels; ch++)
                        output[outputFrames * mChannels + ch] = input[i * mChannels + ch];
                    outputFrames++;
                }
            }

            return outputFrames;
        }

        // Default case: simple linear interpolation
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
