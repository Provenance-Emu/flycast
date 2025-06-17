#if !defined(__ANDROID__) && !defined(__APPLE__)
#include <cstdlib>

[[noreturn]] void os_DebugBreak()
{
	std::abort();
}

void os_DoEvents()
{
}

void os_RunInstance(int argc, const char *argv[])
{
}

#ifdef _WIN32
void os_SetThreadName(const char *name)
{
}
#endif
#endif

// Conditional compilation:// This file contains stub implementations for functions that are normally
// provided by the main application or other libraries, but are needed for tests.

#include "types.h"

// Input global variables (stubs for testing)
// extern "C" {
//     int _joyx = 0;
//     int _joyy = 0;
//     int _joyrl = 0;
//     int _joyrr = 0;
//     int _joyrx = 0;
//     int _joyry = 0;
//     int _joy3x = 0;
//     int _joy3y = 0;
//     unsigned char _kcode[256] = {0};
//     unsigned short _kb_key = 0;
//     unsigned char _kb_shift = 0;
//     int _lt = 0;
//     int _rt = 0;
//     int _lt2 = 0;
//     int _rt2 = 0;
//     unsigned int _mo_buttons = 0;
//     int _mo_wheel_delta = 0;
//     int _mo_x_abs = 0;
//     int _mo_y_abs = 0;
//     int _mo_x_delta = 0;
//     int _mo_y_delta = 0;
//     // Other potentially missing globals from linker errors
//     unsigned char _vmu_lcd_status[4*48*32] = {0};
//     char _subfolders_read[1024*10] = {0};
//     // These network related ones might be better handled by FEAT_NO_NETWORKING stubs if they persist
//     // For now, providing a basic definition.
//     void* _naomiNetwork = nullptr;
//     void (*_networkOutput)(int, unsigned char*, int) = nullptr;
//     // For _ip_meta, its type is unknown, using void* as a placeholder
//     void* _ip_meta = nullptr;
//     // For _relPosMutex, assuming it's a pointer, possibly to a mutex type. Using void*.
//     void* _relPosMutex = nullptr;
// }

#if defined(FLYCAST_TEST_BUILD) && defined(FEAT_NO_NETWORKING)

// Stubs for Modem memory access functions
// (Normally defined in core/hw/modem/modem.cpp)
// Signatures from core/hw/modem/modem.h

/**
 * @brief Stub for ModemReadMem_A0_006
 * Called from core/hw/holly/sb_mem.cpp
 */
extern "C" u32 ModemReadMem_A0_006(u32 /*addr*/, u32 /*size*/) {
    // In a test build without networking, this shouldn't be critical.
    // Return a default value. Logging can be added if specific test behavior is desired.
    // printf("[STUB] ModemReadMem_A0_006 called\n");
    return 0;
}

/**
 * @brief Stub for ModemWriteMem_A0_006
 * Called from core/hw/holly/sb_mem.cpp
 */
extern "C" void ModemWriteMem_A0_006(u32 /*addr*/, u32 /*data*/, u32 /*size*/) {
    // In a test build without networking, this is likely a no-op.
    // Logging can be added if specific test behavior is desired.
    // printf("[STUB] ModemWriteMem_A0_006 called\n");
}

#endif // FLYCAST_TEST_BUILD && FEAT_NO_NETWORKING
