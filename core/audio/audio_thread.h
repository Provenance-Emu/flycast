#pragma once

#include "types.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <array>
#include <memory>

// Forward declarations
namespace aica::sgc {
    void AICA_Sample();
}

// Audio thread system that decouples audio processing from the main emulation thread
class AudioThread {
public:
    // Initialize the audio thread system
    static bool init();
    
    // Terminate the audio thread system
    static void term();
    
    // Push audio data to the audio thread
    // This is called from the emulation thread
    static void pushAudioRequest();
    
    // Check if the audio thread is running
    static bool isRunning() { return running.load(std::memory_order_acquire); }
    
    // Get the current audio buffer fullness (0.0 = empty, 1.0 = full)
    static float getBufferFullness();
    
    // Get the current audio processing load (0.0 = idle, 1.0 = maxed out)
    static float getProcessingLoad() { return processingLoad.load(std::memory_order_relaxed); }
    
    // Set the audio thread priority
    static void setThreadPriority(int priority);
    
    // Audio channel type classification
    enum class ChannelType {
        UNKNOWN,        // Not yet classified
        BACKGROUND,     // Background music (can be processed less frequently)
        SOUND_EFFECT,   // Sound effects (must be processed every frame)
    };
    
    // Set the channel type for a specific channel
    static void setChannelType(u32 channel, ChannelType type);
    
    // Get the channel type for a specific channel
    static ChannelType getChannelType(u32 channel);
    
    // Adaptive resampling quality levels
    enum class ResamplingQuality {
        HIGH,    // High quality resampling (full quality)
        MEDIUM,  // Medium quality resampling (reduced quality)
        LOW      // Low quality resampling (minimal quality)
    };
    
    // Set the resampling quality
    static void setResamplingQuality(ResamplingQuality quality);
    
    // Get the current resampling quality
    static ResamplingQuality getResamplingQuality();
    
private:
    // Audio thread function
    static void audioThreadFunc();
    
    // Process audio in the audio thread
    static void processAudio();
    
    // Update the audio processing metrics
    static void updateMetrics(u64 processingTimeNs);
    
    // The audio thread
    static std::thread audioThread;
    
    // Audio thread state
    static std::atomic<bool> running;
    static std::atomic<bool> requestPending;
    static std::atomic<float> processingLoad;
    static std::atomic<ResamplingQuality> resamplingQuality;
    
    // Synchronization
    static std::mutex mutex;
    static std::condition_variable condition;
    
    // Channel classification
    static std::array<ChannelType, 64> channelTypes;
    
    // Performance metrics
    static constexpr int METRICS_WINDOW_SIZE = 60;
    static std::array<u64, METRICS_WINDOW_SIZE> processingTimes;
    static int metricsIndex;
    
    // Audio buffer fullness tracking
    static std::atomic<float> bufferFullness;
    
    // Background music processing rate control
    static int bgmProcessCounter;
    static constexpr int BGM_PROCESS_RATE = 2; // Process background music every N frames
};

// Lock-free ring buffer for audio samples
template<typename T, size_t Size>
class AudioRingBuffer {
public:
    AudioRingBuffer() : readPos(0), writePos(0) {
        static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");
    }
    
    // Write data to the ring buffer
    // Returns the number of items actually written
    size_t write(const T* data, size_t count) {
        size_t available = Size - size();
        size_t toWrite = std::min(count, available);
        
        if (toWrite == 0)
            return 0;
            
        size_t wp = writePos.load(std::memory_order_relaxed);
        size_t firstChunk = std::min(toWrite, Size - (wp & (Size - 1)));
        size_t secondChunk = toWrite - firstChunk;
        
        // Write first chunk
        memcpy(&buffer[wp & (Size - 1)], data, firstChunk * sizeof(T));
        
        // Write second chunk if needed
        if (secondChunk > 0)
            memcpy(&buffer[0], data + firstChunk, secondChunk * sizeof(T));
            
        // Update write position
        writePos.store(wp + toWrite, std::memory_order_release);
        
        return toWrite;
    }
    
    // Read data from the ring buffer
    // Returns the number of items actually read
    size_t read(T* data, size_t count) {
        size_t available = size();
        size_t toRead = std::min(count, available);
        
        if (toRead == 0)
            return 0;
            
        size_t rp = readPos.load(std::memory_order_relaxed);
        size_t firstChunk = std::min(toRead, Size - (rp & (Size - 1)));
        size_t secondChunk = toRead - firstChunk;
        
        // Read first chunk
        memcpy(data, &buffer[rp & (Size - 1)], firstChunk * sizeof(T));
        
        // Read second chunk if needed
        if (secondChunk > 0)
            memcpy(data + firstChunk, &buffer[0], secondChunk * sizeof(T));
            
        // Update read position
        readPos.store(rp + toRead, std::memory_order_release);
        
        return toRead;
    }
    
    // Get the number of items in the buffer
    size_t size() const {
        return writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_acquire);
    }
    
    // Get the fullness of the buffer (0.0 = empty, 1.0 = full)
    float fullness() const {
        return static_cast<float>(size()) / Size;
    }
    
    // Clear the buffer
    void clear() {
        readPos.store(0, std::memory_order_relaxed);
        writePos.store(0, std::memory_order_relaxed);
    }
    
private:
    std::array<T, Size> buffer;
    std::atomic<size_t> readPos;
    std::atomic<size_t> writePos;
};

// Audio sample structure
struct AudioSample {
    s16 left;
    s16 right;
};

// Global audio ring buffer
// Using a power of 2 size for efficient masking
extern AudioRingBuffer<AudioSample, 16384> g_audioRingBuffer;
