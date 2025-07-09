# AICA Threading Audio Fix - Jitless Dynarec

## Problem Statement

**Context**: Flycast iOS ARM64 jitless dynarec implementation  
**Issue**: Intermittent audio dropouts during gameplay in Tony Hawk Pro Skater 2  
**Root Cause**: Threading desynchronization between SH4 emulation and rendering threads  

### Symptoms
- Audio works perfectly for intros, FMVs, and menus
- Audio fails during gameplay (which uses more audio mixer functionality)
- When audio fails, game runs **fast/sped up** but not dropping frames
- Audio provides master timing synchronization for entire emulation
- Workarounds that fix it: Enable framebuffer emulation OR disable threaded rendering

### Technical Analysis
The audio backend provides timing synchronization via `push()` function with wait parameter. When audio doesn't block properly, emulation runs unthrottled. The real problem was threaded rendering blocking the emulation thread in `rend_end_render()` with `renderEnd.Wait()`, preventing AICA from running at its required 4535-cycle intervals.

## Solution Implemented

**File Modified**: `core/hw/pvr/Renderer_if.cpp`  
**Approach**: AICA-aware threading synchronization  

### Three-Part Solution

#### 1. AICA-Aware Message Queue Processing
```cpp
bool waitAndExecuteAICAaware()
```
- **Frequency**: Check AICA timing every 50 calls (~20ms)
- **AICA Threshold**: 95% of 4535-cycle interval 
- **Timeout**: 8ms when AICA needs attention, normal 20-23ms otherwise
- **Performance**: 98% of operations use fast path

#### 2. AICA-Aware Render Synchronization  
```cpp
rend_end_render() modifications
```
- **Frequency**: Check AICA timing every 20 render calls (~300ms at 60fps)
- **AICA Threshold**: 97% of 4535-cycle interval
- **Timeout**: 25ms when AICA needs attention, infinite wait otherwise
- **Performance**: 95% of renders use infinite wait for maximum performance

#### 3. Enhanced AICA Timing Constants
```cpp
static constexpr int AICA_TICK_INTERVAL = 4535;  // 44.1 KHz timing
```

### Implementation Details

**AICA Timing**: 4535 cycles at 44.1 KHz  
**Safety Margins**: Conservative thresholds (95-97%) to minimize performance impact  
**Balanced Approach**: Moderate checking frequency - enough protection without crushing performance

## Current Status

### ✅ **RESOLVED**
- Tony Hawk Pro Skater 2 gameplay audio now works consistently
- Performance maintained at good levels
- No more unthrottled/fast execution when audio fails
- FMV and menu audio continue to work perfectly

### ⚠️ **POTENTIAL SIDE EFFECTS** 
- Some delays in loading scenes where AICA/DSP is heavily used
- Audio-heavy scene transitions may have slight latency
- Need further investigation into optimal timing thresholds

## Technical Background

### AICA (Audio Interface Controller for Arcade)
- Runs at 44.1 KHz requiring service every 4535 SH4 cycles
- Critical for audio timing synchronization 
- ARM7 core for audio DSP processing
- Memory-mapped interface with strict timing requirements

### Threading Architecture
- **Main Thread**: SH4 emulation + AICA processing
- **Render Thread**: GPU command generation and submission  
- **Problem**: Render thread blocking prevented AICA service intervals
- **Solution**: Timeout-based synchronization with AICA deadline awareness

## Future Investigation Areas

### 1. **Scene Loading Optimization**
- Investigate AICA/DSP timing during heavy audio scene loads
- Potential for more granular timeout adjustment during loading phases
- Consider audio workload detection for dynamic timeout scaling

### 2. **Fine-Tuning Thresholds**
- Current: 95% (messages) and 97% (rendering) AICA thresholds
- Experiment with tighter thresholds for better responsiveness
- Profile different games for optimal balance points

### 3. **Performance Metrics**
- Measure actual AICA service interval consistency
- Profile rendering performance impact during audio-heavy scenes
- Consider frame time variance during transitions

### 4. **Alternative Approaches**
- AICA thread priority boosting during critical periods
- Lock-free audio buffer management
- Hybrid infinite/timeout wait strategies based on workload

## Code References

**Primary File**: `core/hw/pvr/Renderer_if.cpp`
- `waitAndExecuteAICAaware()` - Message queue processing
- `rend_end_render()` - Render synchronization  
- AICA timing constants and thresholds

**Related Files**:
- `core/hw/aica/` - AICA implementation
- `core/hw/sh4/interpr/sh4_interpreter.cpp` - Cycle batching
- `core/hw/sh4/sh4_sched.cpp` - SH4 scheduling

## Testing Notes

**Test Game**: Tony Hawk Pro Skater 2  
**Build Command**: `tests/test.sh` with jitless dynarec  
**Binary Location**: `../build_for_tests/Flycast.app/Contents/MacOS/Flycast`

**Success Criteria**:
- Consistent audio during gameplay
- No speed-up/unthrottled execution  
- Maintained performance during FMVs
- Stable frame timing

---

**Date**: January 2024  
**Contributors**: JoeMatt, AI Assistant  
**Status**: Resolved with minor optimization opportunities  
**Next Session**: Investigate scene loading delays and fine-tune thresholds 