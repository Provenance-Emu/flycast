// SPDX-License-Identifier: MIT
// Copyright (c) 2024 flycast
// Based on NullAudioBackend

#pragma once

#include "audio/AudioBackend.h"
#include "AudioRingBuffer.h"

/**
 * @brief Audio backend implementation for libretro.
 *
 * This backend conforms to the AudioBackend interface but relies on the
 * libretro frontend to pull audio samples via the AudioEngine.
 * It does not actively manage audio hardware or playback threads.
 */
class LibretroAudioBackend : public AudioBackend {
public:
    LibretroAudioBackend();
    ~LibretroAudioBackend() override;

    // Disable copy and assign
    LibretroAudioBackend(const LibretroAudioBackend&) = delete;
    LibretroAudioBackend& operator=(const LibretroAudioBackend&) = delete;

    /// Initializes the audio backend.
    /// @param desired_sample_rate Preferred sample rate.
    /// @param desired_buffer_size Preferred buffer size in frames.
    /// @return True if initialization was successful, false otherwise.
    bool init(int desired_sample_rate, int desired_buffer_size) override;

    /// Shuts down the audio backend.
    void shutdown() override;

    /// Starts audio playback (no-op for libretro).
    void start() override;

    /// Stops audio playback (no-op for libretro).
    void stop() override;

    /// Gets the actual sample rate used by the backend.
    /// @return The sample rate in Hz.
    int get_sample_rate() const override;

    /// Gets the actual buffer size used by the backend.
    /// @return The buffer size in frames.
    int get_buffer_size() const override;

    /// Sets the ring buffer used as the audio source.
    /// @param buffer Pointer to the AudioRingBuffer.
    // Method specific to this backend for setting the source ring buffer
    void set_source_buffer(AudioRingBuffer<int16_t>* buffer);

    /// Audio callback function (unused in libretro pull model).
    /// @param buffer Buffer to fill with audio data.
    /// @param num_frames Number of frames requested.
    void audio_callback(int16_t* buffer, size_t num_frames) override;

private:
    bool initialized_ = false;
    int sample_rate_ = 0;
    int buffer_size_ = 0;
    AudioRingBuffer<int16_t>* source_buffer_ = nullptr; // Keep a reference, though not actively used by this backend
};
