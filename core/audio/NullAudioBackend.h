//
// Created by Cascade on 2025-04-16.
// Null audio backend implementation (does nothing).
//

#pragma once

#include "AudioBackend.h"
#include "AudioRingBuffer.h"
#include <cstdint>

/// A null audio backend that consumes audio data but produces no output.
class NullAudioBackend : public AudioBackend {
public:
    NullAudioBackend();
    ~NullAudioBackend() override;

    bool init(int desired_sample_rate, int desired_buffer_size) override;
    void shutdown() override;
    void start() override;
    void stop() override;

    int get_sample_rate() const override;
    int get_buffer_size() const override;

    void audio_callback(int16_t* buffer, size_t num_frames) override;

    // Method specific to this backend for setting the source ring buffer
    void set_source_buffer(AudioRingBuffer<int16_t>* buffer);

private:
    int sample_rate_;
    int buffer_size_;
    AudioRingBuffer<int16_t>* source_buffer_ = nullptr; // Pointer to the buffer owned by AudioEngine
    bool initialized_ = false;
};
