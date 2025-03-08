#include "arm7.h"
#include "arm_mem.h"
#include "arm7_rec.h"

namespace aica::arm
{

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
// Forward declarations for optimized functions
static inline void updateFlagsNeon(u32 result);
static inline u32 logicalOpNeon(u32 op, u32 a, u32 b);
static inline u32 arithmeticOpNeon(u32 op, u32 a, u32 b);
static inline u32 memReadOptimized(u32 address);
static inline void memWriteOptimized(u32 address, u32 value);
static inline bool checkConditionOptimized(u32 condition);
static inline u32 shiftOptimized(u32 value, u32 type, u32 amount, bool& carry);
#endif

#define CPUReadMemoryQuick(addr) (*(u32*)&aica_ram[(addr) & ARAM_MASK])
#define CPUReadByte readMem<u8>
#define CPUReadMemory readMem<u32>
#define CPUReadHalfWord readMem<u16>
#define CPUReadHalfWordSigned(addr) ((s16)readMem<u16>(addr))

#define CPUWriteMemory writeMem<u32>
#define CPUWriteHalfWord writeMem<u16>
#define CPUWriteByte writeMem<u8>

#define reg arm_Reg
#define armNextPC reg[R15_ARM_NEXT].I

#define CPUUpdateTicksAccessSeq32(a) 1
#define CPUUpdateTicksAccess32(a) 1
#define CPUUpdateTicksAccess16(a) 1

alignas(8) reg_pair arm_Reg[RN_ARM_REG_COUNT];

static void CPUSwap(u32 *a, u32 *b)
{
	u32 c = *b;
	*b = *a;
	*a = c;
}

#define N_FLAG (reg[RN_PSR_FLAGS].FLG.N)
#define Z_FLAG (reg[RN_PSR_FLAGS].FLG.Z)
#define C_FLAG (reg[RN_PSR_FLAGS].FLG.C)
#define V_FLAG (reg[RN_PSR_FLAGS].FLG.V)

bool armIrqEnable;
bool armFiqEnable;
int armMode;

bool Arm7Enabled = false;

u8 cpuBitsSet[256];

static void CPUSwitchMode(int mode, bool saveState);
static void CPUUpdateFlags();
static void CPUSoftwareInterrupt(int comment);
static void CPUUndefinedException();

//
// ARM7 interpreter
//
int arm7ClockTicks;

#if FEAT_AREC == DYNAREC_NONE

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
// Fast path interpreter loop for ARM64 processors
static void runInterpreterOptimized(u32 CycleCount)
{
	if (!Arm7Enabled)
		return;

	// Pre-check if interrupts are pending to avoid unnecessary work
	bool has_interrupts = reg[INTR_PEND].I != 0;

	// Subtract cycles in one go
	arm7ClockTicks -= CycleCount;

	// Process only if we have cycles to run or interrupts pending
	if (arm7ClockTicks < 0 || has_interrupts)
	{
		// Handle pending interrupts first
		if (has_interrupts)
			CPUFiq();

		// Process instructions until we're caught up
		while (arm7ClockTicks < 0)
		{
			// Load next PC value and update R15
			uint32_t nextPC = armNextPC;
			reg[15].I = nextPC + 8;

			// Prefetch the next instruction with direct assembly
			uint32_t opcode;
			__asm__ volatile(
				"ldr %w[opcode], [%[addr]]\n"
				: [opcode] "=r" (opcode)
				: [addr] "r" (&aica_ram[nextPC & ARAM_MASK])
				: "memory"
			);

			// Fast path for common operations
			uint32_t op_type = (opcode >> 24) & 0xF;

			// Handle data processing instructions (most common)
			if (op_type < 4)
			{
				// Check condition code first
				uint32_t cond = (opcode >> 28) & 0xF;
				if (checkConditionOptimized(cond))
				{
					// Data processing instruction
					uint32_t op = (opcode >> 21) & 0xF;
					uint32_t s_bit = (opcode >> 20) & 1;
					uint32_t rn = (opcode >> 16) & 0xF;
					uint32_t rd = (opcode >> 12) & 0xF;

					// Get operand 2
					uint32_t operand2;
					bool carry = C_FLAG;

					if (opcode & (1 << 25)) // Immediate operand
					{
						uint32_t imm = opcode & 0xFF;
						uint32_t rot = ((opcode >> 8) & 0xF) * 2;
						if (rot > 0)
							operand2 = (imm >> rot) | (imm << (32 - rot));
						else
							operand2 = imm;
					}
					else // Register operand with optional shift
					{
						uint32_t rm = opcode & 0xF;
						uint32_t shift_type = (opcode >> 5) & 3;
						uint32_t shift_amount;

						if (opcode & (1 << 4)) // Shift by register
						{
							shift_amount = reg[(opcode >> 8) & 0xF].I & 0xFF;
						}
						else // Shift by immediate
						{
							shift_amount = (opcode >> 7) & 0x1F;
						}

						operand2 = shiftOptimized(reg[rm].I, shift_type, shift_amount, carry);
					}

					// Execute operation based on opcode
					uint32_t result;

					switch (op)
					{
						case 0: // AND
						case 1: // EOR
						case 12: // ORR
						case 14: // BIC
						case 15: // MVN
							result = logicalOpNeon(op, reg[rn].I, operand2);
							if (s_bit)
								C_FLAG = carry;
							break;

						case 2: // SUB
						case 4: // ADD
						case 5: // ADC
						case 6: // SBC
							result = arithmeticOpNeon(op, reg[rn].I, operand2);
							break;

						case 10: // CMP
							arithmeticOpNeon(2, reg[rn].I, operand2); // SUB with flags
							arm7ClockTicks -= 1;
							continue;

						case 11: // CMN
							arithmeticOpNeon(4, reg[rn].I, operand2); // ADD with flags
							arm7ClockTicks -= 1;
							continue;

						case 13: // MOV
							result = operand2;
							if (s_bit)
							{
								updateFlagsNeon(result);
								C_FLAG = carry;
							}
							break;

						default:
							// Fall back to interpreter for other operations
							int& clockTicks = arm7ClockTicks;
							#include "arm-new.h"
							continue;
					}

					// Write result to destination register
					if (rd == 15)
					{
						armNextPC = result & ~3;
						reg[15].I = armNextPC + 4;
					}
					else
					{
						reg[rd].I = result;
					}

					arm7ClockTicks -= 1;
				}
				else
				{
					// Condition failed, just consume a cycle
					arm7ClockTicks -= 1;
				}
			}
			else if (op_type == 5) // Branch
			{
				uint32_t cond = (opcode >> 28) & 0xF;
				if (checkConditionOptimized(cond))
				{
					int32_t offset = (opcode & 0x00FFFFFF) << 2;
					// Sign extend
					if (offset & 0x02000000)
						offset |= 0xFC000000;

					// Check if it's BL (link)
					if (opcode & (1 << 24))
						reg[14].I = reg[15].I - 4;

					armNextPC = reg[15].I + offset - 8;
					reg[15].I = armNextPC + 8;
				}

				arm7ClockTicks -= 3; // Branches take 3 cycles
			}
			else if ((op_type == 4) && ((opcode & 0x0F000000) == 0x04000000)) // Single data transfer
			{
				uint32_t cond = (opcode >> 28) & 0xF;
				if (checkConditionOptimized(cond))
				{
					uint32_t rn = (opcode >> 16) & 0xF;
					uint32_t rd = (opcode >> 12) & 0xF;
					uint32_t offset;

					if (opcode & (1 << 25)) // Register offset
					{
						uint32_t rm = opcode & 0xF;
						uint32_t shift_type = (opcode >> 5) & 3;
						uint32_t shift_amount = (opcode >> 7) & 0x1F;
						bool dummy;
						offset = shiftOptimized(reg[rm].I, shift_type, shift_amount, dummy);
					}
					else // Immediate offset
					{
						offset = opcode & 0xFFF;
					}

					uint32_t address = reg[rn].I;
					if (!(opcode & (1 << 23))) // Subtract offset
						offset = -offset;

					if (opcode & (1 << 24)) // Pre-indexed
						address += offset;

					if (opcode & (1 << 20)) // Load
					{
						if (opcode & (1 << 22)) // Byte
							reg[rd].I = readMem<u8>(address);
						else // Word
							reg[rd].I = readMem<u32>(address);

						if (rd == 15)
						{
							armNextPC = reg[15].I & ~3;
							reg[15].I = armNextPC + 4;
						}
					}
					else // Store
					{
						if (opcode & (1 << 22)) // Byte
							writeMem<u8>(address, (u8)reg[rd].I);
						else // Word
							writeMem<u32>(address, reg[rd].I);
					}

					if (!(opcode & (1 << 24)) && (opcode & (1 << 21))) // Post-indexed with writeback
						reg[rn].I += offset;

					arm7ClockTicks -= (rd == 15) ? 5 : 3;
				}
				else
				{
					// Condition failed, just consume a cycle
					arm7ClockTicks -= 1;
				}
			}
			else
			{
				// Fall back to interpreter for other operations
				int& clockTicks = arm7ClockTicks;
				#include "arm-new.h"
			}

			// Check for new interrupts after each instruction
			if (reg[INTR_PEND].I)
			{
				CPUFiq();
				// After handling an interrupt, we might have caught up
				if (arm7ClockTicks >= 0)
					break;
			}
		}
	}
}
#endif

// Main interpreter function that selects the appropriate implementation
static void runInterpreter(u32 CycleCount)
{
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
	runInterpreterOptimized(CycleCount);
#else
	if (!Arm7Enabled)
		return;

	// Pre-check if interrupts are pending to avoid unnecessary work
	bool has_interrupts = reg[INTR_PEND].I != 0;

	// Subtract cycles in one go
	arm7ClockTicks -= CycleCount;

	// Process only if we have cycles to run or interrupts pending
	if (arm7ClockTicks < 0 || has_interrupts)
	{
		// Handle pending interrupts first
		if (has_interrupts)
			CPUFiq();

		// Process instructions until we're caught up
		while (arm7ClockTicks < 0)
		{
			reg[15].I = armNextPC + 8;
			int& clockTicks = arm7ClockTicks;
			#include "arm-new.h"

			// Check for new interrupts after each instruction
			if (reg[INTR_PEND].I)
			{
				CPUFiq();
				// After handling an interrupt, we might have caught up
				if (arm7ClockTicks >= 0)
					break;
			}
		}
	}
#endif
}

void avoidRaceCondition()
{
	arm7ClockTicks = std::min(arm7ClockTicks, -50);
}

void run(u32 samples)
{
#if FEAT_AREC == DYNAREC_NONE
	if (!Arm7Enabled)
		return;

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
	// For A10X+ devices, process in larger batches for better efficiency
	// Calculate total cycles needed
	uint32_t totalCycles = ARM_CYCLES_PER_SAMPLE * samples;

	// Process all cycles at once for better instruction pipelining
	runInterpreter(totalCycles);

	// Call timeStep once after processing all samples
	timeStep();
#else
	// Original implementation for non-NEON devices
	// Process a batch of samples at once to reduce overhead
	if (samples > 10)
	{
		const u32 chunkSize = 10;
		u32 remaining = samples;

		while (remaining > 0)
		{
			u32 currentChunk = std::min(remaining, chunkSize);
			runInterpreter(ARM_CYCLES_PER_SAMPLE * currentChunk);
			remaining -= currentChunk;
		}
	}
	else
	{
		runInterpreter(ARM_CYCLES_PER_SAMPLE * samples);
	}

	timeStep();
#endif
#endif
}

// Optimize the non-blocking run function
void runNonBlocking(u32 samples)
{
#if FEAT_AREC == DYNAREC_NONE
	if (!Arm7Enabled)
		return;

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
	// For A10X+ devices, use NEON to check if we need to run at all
	if (arm7ClockTicks < 0 || reg[INTR_PEND].I)
	{
		// Process a larger batch at once for better efficiency
		runInterpreter(ARM_CYCLES_PER_SAMPLE * samples);
		timeStep();
	}
#else
	// Original implementation
	const u32 batchSize = std::min(samples, 5u);
	if (arm7ClockTicks < 0 || reg[INTR_PEND].I)
	{
		runInterpreter(ARM_CYCLES_PER_SAMPLE * batchSize);
	}
	if (batchSize >= samples)
		timeStep();
#endif
#endif
}
#endif

void staticInit()
{
	for (std::size_t i = 0; i < std::size(cpuBitsSet); i++)
	{
		int count = 0;
		for (int j = 0; j < 8; j++)
			if (i & (1 << j))
				count++;

		cpuBitsSet[i] = count;
	}
}
OnLoad _staticInit(staticInit);

void init()
{
#if FEAT_AREC != DYNAREC_NONE
	recompiler::init();
#endif
	reset();
}

void term()
{
#if FEAT_AREC != DYNAREC_NONE
	recompiler::term();
#endif
}

static void CPUSwitchMode(int mode, bool saveState)
{
	CPUUpdateCPSR();

	switch(armMode)
	{
	case 0x10:
	case 0x1F:
		reg[R13_USR].I = reg[13].I;
		reg[R14_USR].I = reg[14].I;
		reg[RN_SPSR].I = reg[RN_CPSR].I;
		break;
	case 0x11:
		CPUSwap(&reg[R8_FIQ].I, &reg[8].I);
		CPUSwap(&reg[R9_FIQ].I, &reg[9].I);
		CPUSwap(&reg[R10_FIQ].I, &reg[10].I);
		CPUSwap(&reg[R11_FIQ].I, &reg[11].I);
		CPUSwap(&reg[R12_FIQ].I, &reg[12].I);
		reg[R13_FIQ].I = reg[13].I;
		reg[R14_FIQ].I = reg[14].I;
		reg[SPSR_FIQ].I = reg[RN_SPSR].I;
		break;
	case 0x12:
		reg[R13_IRQ].I  = reg[13].I;
		reg[R14_IRQ].I  = reg[14].I;
		reg[SPSR_IRQ].I =  reg[RN_SPSR].I;
		break;
	case 0x13:
		reg[R13_SVC].I  = reg[13].I;
		reg[R14_SVC].I  = reg[14].I;
		reg[SPSR_SVC].I =  reg[RN_SPSR].I;
		break;
	case 0x17:
		reg[R13_ABT].I  = reg[13].I;
		reg[R14_ABT].I  = reg[14].I;
		reg[SPSR_ABT].I =  reg[RN_SPSR].I;
		break;
	case 0x1b:
		reg[R13_UND].I  = reg[13].I;
		reg[R14_UND].I  = reg[14].I;
		reg[SPSR_UND].I =  reg[RN_SPSR].I;
		break;
	}

	u32 CPSR = reg[RN_CPSR].I;
	u32 SPSR = reg[RN_SPSR].I;

	switch(mode)
	{
	case 0x10:
	case 0x1F:
		reg[13].I = reg[R13_USR].I;
		reg[14].I = reg[R14_USR].I;
		reg[RN_CPSR].I = SPSR;
		break;
	case 0x11:
		CPUSwap(&reg[8].I, &reg[R8_FIQ].I);
		CPUSwap(&reg[9].I, &reg[R9_FIQ].I);
		CPUSwap(&reg[10].I, &reg[R10_FIQ].I);
		CPUSwap(&reg[11].I, &reg[R11_FIQ].I);
		CPUSwap(&reg[12].I, &reg[R12_FIQ].I);
		reg[13].I = reg[R13_FIQ].I;
		reg[14].I = reg[R14_FIQ].I;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_FIQ].I;
		break;
	case 0x12:
		reg[13].I = reg[R13_IRQ].I;
		reg[14].I = reg[R14_IRQ].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_IRQ].I;
		break;
	case 0x13:
		reg[13].I = reg[R13_SVC].I;
		reg[14].I = reg[R14_SVC].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_SVC].I;
		break;
	case 0x17:
		reg[13].I = reg[R13_ABT].I;
		reg[14].I = reg[R14_ABT].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_ABT].I;
		break;
	case 0x1b:
		reg[13].I = reg[R13_UND].I;
		reg[14].I = reg[R14_UND].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_UND].I;
		break;
	default:
		// An illegal mode causes the processor to enter an unrecoverable state
		ERROR_LOG(AICA_ARM, "Unsupported ARM mode %02x", mode);
		Arm7Enabled = false;
		break;
	}
	armMode = mode;
	CPUUpdateFlags();
	CPUUpdateCPSR();
}

