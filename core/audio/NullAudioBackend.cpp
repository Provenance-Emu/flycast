//
// Created by Cascade on 2025-04-16.
//

#include "NullAudioBackend.h"
#include <vector>
#include <cstring>
#include <algorithm>

// Provenance Logging - Assuming DLOG is available via a common header
// #include "path/to/your/log_header.h" // Replace with actual path if needed
// If DLOG isn't readily available, we can use printf for now.
#ifndef DLOG
#include <cstdio>
#define DLOG(...) printf("DEBUG: " __VA_ARGS__); printf("\n")
#endif

NullAudioBackend::NullAudioBackend() :
    sample_rate_(0),
    buffer_size_(0),
    source_buffer_(nullptr),
    initialized_(false) {
    DLOG("NullAudioBackend created.");
}

NullAudioBackend::~NullAudioBackend() {
    if (initialized_) {
        shutdown();
    }
    DLOG("NullAudioBackend destroyed.");
}

bool NullAudioBackend::init(int desired_sample_rate, int desired_buffer_size) {
    DLOG("NullAudioBackend::init(rate=%d, buffer=%d)", desired_sample_rate, desired_buffer_size);
    if (initialized_) {
        DLOG("NullAudioBackend already initialized.");
        return false;
    }
    // We just accept the desired values for the null backend.
    sample_rate_ = desired_sample_rate;
    buffer_size_ = desired_buffer_size;
    initialized_ = true;
    DLOG("NullAudioBackend initialized successfully.");
    return true;
}

void NullAudioBackend::shutdown() {
    DLOG("NullAudioBackend::shutdown()");
    if (!initialized_) {
        return;
    }
    stop(); // Ensure stream is stopped
    initialized_ = false;
    source_buffer_ = nullptr; // Clear pointer
    DLOG("NullAudioBackend shut down.");
}

void NullAudioBackend::start() {
    // Nothing to do for the null backend
    DLOG("NullAudioBackend::start() called.");
}

void NullAudioBackend::stop() {
    // Nothing to do for the null backend
    DLOG("NullAudioBackend::stop() called.");
}

int NullAudioBackend::get_sample_rate() const {
    return sample_rate_;
}

int NullAudioBackend::get_buffer_size() const {
    return buffer_size_;
}

void NullAudioBackend::set_source_buffer(AudioRingBuffer<int16_t>* buffer) {
    source_buffer_ = buffer;
}

void NullAudioBackend::audio_callback(int16_t* buffer, size_t num_frames) {
    if (!initialized_ || !source_buffer_) {
        // Not initialized or no buffer set, fill with silence
        memset(buffer, 0, num_frames * 2 * sizeof(int16_t));
        return;
    }

    size_t samples_to_read = num_frames * 2; // Stereo samples
    size_t samples_read = 0;

    // Try to read the requested number of samples from the ring buffer
    while (samples_read < samples_to_read) {
        if (!source_buffer_->try_read(buffer[samples_read])) {
            // Buffer is empty, fill the rest with silence
            memset(&buffer[samples_read], 0, (samples_to_read - samples_read) * sizeof(int16_t));
            // Optionally log underflow
            // WLOG("Audio buffer underflow! %zu samples requested, %zu read.", samples_to_read, samples_read);
            break;
        }
        samples_read++;
    }
}
