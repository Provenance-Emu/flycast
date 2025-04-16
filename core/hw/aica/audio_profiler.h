#pragma once

#include "types.h"
#include <chrono>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <map>
#include "log/LogManager.h"

// Simple profiler for audio performance analysis
namespace audio_profiler {

struct ProfileEntry {
    std::string name;
    double totalTime;      // in milliseconds
    double maxTime;        // in milliseconds
    double minTime;        // in milliseconds
    uint64_t callCount;
    
    ProfileEntry() : totalTime(0), maxTime(0), minTime(999999), callCount(0) {}
};

class ScopedProfiler {
private:
    std::string name;
    std::chrono::high_resolution_clock::time_point startTime;
    bool enabled;
    
public:
    ScopedProfiler(const std::string& functionName, bool isEnabled = true) 
        : name(functionName), enabled(isEnabled) {
        if (enabled)
            startTime = std::chrono::high_resolution_clock::now();
    }
    
    ~ScopedProfiler() {
        if (enabled) {
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
            recordTime(name, duration / 1000.0); // Convert to milliseconds
        }
    }
    
    static void recordTime(const std::string& name, double timeMs);
    static void printResults();
    static void reset();
};

// Global profiling data
extern std::map<std::string, ProfileEntry> profileData;
extern std::mutex profileMutex;
extern bool profilingEnabled;

// Helper macros for easy profiling
#define PROFILE_AUDIO(name) audio_profiler::ScopedProfiler profiler(name, audio_profiler::profilingEnabled)
#define ENABLE_AUDIO_PROFILING() audio_profiler::profilingEnabled = true
#define DISABLE_AUDIO_PROFILING() audio_profiler::profilingEnabled = false
#define PRINT_AUDIO_PROFILE() audio_profiler::ScopedProfiler::printResults()
#define RESET_AUDIO_PROFILE() audio_profiler::ScopedProfiler::reset()

} // namespace audio_profiler