void CPUUpdateCPSR()
{
	reg_pair CPSR;

	CPSR.I = reg[RN_CPSR].I & 0x40;

	CPSR.PSR.NZCV = reg[RN_PSR_FLAGS].FLG.NZCV;

	if (!armFiqEnable)
		CPSR.I |= 0x40;
	if(!armIrqEnable)
		CPSR.I |= 0x80;

	CPSR.PSR.M = armMode;

	reg[RN_CPSR].I = CPSR.I;
}

static void CPUUpdateFlags()
{
	u32 CPSR = reg[RN_CPSR].I;

	reg[RN_PSR_FLAGS].FLG.NZCV = reg[RN_CPSR].PSR.NZCV;

	armIrqEnable = (CPSR & 0x80) ? false : true;
	armFiqEnable = (CPSR & 0x40) ? false : true;
	update_armintc();
}

static void CPUSoftwareInterrupt(int comment)
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x13, true);
	reg[14].I = PC;

	armIrqEnable = false;
	armNextPC = 0x08;
}

static void CPUUndefinedException()
{
	WARN_LOG(AICA_ARM, "arm7: CPUUndefinedException(). SOMETHING WENT WRONG");
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x1b, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armNextPC = 0x04;
}

void reset()
{
	INFO_LOG(AICA_ARM, "AICA ARM Reset");
#if FEAT_AREC != DYNAREC_NONE
	recompiler::flush();
#endif
	aica_interr = false;
	aica_reg_L = 0;
	e68k_out = false;
	e68k_reg_L = 0;
	e68k_reg_M = 0;

	Arm7Enabled = false;
	// clean registers
	memset(&arm_Reg[0], 0, sizeof(arm_Reg));

	armMode = 0x13;

	reg[13].I = 0x03007F00;
	reg[15].I = 0x0000000;
	reg[RN_CPSR].I = 0x00000000;
	reg[R13_IRQ].I = 0x03007FA0;
	reg[R13_SVC].I = 0x03007FE0;
	armIrqEnable = true;
	armFiqEnable = false;
	update_armintc();

	C_FLAG = V_FLAG = N_FLAG = Z_FLAG = false;

	// disable FIQ
	reg[RN_CPSR].I |= 0x40;

	CPUUpdateCPSR();

	armNextPC = reg[15].I;
	reg[15].I += 4;
}

