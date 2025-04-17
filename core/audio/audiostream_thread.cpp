#include "audiostream.h"
#include "audio_thread.h"
#include "cfg/option.h"
#include "emulator.h"

// This file implements a threaded version of the audio system
// It replaces the direct audio processing with a decoupled thread-based approach

// Initialize the audio thread system
void InitAudioThread()
{
    INFO_LOG(AUDIO, "Initializing audio thread system");
    
    // Clear the audio ring buffer
    g_audioRingBuffer.clear();
    
    // Initialize the audio thread
    if (!AudioThread::init()) {
        WARN_LOG(AUDIO, "Failed to initialize audio thread, falling back to synchronous audio");
    }
    else {
        INFO_LOG(AUDIO, "Audio thread system initialized successfully");
    }
}

// Terminate the audio thread system
void TermAudioThread()
{
    INFO_LOG(AUDIO, "Terminating audio thread system");
    
    // Terminate the audio thread
    AudioThread::term();
}

// Process audio samples from the ring buffer
void ProcessAudioThreadBuffer()
{
    // Check if the audio thread is running
    if (!AudioThread::isRunning()) {
        return;
    }
    
    // Get samples from the ring buffer and write them to the audio stream
    constexpr size_t BATCH_SIZE = 32; // Process in small batches
    AudioSample samples[BATCH_SIZE];
    
    // Read samples from the ring buffer
    size_t samplesRead = g_audioRingBuffer.read(samples, BATCH_SIZE);
    
    // Write samples to the audio stream
    for (size_t i = 0; i < samplesRead; i++) {
        WriteSample(samples[i].right, samples[i].left);
    }
}

// Function to request audio processing from the audio thread
void RequestAudioThreadProcessing()
{
    // Check if the audio thread is running
    if (!AudioThread::isRunning()) {
        return;
    }
    
    // Request audio processing from the audio thread
    AudioThread::pushAudioRequest();
}

// Get the audio buffer fullness
float GetAudioThreadBufferFullness()
{
    // Check if the audio thread is running
    if (!AudioThread::isRunning()) {
        return 0.5f; // Default value if thread is not running
    }
    
    // Get the buffer fullness from the audio thread
    return AudioThread::getBufferFullness();
}

// Get the audio processing load
float GetAudioThreadProcessingLoad()
{
    // Check if the audio thread is running
    if (!AudioThread::isRunning()) {
        return 0.0f; // Default value if thread is not running
    }
    
    // Get the processing load from the audio thread
    return AudioThread::getProcessingLoad();
}
