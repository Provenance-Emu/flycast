#pragma once
#include "types.h"
#include "hw/aica/aica_if.h"

namespace aica::arm
{

template <typename T> T readReg(u32 addr);
template <typename T> void writeReg(u32 addr, T data);

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
// Optimized memory access for ARM64 processors
template<typename T>
static inline T DYNACALL readMem(u32 addr)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
	{
		addr &= (ARAM_MASK - (sizeof(T) - 1));
		T rv;

		// Use direct assembly for faster memory access
		if (sizeof(T) == 4) {
			__asm__ volatile(
				"ldr %w[result], [%[address]]\n"
				: [result] "=r" (rv)
				: [address] "r" (&aica_ram[addr])
				: "memory"
			);
		}
		else if (sizeof(T) == 2) {
			__asm__ volatile(
				"ldrh %w[result], [%[address]]\n"
				: [result] "=r" (rv)
				: [address] "r" (&aica_ram[addr])
				: "memory"
			);
		}
		else {
			__asm__ volatile(
				"ldrb %w[result], [%[address]]\n"
				: [result] "=r" (rv)
				: [address] "r" (&aica_ram[addr])
				: "memory"
			);
		}

		if (unlikely(sizeof(T) == 4 && (addr & 3) != 0))
		{
			u32 sf = (addr & 3) * 8;
			return (rv >> sf) | (rv << (32 - sf));
		}
		else
			return rv;
	}
	else
	{
		return readReg<T>(addr);
	}
}

template<typename T>
static inline void DYNACALL writeMem(u32 addr, T data)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
	{
		addr &= (ARAM_MASK - (sizeof(T) - 1));

		// Use direct assembly for faster memory access
		if (sizeof(T) == 4) {
			__asm__ volatile(
				"str %w[value], [%[address]]\n"
				:
				: [value] "r" (data), [address] "r" (&aica_ram[addr])
				: "memory"
			);
		}
		else if (sizeof(T) == 2) {
			__asm__ volatile(
				"strh %w[value], [%[address]]\n"
				:
				: [value] "r" (data), [address] "r" (&aica_ram[addr])
				: "memory"
			);
		}
		else {
			__asm__ volatile(
				"strb %w[value], [%[address]]\n"
				:
				: [value] "r" (data), [address] "r" (&aica_ram[addr])
				: "memory"
			);
		}
	}
	else
	{
		writeReg(addr, data);
	}
}
#else
// Original implementation
template<typename T>
static inline T DYNACALL readMem(u32 addr)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
	{
		T rv = *(T *)&aica_ram[addr & (ARAM_MASK - (sizeof(T) - 1))];

		if (unlikely(sizeof(T) == 4 && (addr & 3) != 0))
		{
			u32 sf = (addr & 3) * 8;
			return (rv >> sf) | (rv << (32 - sf));
		}
		else
			return rv;
	}
	else
	{
		return readReg<T>(addr);
	}
}

template<typename T>
static inline void DYNACALL writeMem(u32 addr, T data)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
	{
		*(T *)&aica_ram[addr & (ARAM_MASK - (sizeof(T) - 1))] = data;
	}
	else
	{
		writeReg(addr, data);
	}
}
#endif

extern bool aica_interr;
extern u32 aica_reg_L;
extern bool e68k_out;
extern u32 e68k_reg_L;
extern u32 e68k_reg_M;

void update_armintc();
void interruptChange(u32 bits, u32 L);

} // namespace aica::arm