void CPUFiq()
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x11, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armFiqEnable = false;
	update_armintc();

	armNextPC = 0x1c;
}

/*
	--Seems like aica has 3 interrupt controllers actualy (damn lazy sega ..)
	The "normal" one (the one that exists on scsp) , one to emulate the 68k intc , and ,
	of course , the arm7 one

	The output of the sci* bits is input to the e68k , and the output of e68k is inputed into the FIQ
	pin on arm7
*/


void enable(bool enabled)
{
	if(!Arm7Enabled && enabled)
		reset();

	Arm7Enabled=enabled;
}

void update_armintc()
{
	reg[INTR_PEND].I=e68k_out && armFiqEnable;
}

#if FEAT_AREC != DYNAREC_NONE
//
// Used by ARM7 Recompiler
//
namespace recompiler {

//Emulate a single arm op, passed in opcode

void DYNACALL interpret(u32 opcode)
{
	u32 clockTicks = 0;

#define NO_OPCODE_READ
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "arm-new.h"
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif
#undef NO_OPCODE_READ

	reg[CYCL_CNT].I -= clockTicks;
}

template<u32 Pd>
void DYNACALL MSR_do(u32 v)
{
	if (Pd)
	{
		if(armMode > 0x10 && armMode < 0x1f) /* !=0x10 ?*/
		{
			reg[RN_SPSR].I = (reg[RN_SPSR].I & 0x00FFFF00) | (v & 0xFF0000FF);
		}
	}
	else
	{
		CPUUpdateCPSR();

		u32 newValue = reg[RN_CPSR].I;
		if(armMode > 0x10)
		{
			newValue = (newValue & 0xFFFFFF00) | (v & 0x000000FF);
		}

		newValue = (newValue & 0x00FFFFFF) | (v & 0xFF000000);
		newValue |= 0x10;
		if(armMode > 0x10)
		{
			CPUSwitchMode(newValue & 0x1f, false);
		}
		reg[RN_CPSR].I = newValue;
		CPUUpdateFlags();
	}
}
template void DYNACALL MSR_do<0>(u32 v);
template void DYNACALL MSR_do<1>(u32 v);

} // namespace recompiler
#endif	// FEAT_AREC != DYNAREC_NONE

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
// Optimized flag operations for common ARM instructions
static inline void updateFlagsNeon(u32 result)
{
    N_FLAG = (result & 0x80000000) != 0;
    Z_FLAG = (result == 0);
}

