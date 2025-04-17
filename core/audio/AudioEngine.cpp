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
    actual_sample_rate_ = backend_->get_sample_rate();
    backend_buffer_frames_ = backend_->get_buffer_size(); // Use backend_->get_buffer_size()

    // Resize internal buffer based on backend settings
    // Assuming stereo (2 channels) as backend doesn't provide channel info
    // callback_buffer_.resize(backend_->get_buffer_size() * 2); // Use backend_->get_buffer_size()

    // if (callback_buffer_.empty())
    // {
    //     // ELOG("AudioEngine::init error: Failed to allocate callback buffer."); // Removed
    //     return false;
    // }

    // // Start the audio thread
    // running_ = true;
    // try {
    //     audio_thread_ = std::thread(&AudioEngine::audio_thread_main, this);
    // } catch (const std::system_error& e) {
    //     // ELOG("AudioEngine::init error: Failed to start audio thread: %s", e.what()); // Removed
    //     running_ = false;
    //     backend_->shutdown(); // Use backend_
    //     return false;
    // }

    initialized_ = true;
    // ILOG("AudioEngine initialized successfully."); // Removed
    return true;
}

void AudioEngine::shutdown() {
    if (!initialized_) {
        return;
    }

    // Shutdown the backend
    if (backend_) { // Check if backend_ is valid
        backend_->shutdown();
        // backend_.release(); // Release ownership if managed externally - Let unique_ptr handle this
    }

    // Clear the ring buffer
    if (ring_buffer_) { // Check if ring_buffer_ is valid
        // Removed: ring_buffer_->clear(); - Method does not exist
        // ring_buffer_.reset(); // Explicitly reset unique_ptr - Let unique_ptr handle this
    }

    // Reset state variables
    initialized_ = false;
    actual_sample_rate_ = 0;
    backend_buffer_frames_ = 0;

    // DLOG("AudioEngine shut down."); // Removed
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

size_t AudioEngine::read_samples(int16_t* buffer, size_t num_frames) {
    if (!initialized_ || !ring_buffer_ || !buffer) {
        return 0;
    }

    // Each frame consists of 2 samples (stereo)
    size_t samples_to_read = num_frames * 2;
    size_t samples_read = 0;

    // Read data from ring buffer into output buffer, one sample at a time
    for (size_t i = 0; i < samples_to_read; ++i) {
        if (!ring_buffer_->try_read(buffer[i])) {
            // Underflow - buffer is empty
            break;
        }
        samples_read++;
    }

    // Return the number of frames read (samples / 2)
    return samples_read / 2;
}

int AudioEngine::get_sample_rate() const {
    return initialized_ ? actual_sample_rate_ : 0;
}

int AudioEngine::get_backend_buffer_size() const {
     return initialized_ ? backend_->get_buffer_size() : 0; // Use backend_->get_buffer_size()
}

size_t AudioEngine::get_ring_buffer_level() const {
    return (initialized_ && ring_buffer_) ? ring_buffer_->size() : 0;
}
