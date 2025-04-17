//
// Created by Cascade on 2025-04-16.
// Defines the interface for audio output backends.
//

#pragma once

#include <cstddef>
#include <cstdint>

/// Abstract base class for audio output backends.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    /// Initializes the audio backend.
    /// @param desired_sample_rate The preferred sample rate.
    /// @param desired_buffer_size The preferred buffer size in frames.
    /// @return True if initialization was successful, false otherwise.
    virtual bool init(int desired_sample_rate, int desired_buffer_size) = 0;

    /// Shuts down the audio backend.
    virtual void shutdown() = 0;

    /// Starts the audio stream playback.
    virtual void start() = 0;

    /// Stops the audio stream playback.
    virtual void stop() = 0;

    /// Gets the actual sample rate used by the backend.
    /// @return The sample rate in Hz.
    virtual int get_sample_rate() const = 0;

    /// Gets the actual buffer size used by the backend.
    /// @return The buffer size in frames.
    virtual int get_buffer_size() const = 0;

    /// Called by the AudioEngine to request audio data.
    /// The backend should fill the provided buffer with audio samples.
    /// @param buffer Pointer to the buffer to fill.
    /// @param num_frames The number of stereo frames requested (samples = num_frames * 2).
    virtual void audio_callback(int16_t* buffer, size_t num_frames) = 0;
};
