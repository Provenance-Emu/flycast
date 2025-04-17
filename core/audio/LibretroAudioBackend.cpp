// SPDX-License-Identifier: MIT
// Copyright (c) 2024 flycast

#include "LibretroAudioBackend.h"
#include <iostream>

LibretroAudioBackend::LibretroAudioBackend() {

}

LibretroAudioBackend::~LibretroAudioBackend() {
    shutdown(); // Ensure shutdown is called
    std::cout << "LibretroAudioBackend destroyed." << std::endl;
}

bool LibretroAudioBackend::init(int desired_sample_rate, int desired_buffer_size) {
    std::cout << "LibretroAudioBackend::init(rate=" << desired_sample_rate << ", buffer=" << desired_buffer_size << ")" << std::endl;
    if (initialized_) {
        std::cout << "LibretroAudioBackend already initialized." << std::endl;
        return true; // Or false? Depending on desired behavior for re-init
    }

    // In the libretro context, we usually just accept the rates provided
    // by the core/engine, as the frontend will handle the actual playback.
    sample_rate_ = desired_sample_rate;
    buffer_size_ = desired_buffer_size; // Store this, though libretro might use its own frame count

    if (sample_rate_ <= 0) {
        std::cout << "Invalid sample rate requested: " << sample_rate_ << std::endl;
        return false;
    }

    initialized_ = true;
    std::cout << "LibretroAudioBackend initialized successfully (Rate: " << sample_rate_ << " Hz, Buffer: " << buffer_size_ << " frames)." << std::endl;
    return true;
}

void LibretroAudioBackend::shutdown() {
    std::cout << "LibretroAudioBackend::shutdown()" << std::endl;
    if (!initialized_) {
        return;
    }

    // No active resources to release for this simple backend
    initialized_ = false;
    sample_rate_ = 0;
    buffer_size_ = 0;
    source_buffer_ = nullptr;
    std::cout << "LibretroAudioBackend shut down." << std::endl;
}

void LibretroAudioBackend::start() {
    // No-op for libretro pull model
    std::cout << "LibretroAudioBackend::start() called (no-op)." << std::endl;
}

void LibretroAudioBackend::stop() {
    // No-op for libretro pull model
    std::cout << "LibretroAudioBackend::stop() called (no-op)." << std::endl;
}

int LibretroAudioBackend::get_sample_rate() const {
    return sample_rate_;
}

int LibretroAudioBackend::get_buffer_size() const {
    // Note: The libretro frontend determines the actual number of samples needed per frame.
    // This value might not be directly used by the frontend.
    return buffer_size_;
}

void LibretroAudioBackend::set_source_buffer(AudioRingBuffer<int16_t>* buffer) {
    std::cout << "LibretroAudioBackend::set_source_buffer()" << std::endl;
    source_buffer_ = buffer;
    // The AudioEngine uses this buffer directly; the backend doesn't need to interact with it.
}

void LibretroAudioBackend::audio_callback(int16_t* buffer, size_t num_frames) {
    // This callback is typically used in push models. In libretro's pull model,
    // the frontend calls AudioEngine::read_samples via retro_run().
    // This function should not be called in the libretro context.
    std::cout << "LibretroAudioBackend::audio_callback() called unexpectedly." << std::endl;
    // Optionally clear the buffer to ensure silence if called incorrectly
    // memset(buffer, 0, num_frames * 2 * sizeof(int16_t));
}
