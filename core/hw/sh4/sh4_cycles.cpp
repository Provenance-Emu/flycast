/*
	Copyright 2023 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "sh4_cycles.h"
#include "oslib/oslib.h"

// === ENHANCED DYNAMIC CPU_RATIO SYSTEM ===
// Automatically adjusts CPU_RATIO based on frame sync delays with aggressive FMV detection:
// - CPU_RATIO = 1 for fast gameplay (when frames are on time)  
// - CPU_RATIO = 8-16 for smooth FMV (when frames are consistently slow)

struct DynamicCpuRatio {
    int current_ratio = 1;           // Start with fast gameplay ratio
    int slow_frame_count = 0;        // Count of consecutive slow frames
    int fast_frame_count = 0;        // Count of consecutive fast frames
    u64 last_frame_time = 0;         // Last frame timestamp
    
    // Enhanced thresholds for better FMV detection
    static constexpr int SLOW_FRAMES_TO_FMV = 4;      // Only 4 slow frames = switch to FMV mode
    static constexpr int FAST_FRAMES_TO_GAME = 8;     // 8 fast frames = switch to gameplay mode
    static constexpr u64 SLOW_FRAME_THRESHOLD = 22;   // >22ms = slow frame (more sensitive)
    static constexpr u64 FAST_FRAME_THRESHOLD = 18;   // <18ms = fast frame (more sensitive)
    static constexpr u64 VERY_SLOW_THRESHOLD = 35;    // >35ms = very slow frame (needs higher ratio)
    static constexpr u64 EXTREMELY_SLOW_THRESHOLD = 50; // >50ms = extremely slow frame (max ratio)
    
    int getCpuRatio() {
        u64 current_time = getTimeMs();
        
        if (last_frame_time > 0) {
            u64 frame_duration = current_time - last_frame_time;
            
            if (frame_duration > SLOW_FRAME_THRESHOLD) {
                // Slow frame detected
                slow_frame_count++;
                fast_frame_count = 0;
                
                // Immediate response for extremely slow frames (don't wait for 4 frames)
                if (frame_duration > EXTREMELY_SLOW_THRESHOLD && current_ratio < 16) {
                    current_ratio = 16;  // Immediate maximum ratio
                    INFO_LOG(INTERPRETER, "🚨 ENHANCED DYNAMIC CPU_RATIO: IMMEDIATE FMV mode CPU_RATIO = 16 (extremely slow frame: %llums)", frame_duration);
                }
                // Gradual escalation if already in FMV mode but still slow
                else if (current_ratio > 1 && slow_frame_count >= 2) {
                    int new_ratio = current_ratio;
                    
                    // Escalate based on frame severity
                    if (frame_duration > EXTREMELY_SLOW_THRESHOLD && current_ratio < 16) {
                        new_ratio = 16;
                    } else if (frame_duration > VERY_SLOW_THRESHOLD && current_ratio < 12) {
                        new_ratio = 12;
                    } else if (current_ratio < 8) {
                        new_ratio = 8;
                    }
                    
                    if (new_ratio > current_ratio) {
                        current_ratio = new_ratio;
                        INFO_LOG(INTERPRETER, "📈 ENHANCED DYNAMIC CPU_RATIO: ESCALATED to CPU_RATIO = %d (frame: %llums, still slow)", 
                                current_ratio, frame_duration);
                    }
                }
                // Switch to FMV mode faster and with adaptive ratios
                else if (slow_frame_count >= SLOW_FRAMES_TO_FMV) {
                    int new_ratio;
                    
                    // Adaptive CPU_RATIO based on frame severity
                    if (frame_duration > EXTREMELY_SLOW_THRESHOLD) {
                        new_ratio = 16;  // Maximum ratio for extremely slow frames
                    } else if (frame_duration > VERY_SLOW_THRESHOLD) {
                        new_ratio = 12;  // High ratio for very slow frames
                    } else {
                        new_ratio = 8;   // Standard FMV ratio
                    }
                    
                    if (current_ratio != new_ratio) {
                        current_ratio = new_ratio;
                        INFO_LOG(INTERPRETER, "🎬 ENHANCED DYNAMIC CPU_RATIO: FMV mode CPU_RATIO = %d (frame: %llums, %d slow frames)", 
                                current_ratio, frame_duration, slow_frame_count);
                    }
                }
            } else if (frame_duration < FAST_FRAME_THRESHOLD) {
                // Fast frame detected  
                fast_frame_count++;
                slow_frame_count = 0;
                
                if (fast_frame_count >= FAST_FRAMES_TO_GAME && current_ratio > 1) {
                    current_ratio = 1;  // Switch to gameplay mode
                    INFO_LOG(INTERPRETER, "🎮 ENHANCED DYNAMIC CPU_RATIO: Gameplay mode CPU_RATIO = 1 (frame: %llums, %d fast frames)", 
                            frame_duration, fast_frame_count);
                }
            } else {
                // Neutral frame - don't reset counters completely, just reduce them
                if (slow_frame_count > 0) slow_frame_count--;
                if (fast_frame_count > 0) fast_frame_count--;
            }
        }
        
        last_frame_time = current_time;
        return current_ratio;
    }
};

static DynamicCpuRatio g_dynamic_cpu_ratio;

// Get current dynamic CPU ratio
int getDynamicCpuRatio() {
    return g_dynamic_cpu_ratio.getCpuRatio();
}

#ifdef STRICT_MODE
constexpr int CPU_RATIO = 1;
#else
constexpr int CPU_RATIO = 1;  // REMOVED BOTTLENECK: Set to 1 for maximum speed!
#endif

Sh4Cycles sh4cycles(1);  // Will be multiplied by dynamic ratio

// TODO additional wait cycles depending on area?:
// Area       Wait cycles (not including external wait)
// 0          3
// 1 VRAM     3
// 2 reserved 3
// 3 SDRAM    0         CAS latency 3
// 4 TA,YUV   1
// 5 G2 ext   3
// 6 reserved 3

int Sh4Cycles::readExternalAccessCycles(u32 addr, u32 size)
{
	if ((addr & 0xfc000000) == 0xe0000000)
		// store queues
		return 0;

	addr &= 0x1fffffff;
	switch (addr >> 26)
	{
	case 0:
		if (!settings.platform.isAtomiswave())
		{
			// Dreamcast, Naomi
			if (addr < 0x00200000)
			{
				// system rom
				switch (size)
				{
				case 1:
					return 44;
				case 2:
					return 63;
				case 4:
					return 99;
				case 32:
				default:
					return 618;
				}
			}
			if (addr < 0x00200000 + settings.platform.flash_size)
			{
				// flash
				switch (size)
				{
				case 1:
					return 41;
				case 2:
					return 55;
				case 4:
					return 83;
				case 32:
				default:
					return 489;
				}
			}
		}
		else
		{
			// Atomiswave
			if (addr < 0x00020000 || (addr >= 0x00200000 && addr < 0x00200000 + settings.platform.flash_size))
			{
				// flash
				switch (size)
				{
				case 1:
					return 41;
				case 2:
					return 55;
				case 4:
					return 83;
				case 32:
				default:
					return 489;
				}
			}
		}
		addr &= 0x01ffffff;
		if (addr >= 0x005f6800 && addr <= 0x005f69ff)
		{
			// holly system control regs
			if (size != 4)
				INFO_LOG(SH4, "holly system reg: Invalid read size %d @ %07x", size, addr);
			return 5;
		}
		if (addr >= 0x005f6c00 && addr <= 0x005f6cff)
		{
			// maple regs
			if (size != 4)
				INFO_LOG(SH4, "maple reg: Invalid read size %d @ %07x", size, addr);
			return 22;
		}
		if (addr >= 0x005f7000 && addr <= 0x005f70ff)
		{
			if (settings.platform.isArcade())
				// naomi/aw cart
				return 20; // ???
			else
			{
				// gd-rom
				if (size > 2)
					INFO_LOG(SH4, "gd-rom: Invalid read size %d @ %07x", size, addr);
				return 39;
			}
		}
		if (addr >= 0x005f7400 && addr <= 0x005f74ff)
		{
			// G1 I/F control regs
			if (settings.platform.isConsole())
			{
				if (size != 4) // unknown for aw/naomi
					INFO_LOG(SH4, "G1 I/F: Invalid read size %d @ %07x", size, addr);
			}
			else
			{
				// unknown for aw/naomi. seeing size 1 and 4 at least
			}
			return 24;
		}
		if (addr >= 0x005f7800 && addr <= 0x005f78ff)
		{
			// G2 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G2 I/F: Invalid read size %d @ %07x", size, addr);
			return 38;
		}
		if (addr >= 0x005f7c00 && addr <= 0x005f7cff)
		{
			// PVR I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "PVR I/F: Invalid read size %d @ %07x", size, addr);
			return 24;
		}
		if (addr >= 0x005f8000 && addr <= 0x005f9fff)
		{
			// TA/PVR core control regs, Palette RAM, fog table
			if (size != 4)
				// TODO 32-byte access allowed for palette and fog tables?
				INFO_LOG(SH4, "PVR/TA core: Invalid read size %d @ %07x", size, addr);
			return 34;
		}
		if (addr >= 0x00600000 && addr <= 0x006007ff)
		{
			if (settings.platform.isConsole())
				// AW registers
				return 20; // ???
			else
			{
				// modem
				if (size != 1)
					INFO_LOG(SH4, "modem: Invalid read size %d @ %07x", size, addr);
				return 67;
			}
		}
		if (addr >= 0x00700000 && addr <= 0x00ffffff)
		{
			// aica regs and ram
			if (size < 4)
				INFO_LOG(SH4, "aica: Invalid read size %d @ %07x", size, addr);
			return 40 * size / 4;	// undocumented
		}
		if (addr >= 0x01000000 && addr <= 0x01ffffff)
		{
			// G2 external area
			switch (size)
			{
			case 1:
			case 2:
				return 56;
			case 4:
				return 60;
			case 32:
			default:
				return 84;
			}
		}
		break;

	case 1:
		// VRAM
		switch (size)
		{
		case 1:
			INFO_LOG(SH4, "vram: Invalid read size 1 @ %07x", addr);
			return 41;
		case 2:
		case 4:
			return 41;
		case 32:
		default:
			return 61;
		}

	case 2:
		// Area 2
		INFO_LOG(SH4, "Invalid read from area 2 @ %07x", addr);
		return 60;

	case 3:
		// System RAM
		return 7;	// or 12 if row miss TODO average?

	case 4:
		// TA FIFO
		if (size != 32)
			INFO_LOG(SH4, "Invalid read size %d from area 4 (TA FIFO) @ %07x", size, addr);
		if ((addr >= 0x11000000 && addr <= 0x11ffffff) || (addr >= 0x13000000 && addr <= 0x13ffffff))
			// VRAM (64 bits)
			return 61;	// undocumented
		break;

	case 5:
		// Ext device
		switch (size)
		{
		case 1:
		case 2:
			return 56;
		case 4:
			return 60;
		case 32:
		default:
			return 84;
		}

	case 6:
		// Area 6
		INFO_LOG(SH4, "Invalid read from area 6 @ %07x", addr);
		return 60;

	case 7:
		// SH4 registers
		return 0;
	}

	INFO_LOG(SH4, "Unmapped read @ %08x", addr);
	return 60;
}


int Sh4Cycles::writeExternalAccessCycles(u32 addr, u32 size)
{
	if ((addr & 0xfc000000) == 0xe0000000)
		// store queues
		return 0;

	addr &= 0x1fffffff;
	switch (addr >> 26)
	{
	case 0:
		if (!settings.platform.isAtomiswave())
		{
			if (addr < 0x00200000)
			{
				// system rom
				INFO_LOG(SH4, "Invalid write to rom @ %07x", addr);
				return 99;
			}
			if (addr < 0x00200000 + settings.platform.flash_size)
			{
				// flash
				if (size != 1)
					INFO_LOG(SH4, "flashrom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
		}
		else
		{
			if (addr < 0x00020000)
			{
				// flash
				if (size != 1)
					INFO_LOG(SH4, "flashrom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
			if (addr >= 0x00200000 && addr < 0x00200000 + settings.platform.flash_size)
			{
				// nvmem
				return 14; // ????
			}
		}
		addr &= 0x01ffffff;
		if (addr >= 0x005f6800 && addr <= 0x005f69ff)
		{
			// holly system control regs
			if (size != 4)
				INFO_LOG(SH4, "holly system reg: Invalid write size %d @ %07x", size, addr);
			return 5;
		}
		if (addr >= 0x005f6c00 && addr <= 0x005f6cff)
		{
			// maple regs
			if (size != 4)
				INFO_LOG(SH4, "maple reg: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7000 && addr <= 0x005f70ff)
		{
			if (settings.platform.isArcade())
				// naomi/aw cart
				return 14; // ???
			else
			{
				// gd-rom
				if (size > 2)
					INFO_LOG(SH4, "gd-rom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
		}
		if (addr >= 0x005f7400 && addr <= 0x005f74ff)
		{
			// G1 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G1 I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7800 && addr <= 0x005f78ff)
		{
			// G2 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G2 I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7c00 && addr <= 0x005f7cff)
		{
			// PVR I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "PVR I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f8000 && addr <= 0x005f9fff)
		{
			// TA/PVR core control regs, Palette RAM, fog table
			if (size != 4)
				// TODO 32-byte access allowed for palette and fog tables?
				INFO_LOG(SH4, "PVR/TA core: Invalid write size %d @ %07x", size, addr);
			return 14;
		}
		if (addr >= 0x00600000 && addr <= 0x006007ff)
		{
			if (settings.platform.isAtomiswave())
				// AW registers
				return 14; // ???
			else
			{
				// modem
				if (size != 1)
					INFO_LOG(SH4, "modem: Invalid write size %d @ %07x", size, addr);
				return 44;
			}
		}
		if (addr >= 0x00700000 && addr <= 0x00ffffff)
		{
			// aica regs and ram
			if (size < 4)
				INFO_LOG(SH4, "aica: Invalid read size %d @ %07x", size, addr);
			return 12 * size / 4;	// undocumented
		}
		if (addr >= 0x01000000 && addr <= 0x01ffffff)
		{
			// G2 external area
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return 28;
			case 32:
			default:
				return 52;
			}
		}
		break;

	case 1:
		// VRAM
		switch (size)
		{
		case 1:
			INFO_LOG(SH4, "vram: Invalid write size 1 @ %07x", addr);
			return 12;
		case 2:
		case 4:
			return 12;
		case 32:
		default:
			return 38;
		}

	case 2:
		// Area 2
		INFO_LOG(SH4, "Invalid read to area 2 @ %07x", addr);
		return 12;

	case 3:
		// System RAM
		return 4;	// or 9 if row miss TODO average?

	case 4:
		// TA FIFO
		if (size != 32)
			INFO_LOG(SH4, "Invalid write size %d to area 4 (TA FIFO) @ %07x", size, addr);
		if ((addr >= 0x10000000 && addr <= 0x107fffff) || (addr >= 0x12000000 && addr <= 0x127fffff))
			// TA polygon data
			return 7;	// undocumented
		if ((addr >= 0x10800000 && addr <= 0x10ffffff) || (addr >= 0x12800000 && addr <= 0x12ffffff))
			// YUV converter
			return 9;	// 858 cycles for 3072 bytes (YUV420)
		if ((addr >= 0x11000000 && addr <= 0x11ffffff) || (addr >= 0x13000000 && addr <= 0x13ffffff))
			// VRAM (64 bits)
			return 5;	// 8 for 32-bit access (LMMODE0/1)
		break;

	case 5:
		// Ext device
		switch (size)
		{
		case 1:
		case 2:
		case 4:
			return 28;
		case 32:
		default:
			return 52;
		}

	case 6:
		// Area 6
		INFO_LOG(SH4, "Invalid write to area 6 @ %07x", addr);
		return 14;

	case 7:
		// SH4 registers
		return 0;
	}

	INFO_LOG(SH4, "Unmapped read @ %08x", addr);
	return 14;
}
