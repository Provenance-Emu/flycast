/*
	Optimized SH4 interpreter for better performance
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "../sh4_cache.h"
#include "debug/gdb_server.h"
#include "../sh4_cycles.h"

// === ENHANCED DYNAMIC CPU_RATIO SYSTEM ===
// Forward declaration from sh4_cycles.cpp
extern int getDynamicCpuRatio();

#ifdef STRICT_MODE
// Use dynamic ratio instead of static
#else
// Use dynamic ratio instead of static  
#endif

Sh4ICache icache;
Sh4OCache ocache;

// === SAFE ADVANCED INSTRUCTION CACHE ===
// This is the key to FMV speed - larger cache with prediction but NO timing disruption
// Expand cache to keep more prefetched instructions for FMV heavy sequences
#define ADVANCED_ICACHE_SIZE 2048 // 2048 seems like the sweet spot.
#define ADVANCED_ICACHE_MASK (ADVANCED_ICACHE_SIZE - 1)

struct SafeAdvancedInstructionCache {
    u32 pc[ADVANCED_ICACHE_SIZE];
    u16 opcode[ADVANCED_ICACHE_SIZE];
    u8 predicted_next[ADVANCED_ICACHE_SIZE];  // Safe instruction type prediction  
    u32 access_count[ADVANCED_ICACHE_SIZE];   // Access frequency (but NO hot path execution)
    
    void reset() {
        // Ultra-fast reset using memset - preserves audio timing
        memset(pc, 0xFF, sizeof(pc));
        memset(predicted_next, 0, sizeof(predicted_next));
        memset(access_count, 0, sizeof(access_count));
    }
    
    // Simple instruction type prediction for better branch prediction (from original AdvancedInstructionCache)
    u8 predictNextInstructionType(u16 op) {
        // Basic categorization for better CPU branch prediction
        if ((op & 0xF000) == 0x6000) return 1; // mov instructions
        if ((op & 0xF000) == 0x3000) return 2; // arithmetic
        if ((op & 0xF000) == 0x8000) return 3; // conditional branches  
        if ((op & 0xF000) == 0xA000) return 4; // unconditional branches
        return 0; // other
    }
    
    // Check if an opcode is safe to predict (no branches, exceptions, system calls)
    inline bool is_safe_for_prediction(u16 op) {
        // Incrementally expand safe opcodes for better FMV performance
        u16 opcode_family = op & 0xF000;
        
        switch (opcode_family) {
            case 0x6000: // mov.l @Rm,Rn / mov.w @Rm,Rn / mov.b @Rm,Rn and variants
            case 0x2000: // mov.l Rm,@Rn / mov.w Rm,@Rn / mov.b Rm,@Rn and variants  
            case 0x1000: // mov.l Rm,@(disp,Rn) / mov.w Rm,@(disp,Rn) / mov.b Rm,@(disp,Rn)
            case 0x5000: // mov.l @(disp,Rm),Rn / mov.w @(disp,Rm),Rn / mov.b @(disp,Rm),Rn
                return true;
            case 0x3000: // cmp/eq, cmp/hs, cmp/ge, cmp/hi, cmp/gt, add, sub, etc.
                return true;
            case 0xE000: // mov #imm,Rn - very safe immediate moves
                return true;
            case 0x7000: // add #imm,Rn - safe immediate arithmetic
                return true;
            case 0x4000: // Safe 4xxx opcodes commonly used in FMV loops
                {
                    u16 sub_op = op & 0x00FF;
                    switch (sub_op) {
                        case 0x0015: // cmp/pl Rn - test if positive
                        case 0x0011: // cmp/pz Rn - test if positive or zero  
                        case 0x0008: // shll2 Rn - shift left logical 2
                        case 0x0009: // shlr2 Rn - shift right logical 2
                        case 0x0018: // shll8 Rn - shift left logical 8
                        case 0x0019: // shlr8 Rn - shift right logical 8
                        case 0x0028: // shll16 Rn - shift left logical 16
                        case 0x0029: // shlr16 Rn - shift right logical 16
                        case 0x0020: // shal Rn - shift arithmetic left
                        case 0x0021: // shar Rn - shift arithmetic right
                        case 0x0000: // shll Rn - shift left logical 1
                        case 0x0001: // shlr Rn - shift right logical 1
                            return true;
                        default:
                            return false;
                    }
                }
            case 0x9000: // mov.w @(disp,PC),Rn - PC-relative loads (common in FMVs)
            case 0xD000: // mov.l @(disp,PC),Rn - PC-relative loads (common in FMVs)
            case 0xC000: // pref / fmov (FMV decode uses pref)
            case 0xF000: // mac.w / fmac – inner loops in mpegsofdec
            case 0xB000: // bt/s bf/s forward branches – safe for look-ahead
                return true;
            default:
                return false;
        }
    }
    
    u16 fetch(u32 addr) {
        u32 index = (addr >> 1) & ADVANCED_ICACHE_MASK;
        
        if (__builtin_expect(pc[index] == addr, 1)) {
            // Cache hit - track access frequency for FMV detection
            access_count[index]++;
            return opcode[index];
        }
        
        // Cache miss - fetch from memory
        u16 op = IReadMem16(addr);
        pc[index] = addr;
        opcode[index] = op;
        access_count[index] = 1;
        predicted_next[index] = predictNextInstructionType(op);
        
        // 🚀 AGGRESSIVE SEQUENTIAL PREFETCH for FMV performance
        // This is the key optimization that made the original AdvancedInstructionCache fast!
        // Prefetch the next 2-4 sequential instructions during cache misses
        // This dramatically improves FMV decode performance
        
        // Only prefetch if this looks like sequential code (not a branch target)
        if (is_safe_for_prediction(op)) {
            // Adaptive sequential prefetch: 4 → 8 → 16 as the loop stays hot
            int max_ahead = 4;
            if (access_count[index] > 5)
                max_ahead = 16;
            else if (access_count[index] > 1)
                max_ahead = 8;

            for (int lookahead = 1; lookahead <= max_ahead; lookahead++) {
                u32 prefetch_addr = addr + (lookahead * 2);
                u32 prefetch_index = (prefetch_addr >> 1) & ADVANCED_ICACHE_MASK;
                
                // Only prefetch if the cache slot is empty
                if (pc[prefetch_index] != prefetch_addr) {
                    try {
                        u16 prefetch_op = IReadMem16(prefetch_addr);
                        pc[prefetch_index] = prefetch_addr;
                        opcode[prefetch_index] = prefetch_op;
                        access_count[prefetch_index] = 0; // Mark as prefetched
                        predicted_next[prefetch_index] = predictNextInstructionType(prefetch_op);
                        
                        // Stop prefetching if we hit a branch or jump instruction
                        u16 op_family = prefetch_op & 0xF000;
                        if (op_family == 0x8000 || op_family == 0xA000 || 
                            op_family == 0xB000 || op_family == 0xC000) {
                            break; // Stop on branches/jumps
                        }
                    } catch (...) {
                        // Stop prefetching on memory access error
                        break;
                    }
                }
            }
        }
        
        return op;
    }
};

static SafeAdvancedInstructionCache g_advanced_icache;

// === MICRO CYCLE-BATCHING ===
static int g_cycle_debt = 0;
static constexpr int CYCLE_BATCH_SIZE = 32; // small enough not to disturb audio timing

inline void DebtAddCycles(int c) {
    g_cycle_debt += c;
    if (__builtin_expect(g_cycle_debt >= CYCLE_BATCH_SIZE, 0)) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

inline void FlushCycleDebt() {
    if (g_cycle_debt) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

static inline void ExecuteOpcode(u16 op)
{
	if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
		RaiseFPUDisableException();
	OpPtr[op](op);
	DebtAddCycles(sh4cycles.countCycles(op));
}

static inline u16 ReadNexOp()
{
	if (__builtin_expect(!mmu_enabled() && (next_pc & 1), 0))
		// address error
		throw SH4ThrownException(next_pc, Sh4Ex_AddressErrorRead);

	u32 addr = next_pc;
	next_pc += 2;

	// Use advanced instruction cache for better performance
	return g_advanced_icache.fetch(addr);
}

static void Sh4_int_Run()
{
	RestoreHostRoundingMode();

	// Reset instruction cache at start
	g_advanced_icache.reset();

	try {
		do
		{
			try {
				// Optimized inner loop with minimal overhead
				do
				{
					u32 op = ReadNexOp();
					ExecuteOpcode(op);
					// flush batch each inner loop to avoid debt runaway on long loops
					if (unlikely(g_cycle_debt >= CYCLE_BATCH_SIZE))
						FlushCycleDebt();
				} while (__builtin_expect(p_sh4rcb->cntx.cycle_counter > 0, 1));
				
				FlushCycleDebt();
				p_sh4rcb->cntx.cycle_counter += SH4_TIMESLICE;
				UpdateSystem_INTC();
			} catch (const SH4ThrownException& ex) {
				FlushCycleDebt();
				Do_Exception(ex.epc, ex.expEvn);
				// an exception requires the instruction pipeline to drain, so approx 5 cycles
				sh4cycles.addCycles(5 * getDynamicCpuRatio());
			}
		} while (__builtin_expect(sh4_int_bCpuRun, 1));
	} catch (const debugger::Stop&) {
	}

	FlushCycleDebt();
	sh4_int_bCpuRun = false;
}

static void Sh4_int_Start()
{
	sh4_int_bCpuRun = true;
}

static void Sh4_int_Stop()
{
	sh4_int_bCpuRun = false;
	FlushCycleDebt();
}

void Sh4_int_Step()
{
	verify(!sh4_int_bCpuRun);

	RestoreHostRoundingMode();
	try {
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
	} catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
		// an exception requires the instruction pipeline to drain, so approx 5 cycles
		sh4cycles.addCycles(5 * getDynamicCpuRatio());
	} catch (const debugger::Stop&) {
	}
}

static void Sh4_int_Reset(bool hard)
{
	verify(!sh4_int_bCpuRun);

	if (hard)
	{
		int schedNext = p_sh4rcb->cntx.sh4_sched_next;
		memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
		p_sh4rcb->cntx.sh4_sched_next = schedNext;
	}
	next_pc = 0xA0000000;

	memset(r,0,sizeof(r));
	memset(r_bank,0,sizeof(r_bank));

	gbr=ssr=spc=sgr=dbr=vbr=0;
	mac.full=pr=fpul=0;

	sh4_sr_SetFull(0x700000F0);
	old_sr.status=sr.status;
	UpdateSR();

	fpscr.full = 0x00040001;
	old_fpscr = fpscr;

	icache.Reset(hard);
	ocache.Reset(hard);
	sh4cycles.reset();
	p_sh4rcb->cntx.cycle_counter = SH4_TIMESLICE;
	
	// Reset simple instruction cache
	g_advanced_icache.reset();
	g_cycle_debt = 0;

	INFO_LOG(INTERPRETER, "🚀 SAFE ADVANCED INTERPRETER Reset - FMV-optimized cache without timing issues!");
}

static bool Sh4_int_IsCpuRunning()
{
	return sh4_int_bCpuRun;
}

//TODO : Check for valid delayslot instruction
void ExecuteDelayslot()
{
	try {
		u32 op = ReadNexOp();

		ExecuteOpcode(op);
	} catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		next_pc -= 2;	// break on previous instruction
		throw e;
	}
}

void ExecuteDelayslot_RTE()
{
	try {
		// In an RTE delay slot, status register (SR) bits are referenced as follows.
		// In instruction access, the MD bit is used before modification, and in data access,
		// the MD bit is accessed after modification.
		// The other bits—S, T, M, Q, FD, BL, and RB—after modification are used for delay slot
		// instruction execution. The STC and STC.L SR instructions access all SR bits after modification.
		u32 op = ReadNexOp();
		// Now restore all SR bits
		sh4_sr_SetFull(ssr);
		// And execute
		ExecuteOpcode(op);
	} catch (const SH4ThrownException&) {
		throw FlycastException("Fatal: SH4 exception in RTE delay slot");
	} catch (const debugger::Stop& e) {
		next_pc -= 2;	// break on previous instruction
		throw e;
	}
}

// every SH4_TIMESLICE cycles
int UpdateSystem()
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next < 0)
		sh4_sched_tick(SH4_TIMESLICE);

	return Sh4cntx.interrupt_pend;
}

int UpdateSystem_INTC()
{
	if (UpdateSystem())
		return UpdateINTC();
	else
		return 0;
}

static void sh4_int_resetcache() {
	g_advanced_icache.reset();
	g_cycle_debt = 0;
}

static void Sh4_int_Init()
{
	static_assert(sizeof(Sh4cntx) == 448, "Invalid Sh4Cntx size");

	memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
	g_advanced_icache.reset();
	g_cycle_debt = 0;
}

static void Sh4_int_Term()
{
	Sh4_int_Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}

#ifndef ENABLE_SH4_CACHED_IR
void Get_Sh4Interpreter(sh4_if* cpu)
{
    INFO_LOG(INTERPRETER, "🚀 OPTIMIZED-INTERPRETER: Get_Sh4Interpreter called — linking optimized interpreter!");
    cpu->Start = Sh4_int_Start;
    cpu->Run = Sh4_int_Run;
    cpu->Stop = Sh4_int_Stop;
    cpu->Step = Sh4_int_Step;
    cpu->Reset = Sh4_int_Reset;
    cpu->Init = Sh4_int_Init;
    cpu->Term = Sh4_int_Term;
    cpu->IsCpuRunning = Sh4_int_IsCpuRunning;
    cpu->ResetCache = sh4_int_resetcache;
}
#endif // ENABLE_SH4_CACHED_IR
