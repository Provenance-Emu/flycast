#include "audio_thread.h"
#include "audiostream.h"
#include "hw/aica/sgc_if.h"
#include "hw/aica/aica.h"
#include "hw/aica/audio_profiler.h"
#include "cfg/option.h"
#include "emulator.h"

#include <chrono>
#include <algorithm>
#include <thread>

// Initialize static members
std::thread AudioThread::audioThread;
std::atomic<bool> AudioThread::running{false};
std::atomic<bool> AudioThread::requestPending{false};
std::atomic<float> AudioThread::processingLoad{0.0f};
std::atomic<AudioThread::ResamplingQuality> AudioThread::resamplingQuality{AudioThread::ResamplingQuality::HIGH};
std::mutex AudioThread::mutex;
std::condition_variable AudioThread::condition;
std::array<AudioThread::ChannelType, 64> AudioThread::channelTypes{};
std::array<u64, AudioThread::METRICS_WINDOW_SIZE> AudioThread::processingTimes{};
int AudioThread::metricsIndex{0};
std::atomic<float> AudioThread::bufferFullness{0.5f};
int AudioThread::bgmProcessCounter{0};

// Global audio ring buffer
AudioRingBuffer<AudioSample, 16384> g_audioRingBuffer;

// Initialize the audio thread system
bool AudioThread::init() {
    // Initialize channel types to UNKNOWN
    for (auto& type : channelTypes) {
        type = ChannelType::UNKNOWN;
    }
    
    // Reset metrics
    processingTimes.fill(0);
    metricsIndex = 0;
    processingLoad.store(0.0f, std::memory_order_relaxed);
    bufferFullness.store(0.5f, std::memory_order_relaxed);
    bgmProcessCounter = 0;
    
    // Start the audio thread
    running.store(true, std::memory_order_release);
    requestPending.store(false, std::memory_order_release);
    
    try {
        audioThread = std::thread(audioThreadFunc);
        
        // Set thread priority if supported
        setThreadPriority(1); // High priority
        
        INFO_LOG(AUDIO, "Audio thread started successfully");
        return true;
    }
    catch (const std::exception& e) {
        ERROR_LOG(AUDIO, "Failed to start audio thread: %s", e.what());
        running.store(false, std::memory_order_release);
        return false;
    }
}

// Terminate the audio thread system
void AudioThread::term() {
    // Signal the audio thread to stop
    running.store(false, std::memory_order_release);
    
    // Wake up the audio thread if it's waiting
    {
        std::lock_guard<std::mutex> lock(mutex);
        requestPending.store(true, std::memory_order_release);
        condition.notify_one();
    }
    
    // Wait for the audio thread to finish
    if (audioThread.joinable()) {
        audioThread.join();
    }
    
    INFO_LOG(AUDIO, "Audio thread terminated");
}

// Push audio data to the audio thread
void AudioThread::pushAudioRequest() {
    // If the audio thread is not running, process audio in the main thread
    if (!running.load(std::memory_order_acquire)) {
        aica::sgc::AICA_Sample();
        return;
    }
    
    // Signal the audio thread that a new request is pending
    {
        std::lock_guard<std::mutex> lock(mutex);
        requestPending.store(true, std::memory_order_release);
        condition.notify_one();
    }
}

// Audio thread function
void AudioThread::audioThreadFunc() {
    INFO_LOG(AUDIO, "Audio thread started");
    
    while (running.load(std::memory_order_acquire)) {
        bool shouldProcess = false;
        
        // Wait for a request or timeout
        {
            std::unique_lock<std::mutex> lock(mutex);
            
            // Wait for a request or timeout after 1ms
            condition.wait_for(lock, std::chrono::milliseconds(1), [&]() {
                return requestPending.load(std::memory_order_acquire) || !running.load(std::memory_order_acquire);
            });
            
            shouldProcess = requestPending.load(std::memory_order_acquire);
            requestPending.store(false, std::memory_order_release);
        }
        
        // Process audio if requested
        if (shouldProcess) {
            processAudio();
        }
        
        // Update buffer fullness
        bufferFullness.store(g_audioRingBuffer.fullness(), std::memory_order_relaxed);
        
        // Yield to other threads if buffer is very full
        if (g_audioRingBuffer.fullness() > 0.8f) {
            std::this_thread::yield();
        }
    }
    
    INFO_LOG(AUDIO, "Audio thread stopped");
}

// Process audio in the audio thread
void AudioThread::processAudio() {
    // Measure processing time
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Increment background music counter
    bgmProcessCounter = (bgmProcessCounter + 1) % BGM_PROCESS_RATE;
    
    // Process audio
    aica::sgc::AICA_Sample();
    
    // Measure processing time
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();
    
    // Update metrics
    updateMetrics(duration);
}

// Update the audio processing metrics
void AudioThread::updateMetrics(u64 processingTimeNs) {
    // Store processing time
    processingTimes[metricsIndex] = processingTimeNs;
    metricsIndex = (metricsIndex + 1) % METRICS_WINDOW_SIZE;
    
    // Calculate average processing time
    u64 totalTime = 0;
    for (auto time : processingTimes) {
        totalTime += time;
    }
    
    // Calculate processing load (0.0 - 1.0)
    // Assuming 44100Hz sample rate and 1/60 second frame time
    // Maximum processing time would be 16.67ms (16,670,000ns)
    constexpr u64 MAX_PROCESSING_TIME_NS = 16670000;
    float avgTime = static_cast<float>(totalTime) / METRICS_WINDOW_SIZE;
    float load = avgTime / MAX_PROCESSING_TIME_NS;
    
    // Update processing load
    processingLoad.store(std::min(load, 1.0f), std::memory_order_relaxed);
    
    // Adjust resampling quality based on processing load
    if (load > 0.8f) {
        setResamplingQuality(ResamplingQuality::LOW);
    } else if (load > 0.5f) {
        setResamplingQuality(ResamplingQuality::MEDIUM);
    } else {
        setResamplingQuality(ResamplingQuality::HIGH);
    }
}

// Get the current audio buffer fullness
float AudioThread::getBufferFullness() {
    return bufferFullness.load(std::memory_order_relaxed);
}

// Set the audio thread priority
void AudioThread::setThreadPriority(int priority) {
    // This is platform-specific and may not be supported on all platforms
    // For now, we'll just log the request
    INFO_LOG(AUDIO, "Setting audio thread priority to %d", priority);
    
    // On some platforms, we could use platform-specific APIs to set thread priority
    // For example, on Windows, we could use SetThreadPriority
    // On Linux, we could use pthread_setschedparam
    // On macOS, we could use pthread_setschedparam or thread_policy_set
}

// Set the channel type for a specific channel
void AudioThread::setChannelType(u32 channel, ChannelType type) {
    if (channel < channelTypes.size()) {
        channelTypes[channel] = type;
    }
}

// Get the channel type for a specific channel
AudioThread::ChannelType AudioThread::getChannelType(u32 channel) {
    if (channel < channelTypes.size()) {
        return channelTypes[channel];
    }
    return ChannelType::UNKNOWN;
}

// Set the resampling quality
void AudioThread::setResamplingQuality(ResamplingQuality quality) {
    resamplingQuality.store(quality, std::memory_order_relaxed);
}

// Get the current resampling quality
AudioThread::ResamplingQuality AudioThread::getResamplingQuality() {
    return resamplingQuality.load(std::memory_order_relaxed);
}
