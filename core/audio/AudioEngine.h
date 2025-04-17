//
// Created by Cascade on 2025-04-16.
// Manages the audio thread, ring buffer, and backend.
//

#pragma once

#include "AudioRingBuffer.h"
#include "AudioBackend.h"
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>

constexpr size_t DEFAULT_AUDIO_RING_BUFFER_SIZE = 16384; // Power of 2, holds stereo samples

class AudioEngine {
public:
    /// @brief Constructor takes ownership of the provided audio backend.
    /// @param backend A unique_ptr to the AudioBackend implementation to use.
    explicit AudioEngine(std::unique_ptr<AudioBackend> backend);

    /// Initializes the audio engine with a specific backend.
    /// @param desired_sample_rate The desired sample rate.
    /// @param desired_buffer_frames The desired buffer size in frames (for backend callback).
    /// @param ring_buffer_size The size of the internal ring buffer (must be power of 2).
    /// @return True if initialization succeeded, false otherwise.
    bool init(
        int desired_sample_rate,
        int desired_buffer_frames,
        size_t ring_buffer_size = DEFAULT_AUDIO_RING_BUFFER_SIZE
    );

    /// Shuts down the audio engine, stops the thread, and cleans up resources.
    void shutdown();

    /// Pushes stereo audio samples to the ring buffer.
    /// This is called by the audio source (e.g., AICA emulation).
    /// @param samples Pointer to the stereo samples (interleaved L/R).
    /// @param num_samples The total number of samples (num_frames * 2).
    /// @return True if all samples were successfully written (non-blocking), false if buffer was full.
    bool push_samples(const int16_t* samples, size_t num_samples);

    /// Pushes stereo audio samples to the ring buffer, blocking if necessary.
    /// @param samples Pointer to the stereo samples (interleaved L/R).
    /// @param num_samples The total number of samples (num_frames * 2).
    void push_samples_blocking(const int16_t* samples, size_t num_samples);

    /// @brief Reads stereo audio samples from the ring buffer.
    /// @param buffer Pointer to the buffer to receive the samples (interleaved L/R).
    /// @param num_frames The number of stereo frames to read.
    /// @return The number of stereo frames actually read.
    size_t read_samples(int16_t* buffer, size_t num_frames);

    /// @brief Gets the actual sample rate used by the backend.
    /// @return The sample rate in Hz, or 0 if not initialized.
    int get_sample_rate() const;

    /// Gets the buffer size (in frames) the backend is actually using.
    /// @return Buffer size in frames.
    int get_backend_buffer_size() const;

    /// Gets the current number of samples in the ring buffer.
    /// @return Number of samples.
    size_t get_ring_buffer_level() const;

    // Declare virtual destructor
    virtual ~AudioEngine();

    // Deleted copy/move constructors and assignment operators
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

private:
    /// The main function for the audio processing thread.
    // Rename thread function declaration
    void audio_thread_main();

    std::unique_ptr<AudioBackend> backend_ = nullptr;    // The audio output backend
    std::unique_ptr<AudioRingBuffer<int16_t>> ring_buffer_ = nullptr; // Ring buffer for audio data
    std::thread audio_thread_;                   // The dedicated audio thread
    std::atomic<bool> running_{false};
    std::vector<int16_t> callback_buffer_; // Temporary buffer for backend callbacks

    int actual_sample_rate_ = 0;
    int backend_buffer_frames_ = 0;
    bool initialized_ = false;
};
