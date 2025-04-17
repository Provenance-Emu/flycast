//
// Created by Cascade on 2025-04-16.
//

#include "AudioEngine.h"
#include "NullAudioBackend.h" // Need this for the set_source_buffer call
#include "AudioBackend.h"
#include "AudioRingBuffer.h"
// #include "log/Log.h" // Removed - Not available in Libretro context

#include <algorithm> // For std::fill
#include <chrono>
#include <cstring> // For std::memcpy
#include <stdexcept> // For std::runtime_error
#include <iostream> // TODO: Replace with proper logging

// Provenance Logging - Assuming DLOG/ILOG/WLOG/ELOG are available
// #include "path/to/your/log_header.h"
#ifndef DLOG
#include <cstdio>
#define DLOG(...) // Removed
#define ILOG(...) // Removed
#define WLOG(...) // Removed
#define ELOG(...) // Removed
#endif

AudioEngine::AudioEngine(std::unique_ptr<AudioBackend> backend)
    : backend_(std::move(backend)), // Use backend_
      ring_buffer_(std::make_unique<AudioRingBuffer<int16_t>>(DEFAULT_AUDIO_RING_BUFFER_SIZE)),
      running_(false),
      initialized_(false) {
    if (!backend_) { // Use backend_
        // Potentially throw or log an error if backend is null
        // ELOG("AudioEngine created with a null backend!"); // Removed
        // Consider throwing std::runtime_error here
    }
    // DLOG("AudioEngine created."); // Removed
}

AudioEngine::~AudioEngine() {
    shutdown();
    // DLOG("AudioEngine destroyed."); // Removed
}

bool AudioEngine::init(
    // No backend parameter here anymore
    int desired_sample_rate,
    int desired_buffer_frames,
    size_t ring_buffer_size
) {
    if (initialized_) {
        // WLOG("AudioEngine::init warning: Already initialized."); // Removed
        return true; // Or false, depending on desired behavior
    }

    if (!backend_) { // Check the member variable directly
        // ELOG("AudioEngine::init error: Backend is null (must be provided in constructor)."); // Removed
        return false;
    }

    // Re-initialize ring buffer if a specific size is requested
    if (ring_buffer_size != DEFAULT_AUDIO_RING_BUFFER_SIZE) {
        ring_buffer_ = std::make_unique<AudioRingBuffer<int16_t>>(ring_buffer_size);
        if (!ring_buffer_) { // Check allocation success
             // ELOG("AudioEngine::init error: Failed to allocate ring buffer."); // Removed
             return false;
        }
    }

    // Initialize the backend
    if (!backend_->init(desired_sample_rate, desired_buffer_frames)) { // Use backend_
        // ELOG("AudioEngine::init error: Failed to initialize audio backend."); // Removed
        return false;
    }

    // --- Compatibility Issue: Backend likely doesn't need source buffer set --- 
    // If AudioBackend uses a callback model, it likely *pulls* data via
    // audio_callback rather than having a buffer pushed to it.
    // Commenting this out as it's not in the AudioBackend interface.
    // backend_->set_source_buffer(ring_buffer_.get()); // Use backend_ 

    // Prepare the callback buffer
    // TODO: Retrieve actual values? Currently commented out in original logic.
    // actual_sample_rate_ = backend_->get_sample_rate();
    // backend_buffer_frames_ = backend_->get_buffer_size(); // Use backend_

    // Resize internal buffer based on backend settings
    // Assuming stereo (2 channels) as backend doesn't provide channel info
    callback_buffer_.resize(backend_->get_buffer_size() * 2); // Use backend_->get_buffer_size()

    if (callback_buffer_.empty())
    {
        // ELOG("AudioEngine::init error: Failed to allocate callback buffer."); // Removed
        return false;
    }

    // Start the audio thread
    running_ = true;
    try {
        audio_thread_ = std::thread(&AudioEngine::audio_thread_main, this);
    } catch (const std::system_error& e) {
        // ELOG("AudioEngine::init error: Failed to start audio thread: %s", e.what()); // Removed
        running_ = false;
        backend_->shutdown(); // Use backend_
        return false;
    }

    initialized_ = true;
    // ILOG("AudioEngine initialized successfully."); // Removed
    return true;
}