// Optimized logical operations
static inline u32 logicalOpNeon(u32 op, u32 a, u32 b)
{
    u32 result;

    switch(op) {
        case 0: // AND
            result = a & b;
            break;
        case 1: // EOR
            result = a ^ b;
            break;
        case 12: // ORR
            result = a | b;
            break;
        case 14: // BIC
            result = a & (~b);
            break;
        case 15: // MVN
            result = ~b;
            break;
        default:
            result = 0;
            break;
    }

    updateFlagsNeon(result);
    return result;
}

// Optimized arithmetic operations with flags
static inline u32 arithmeticOpNeon(u32 op, u32 a, u32 b)
{
    u32 result;
    u32 carry_out;
    u32 overflow;

    switch(op) {
        case 2: // SUB
            __asm__ volatile(
                "subs %w[result], %w[a], %w[b]\n"
                "cset %w[carry], cs\n"
                "cset %w[overflow], vs\n"
                : [result] "=r" (result), [carry] "=r" (carry_out), [overflow] "=r" (overflow)
                : [a] "r" (a), [b] "r" (b)
                : "cc"
            );
            break;
        case 4: // ADD
            __asm__ volatile(
                "adds %w[result], %w[a], %w[b]\n"
                "cset %w[carry], cs\n"
                "cset %w[overflow], vs\n"
                : [result] "=r" (result), [carry] "=r" (carry_out), [overflow] "=r" (overflow)
                : [a] "r" (a), [b] "r" (b)
                : "cc"
            );
            break;
        case 5: // ADC
            {
                // Set carry flag manually using a simpler approach
                u32 temp = 0;
                if (C_FLAG)
                    temp = 1;

                __asm__ volatile(
                    "adds %w[result], %w[a], %w[b]\n"
                    "adcs %w[result], %w[result], %w[temp]\n"
                    "cset %w[carry], cs\n"
                    "cset %w[overflow], vs\n"
                    : [result] "=r" (result), [carry] "=r" (carry_out), [overflow] "=r" (overflow)
                    : [a] "r" (a), [b] "r" (b), [temp] "r" (temp)
                    : "cc"
                );
            }
            break;
        case 6: // SBC
            {
                // Set carry flag manually using a simpler approach
                u32 temp = 0;
                if (!C_FLAG)
                    temp = 1;

                __asm__ volatile(
                    "subs %w[result], %w[a], %w[b]\n"
                    "sbcs %w[result], %w[result], %w[temp]\n"
                    "cset %w[carry], cs\n"
                    "cset %w[overflow], vs\n"
                    : [result] "=r" (result), [carry] "=r" (carry_out), [overflow] "=r" (overflow)
                    : [a] "r" (a), [b] "r" (b), [temp] "r" (temp)
                    : "cc"
                );
            }
            break;
        default:
            result = 0;
            carry_out = 0;
            overflow = 0;
            break;
    }

    N_FLAG = (result & 0x80000000) != 0;
    Z_FLAG = (result == 0);
    C_FLAG = (carry_out != 0);
    V_FLAG = (overflow != 0);

    return result;
}

