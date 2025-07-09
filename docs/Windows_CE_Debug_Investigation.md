# Windows CE Debug Investigation

## Problem Statement
**Context**: Flycast RetroArch libretro build (dynarec disabled, pure interpreter)  
**Issue**: Windows CE games hang after BIOS screen  
**Symptoms**:
- BIOS boots normally 
- MMU enabled message appears: `[libretro INFO] core/hw/sh4/modules/mmu.cpp:450 N[SH4]: Enabling Full MMU support`
- Game never progresses past this point

## Debug Approach
Added comprehensive debug logging to `core/hw/sh4/modules/wince.h` to track every step of the Windows CE address resolution:

### Debug Logging Added  
```cpp
// In core/hw/sh4/modules/ccn.cpp - TTB Register Write Handler:
ERROR_LOG(SH4, "🔧 TTB WRITE: PC=0x%08X writing TTB=0x%08X (was 0x%08X)", next_pc, data, CCN_TTB);
ERROR_LOG(SH4, "🎯 TTB NON-ZERO: Windows CE setting up page tables at 0x%08X", data);
ERROR_LOG(SH4, "✅ TTB VALID: Page table in system RAM range"); // or ⚠️ TTB UNUSUAL

// In core/hw/sh4/modules/wince.h - Windows CE Address Resolution:
ERROR_LOG(SH4, "🔧 WINCE DEBUG: Attempting to resolve VA=0x%08X", va);
ERROR_LOG(SH4, "🔧 WINCE DEBUG: CCN_TTB=0x%08X", CCN_TTB);  
ERROR_LOG(SH4, "🔧 WINCE DEBUG: page_group=0x%08X", page_group);
ERROR_LOG(SH4, "🔧 WINCE DEBUG: paddr=0x%08X", paddr);
ERROR_LOG(SH4, "✅ WINCE SUCCESS: Resolved VA=0x%08X to PA=0x%08X", va, result);
ERROR_LOG(SH4, "🚫 WINCE HACK FAILED: VA=0x%08X could not be resolved", va);

// In core/hw/sh4/modules/fastmmu.cpp - TLB Miss Tracking:  
DEBUG_LOG(SH4, "🔍 TLB LOOKUP: VA=0x%08X PC=0x%08X", va, next_pc);
ERROR_LOG(SH4, "💥 TLB MISS: VA=0x%08X PC=0x%08X - Windows CE boot may hang here", va, next_pc);
```

### Diagnostic Framework
**🔧 Debug System**: Tracks every step of Windows CE page table walking
- **TTB Analysis**: Translation Table Base register values
- **Page Table Structure**: First-level and second-level page table entries
- **Register Bank**: r_bank[4] analysis for kernel context
- **Memory Layout**: All virtual-to-physical translation attempts

### Current Status - Debug Build Ready
- ✅ **Detailed logging system**: Every wince_resolve_address() call traced
- ✅ **Build successful**: ../build_for_tests/Flycast.app/Contents/MacOS/Flycast  
- ⚠️ **Next Step**: Test with Windows CE game to analyze failure patterns

### Expected Debug Output
When testing with Windows CE game, look for patterns like:
```
🔧 WINCE DEBUG: CCN_TTB=0x8c000000  // Should point to valid page tables
🔧 WINCE DEBUG: page_group=0x00000000  // May indicate page tables not set up
🔧 WINCE DEBUG: FAIL: paddr doesn't have high bit set  // Common failure mode
```

## Investigation Results
**BUILD STATUS**: ✅ Debug version ready for testing 

## Expected Behavior Analysis

### **Case 1: TTB Never Written (Most Likely)**
```
Enabling Full MMU support  ← This works
🔍 TLB LOOKUP: VA=0x02010008 PC=0x8C0215F4
🔧 WINCE DEBUG: CCN_TTB=0x00000000  ← Problem: TTB never set!
🚫 WINCE HACK FAILED: VA=0x02010008 could not be resolved
💥 TLB MISS: VA=0x02010008 PC=0x8C0215F4 - Windows CE boot may hang here
```

**Diagnosis**: Windows CE kernel crash/infinite loop before MMU setup  
**Investigation**: Check REIOS→Windows CE boot handoff process

### **Case 2: TTB Written But Page Resolution Fails**  
```
🔧 TTB WRITE: PC=0x8C001234 writing TTB=0x8C100000 (was 0x00000000)
🎯 TTB NON-ZERO: Windows CE setting up page tables at 0x8C100000
✅ TTB VALID: Page table in system RAM range
🔍 TLB LOOKUP: VA=0x02010008 PC=0x8C0215F4  
🔧 WINCE DEBUG: page_group=0x00000000  ← Problem: Invalid page table data
🚫 WINCE HACK FAILED: VA=0x02010008 could not be resolved
```

**Diagnosis**: Page table corruption or format mismatch  
**Investigation**: Examine page table contents and Windows CE MMU differences

### **Case 3: Successful Operation (Target Goal)**
```
🔧 TTB WRITE: PC=0x8C001234 writing TTB=0x8C100000 (was 0x00000000)
🎯 TTB NON-ZERO: Windows CE setting up page tables at 0x8C100000  
✅ TTB VALID: Page table in system RAM range
🔍 TLB LOOKUP: VA=0x02010008 PC=0x8C0215F4
🔧 WINCE DEBUG: page_group=0x12345678  
✅ WINCE SUCCESS: Resolved VA=0x02010008 to PA=0x8C010008
```

**Result**: Windows CE boots successfully past BIOS screen

## Current Status

✅ **Build Complete**: TTB register debug logging implemented  
🔧 **Binary Ready**: `../build_for_tests/Flycast.app/Contents/MacOS/Flycast`  
📊 **Next Step**: Test with Windows CE game to determine which case occurs 