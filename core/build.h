#pragma once
//#define STRICT_MODE
#ifndef STRICT_MODE
#define FAST_MMU
#define USE_WINCE_HACK
#endif

#define DC_PLATFORM_DREAMCAST   0
#define DC_PLATFORM_DEV_UNIT    1
#define DC_PLATFORM_NAOMI       2
#define DC_PLATFORM_NAOMI2      3
#define DC_PLATFORM_ATOMISWAVE  4
#define DC_PLATFORM_SYSTEMSP    5

//HOST_CPU
#define CPU_X86      0x20000001
#define CPU_ARM      0x20000002
#define CPU_ARM64    0x20000003
#define CPU_X64      0x20000004

//FEAT_SHREC, FEAT_AREC, FEAT_DSPREC
#define DYNAREC_NONE	0x40000001
#define DYNAREC_JIT		0x40000002

//automatic

#if defined(__x86_64__) || defined(_M_X64)
	#define HOST_CPU CPU_X64
#elif defined(__i386__) || defined(_M_IX86)
	#define HOST_CPU CPU_X86
#elif defined(__arm__) || defined (_M_ARM)
	#define HOST_CPU CPU_ARM
#elif defined(__aarch64__) || defined(_M_ARM64)
	#define HOST_CPU CPU_ARM64
#else
	#error Unsupported architecture
#endif

// --- Start of new recompiler feature flag logic ---

// 1. Default all recompiler features to NONE initially
#define FEAT_SHREC DYNAREC_NONE
#define FEAT_AREC DYNAREC_NONE
#define FEAT_DSPREC DYNAREC_NONE

// 2. Enable features if corresponding CMake compile definitions are set
// JIT recompilers are forcefully disabled here as requested to ensure the
// SH4 IR interpreter is used for testing. This prevents crashes on platforms
// that lack JIT entitlements, such as macOS.

// --- End of new recompiler feature flag logic ---

// Platform-specific overrides. For example, iOS simulator always has them off.
#if defined(__APPLE__)
#include "TargetConditionals.h"
#if TARGET_OS_SIMULATOR
    #undef FEAT_SHREC
    #define FEAT_SHREC DYNAREC_NONE
    #undef FEAT_AREC
    #define FEAT_AREC DYNAREC_NONE
    #undef FEAT_DSPREC
    #define FEAT_DSPREC DYNAREC_NONE
#endif
// Add other specific platform overrides here if truly necessary,
// e.g. if a platform CANNOT support a JIT even if CMake tried to enable it.
#endif

#ifdef __SWITCH__
#define FEAT_NO_RWX_PAGES
#endif

// Some restrictions on FEAT_NO_RWX_PAGES
#if defined(FEAT_NO_RWX_PAGES) && FEAT_SHREC == DYNAREC_JIT
#if HOST_CPU != CPU_X64 && HOST_CPU != CPU_ARM64
#error "FEAT_NO_RWX_PAGES Only implemented for X64 and ARMv8"
#endif
#endif

#ifdef _WIN32
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
#define TARGET_UWP
#endif
#ifdef HAVE_D3D11
#define USE_DX11
#endif
#endif

#if !defined(LIBRETRO)
#define USE_GGPO
#endif

#if !defined(__ANDROID__) && !defined(TARGET_IPHONE) && !defined(TARGET_UWP) \
	&& !defined(__SWITCH__) && !defined(LIBRETRO) && !defined(__NetBSD__) && !defined(__OpenBSD__)
#define NAOMI_MULTIBOARD
#endif

// TARGET PLATFORM
#define GD_CLOCK 33868800						//GDROM XTAL -- 768fs
#define AICA_CORE_CLOCK (GD_CLOCK * 4 / 3)		//[45158400]  GD->PLL 3:4 -> AICA CORE	 -- 1024fs
#define AICA_ARM_CLOCK (AICA_CORE_CLOCK / 2)	//[22579200]  AICA CORE -> PLL 2:1 -> ARM
#define SH4_MAIN_CLOCK (200 * 1000 * 1000)		//[200000000] XTal(13.5) -> PLL (33.3) -> PLL 1:6 (200)
#define G2_BUS_CLOCK (25 * 1000 * 1000)			//[25000000]  from Holly, from SH4_RAM_CLOCK w/ 2 2:1 plls

#if defined(GLES) && !defined(GLES3) && !defined(GLES2)
// Only use GL ES 2.0 API functions
#define GLES2
#endif