// Optimized memory operations
static inline u32 memReadOptimized(u32 address)
{
    address &= ARAM_MASK;
    u32 value;

    __asm__ volatile(
        "ldr %w[value], [%[addr]]\n"
        : [value] "=r" (value)
        : [addr] "r" (&aica_ram[address])
        : "memory"
    );

    return value;
}

static inline void memWriteOptimized(u32 address, u32 value)
{
    address &= ARAM_MASK;

    __asm__ volatile(
        "str %w[value], [%[addr]]\n"
        :
        : [value] "r" (value), [addr] "r" (&aica_ram[address])
        : "memory"
    );
}

// Optimized condition check
static inline bool checkConditionOptimized(u32 condition)
{
    bool result;

    switch(condition) {
        case 0: // EQ
            result = Z_FLAG;
            break;
        case 1: // NE
            result = !Z_FLAG;
            break;
        case 2: // CS/HS
            result = C_FLAG;
            break;
        case 3: // CC/LO
            result = !C_FLAG;
            break;
        case 4: // MI
            result = N_FLAG;
            break;
        case 5: // PL
            result = !N_FLAG;
            break;
        case 6: // VS
            result = V_FLAG;
            break;
        case 7: // VC
            result = !V_FLAG;
            break;
        case 8: // HI
            result = C_FLAG && !Z_FLAG;
            break;
        case 9: // LS
            result = !C_FLAG || Z_FLAG;
            break;
        case 10: // GE
            result = (N_FLAG == V_FLAG);
            break;
        case 11: // LT
            result = (N_FLAG != V_FLAG);
            break;
        case 12: // GT
            result = !Z_FLAG && (N_FLAG == V_FLAG);
            break;
        case 13: // LE
            result = Z_FLAG || (N_FLAG != V_FLAG);
            break;
        case 14: // AL
            result = true;
            break;
        default:
            result = false;
            break;
    }

    return result;
}

