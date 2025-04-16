#include "audio_profiler.h"
#include "log/LogManager.h"
#include <inttypes.h>

namespace audio_profiler {

std::map<std::string, ProfileEntry> profileData;
std::mutex profileMutex;
bool profilingEnabled = false;

void ScopedProfiler::recordTime(const std::string& name, double timeMs) {
    std::lock_guard<std::mutex> lock(profileMutex);
    
    ProfileEntry& entry = profileData[name];
    entry.name = name;
    entry.totalTime += timeMs;
    entry.callCount++;
    entry.maxTime = std::max(entry.maxTime, timeMs);
    entry.minTime = std::min(entry.minTime, timeMs);
}

void ScopedProfiler::printResults() {
    std::lock_guard<std::mutex> lock(profileMutex);
    
    if (profileData.empty()) {
        INFO_LOG(AICA, "No audio profiling data available");
        return;
    }
    
    // Sort entries by total time
    std::vector<std::pair<std::string, ProfileEntry>> sortedEntries;
    for (const auto& entry : profileData) {
        sortedEntries.push_back(entry);
    }
    
    std::sort(sortedEntries.begin(), sortedEntries.end(), 
              [](const auto& a, const auto& b) {
                  return a.second.totalTime > b.second.totalTime;
              });
    
    // Print header
    INFO_LOG(AICA, "===== Audio Performance Profile =====");
    INFO_LOG(AICA, "%-30s %-10s %-10s %-10s %-10s %-10s", 
             "Function", "Total(ms)", "Calls", "Avg(ms)", "Min(ms)", "Max(ms)");
    
    // Print data
    for (const auto& entry : sortedEntries) {
        const ProfileEntry& data = entry.second;
        double avgTime = data.callCount > 0 ? data.totalTime / data.callCount : 0;
        
        // Convert uint64_t to string to avoid format string issues
        std::string callCountStr = std::to_string(data.callCount);
        
        INFO_LOG(AICA, "%-30s %-10.2f %-10s %-10.3f %-10.3f %-10.3f",
                 data.name.c_str(), data.totalTime, callCountStr.c_str(), 
                 avgTime, data.minTime, data.maxTime);
    }
    
    INFO_LOG(AICA, "====================================");
}

void ScopedProfiler::reset() {
    std::lock_guard<std::mutex> lock(profileMutex);
    profileData.clear();
}

} // namespace audio_profiler