void AudioEngine::shutdown() {
    // DLOG("AudioEngine::shutdown starting..."); // Removed
    if (!initialized_) {
        // WLOG("AudioEngine not initialized, nothing to shut down."); // Removed
        return;
    }

    running_ = false;

    if (audio_thread_.joinable()) {
        // DLOG("Joining audio thread..."); // Removed
        try {
            audio_thread_.join();
            // DLOG("Audio thread joined."); // Removed
        } catch (const std::system_error& e) {
            // ELOG("Error joining audio thread: %s", e.what()); // Removed
            // Continue shutdown regardless
        }
    }

    // Shutdown the backend
    if (backend_) { // Use backend_
        backend_->shutdown(); // Use backend_
    }

    ring_buffer_.reset();
    callback_buffer_.clear();
    initialized_ = false;
    // ILOG("AudioEngine shutdown complete."); // Removed
}

bool AudioEngine::push_samples(const int16_t* samples, size_t num_samples) {
    if (!initialized_ || !ring_buffer_) {
        // WLOG("AudioEngine::push_samples called while not initialized or buffer missing."); // Removed
        return false; // Or handle differently, maybe true if not initialized?
    }

    bool success = true;
    for (size_t i = 0; i < num_samples; ++i) {
        if (!ring_buffer_->try_write(samples[i])) {
            // Buffer full, stop writing and report failure
            // WLOG("Audio ring buffer full during non-blocking write."); // Removed
            success = false;
            break;
        }
    }
    return success;
}

void AudioEngine::push_samples_blocking(const int16_t* samples, size_t num_samples) {
    if (!initialized_ || !ring_buffer_) {
        // WLOG("AudioEngine::push_samples_blocking called while not initialized or buffer missing."); // Removed
        return;
    }

    for (size_t i = 0; i < num_samples; ++i) {
        ring_buffer_->write(samples[i]); // This will block/spin if full
    }
}

int AudioEngine::get_sample_rate() const {
    return initialized_ ? backend_->get_sample_rate() : 0;
}

int AudioEngine::get_backend_buffer_size() const {
     return initialized_ ? backend_->get_buffer_size() : 0; // Use backend_->get_buffer_size()
}

size_t AudioEngine::get_ring_buffer_level() const {
    return (initialized_ && ring_buffer_) ? ring_buffer_->size() : 0;
}

void AudioEngine::audio_thread_main() {
    while (running_.load()) {
        // TODO: This whole thread logic needs rethinking based on AudioBackend callback design.
        // The current push model doesn't fit the interface.
        // For now, commenting out incompatible calls to allow compilation.

        // backend_->wait_for_buffer_request(); // ERROR: Method does not exist

        // Calculate how much data is needed
        const size_t frames_to_read = backend_->get_buffer_size(); // Use backend_->get_buffer_size()
        // Assuming stereo (2 channels) as backend doesn't provide channel info
        const size_t samples_to_read = frames_to_read * 2;

        if (samples_to_read == 0 || callback_buffer_.size() < samples_to_read) {
            // Avoid division by zero or buffer overflow if backend gives weird values
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Prevent tight spin
            continue;
        }

        // Read data from ring buffer into callback buffer, one sample at a time
        size_t samples_read = 0;
        for (size_t i = 0; i < samples_to_read; ++i) {
            if (!ring_buffer_->try_read(callback_buffer_[i])) {
                // Underflow during read loop - buffer became empty
                break; // Exit loop, remaining will be filled with silence later
            }
            samples_read++;
        }

        // Handle underflow - fill remaining part of the buffer with silence
        if (samples_read < samples_to_read) {
            std::fill(callback_buffer_.begin() + samples_read, callback_buffer_.begin() + samples_to_read, 0);
        }

        // backend_->submit_buffer(callback_buffer_.data(), frames_to_read); // ERROR: Method does not exist

        // Since we can't submit, add a small sleep to prevent this thread
        // from consuming 100% CPU in a tight loop.
        // This is a temporary measure until the logic is refactored.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