// Optimized shift operations
static inline u32 shiftOptimized(u32 value, u32 type, u32 amount, bool& carry)
{
    u32 result;
    u32 carry_out = 0;

    switch(type) {
        case 0: // LSL
            if (amount == 0) {
                return value;
            }
            else if (amount < 32) {
                result = value << amount;
                carry_out = (value >> (32 - amount)) & 1;
            }
            else if (amount == 32) {
                result = 0;
                carry_out = value & 1;
            }
            else {
                result = 0;
                carry_out = 0;
            }
            break;

        case 1: // LSR
            if (amount == 0) {
                return value;
            }
            else if (amount < 32) {
                result = value >> amount;
                carry_out = (value >> (amount - 1)) & 1;
            }
            else if (amount == 32) {
                result = 0;
                carry_out = (value >> 31) & 1;
            }
            else {
                result = 0;
                carry_out = 0;
            }
            break;

        case 2: // ASR
            if (amount == 0) {
                return value;
            }
            else if (amount < 32) {
                result = (s32)value >> amount;
                carry_out = (value >> (amount - 1)) & 1;
            }
            else {
                // ASR by 32 or more gives all 1s or all 0s depending on sign bit
                result = (value & 0x80000000) ? 0xFFFFFFFF : 0;
                carry_out = (value >> 31) & 1;
            }
            break;

        case 3: // ROR
            if (amount == 0) {
                return value;
            }
            else {
                amount &= 0x1F; // ROR is always modulo 32
                if (amount == 0) {
                    result = value;
                    carry_out = (value >> 31) & 1;
                }
                else {
                    result = (value >> amount) | (value << (32 - amount));
                    carry_out = (value >> (amount - 1)) & 1;
                }
            }
            break;

        default:
            result = value;
            carry_out = 0;
            break;
    }

    carry = carry_out != 0;
    return result;
}
#endif

} // namespace aica::arm
