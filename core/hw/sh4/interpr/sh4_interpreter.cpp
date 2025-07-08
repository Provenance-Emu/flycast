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

// === ADVANCED INSTRUCTION CACHE WITH PREDICTION ===
#define ADVANCED_ICACHE_SIZE 2048
#define ADVANCED_ICACHE_MASK (ADVANCED_ICACHE_SIZE - 1)

struct AdvancedInstructionCache {
    u32 pc[ADVANCED_ICACHE_SIZE];
    u16 opcode[ADVANCED_ICACHE_SIZE];
    u8 predicted_next[ADVANCED_ICACHE_SIZE];  // Predicted next instruction type
    u32 access_count[ADVANCED_ICACHE_SIZE];   // Access frequency for hot path detection
    
    void reset() {
        for (int i = 0; i < ADVANCED_ICACHE_SIZE; i++) {
            pc[i] = 0xFFFFFFFF;
            predicted_next[i] = 0;
            access_count[i] = 0;
        }
    }
    
    u16 fetch(u32 addr) {
        u32 index = (addr >> 1) & ADVANCED_ICACHE_MASK;
        
        if (__builtin_expect(pc[index] == addr, 1)) {
            access_count[index]++;
            return opcode[index];
        }
        
        // Cache miss - fetch from memory
        u16 op = IReadMem16(addr);
        pc[index] = addr;
        opcode[index] = op;
        access_count[index] = 1;
        predicted_next[index] = predictNextInstructionType(op);
        return op;
    }
    
    // Simple instruction type prediction for better branch prediction
    u8 predictNextInstructionType(u16 op) {
        // Basic categorization for better CPU branch prediction
        if ((op & 0xF000) == 0x6000) return 1; // mov instructions
        if ((op & 0xF000) == 0x3000) return 2; // arithmetic
        if ((op & 0xF000) == 0x8000) return 3; // conditional branches  
        if ((op & 0xF000) == 0xA000) return 4; // unconditional branches
        return 0; // other
    }
    
    bool isHotPath(u32 addr) {
        u32 index = (addr >> 1) & ADVANCED_ICACHE_MASK;
        return pc[index] == addr && access_count[index] > 100;
    }
};

static AdvancedInstructionCache g_advanced_icache;

// === OPTIMIZED CYCLE COUNTING ===
// Batch cycle counting to reduce overhead
static int g_cycle_debt = 0;
static constexpr int CYCLE_BATCH_SIZE = 16;  // Increased from 8 to 16 for better batching

static inline void BatchedExecuteCycles(u16 op) {
    // For fast path operations, use simplified cycle counting
    g_cycle_debt += 1;  // Most fast path ops are 1 cycle
    if (__builtin_expect(g_cycle_debt >= CYCLE_BATCH_SIZE, 0)) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

static inline void BatchedExecuteCyclesStandard(u16 op) {
    // For standard path operations, use full cycle counting
    g_cycle_debt += sh4cycles.countCycles(op);
    if (__builtin_expect(g_cycle_debt >= CYCLE_BATCH_SIZE, 0)) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

static inline void FlushCycleDebt() {
    if (g_cycle_debt > 0) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

// === FAST PATH OPCODE EXECUTION ===
// Optimize the most common opcodes with specialized fast paths
static inline bool ExecuteFastPath(u16 op) {
    // Fast path for most common opcodes during FMV playback
    switch (op & 0xF000) {
        case 0x6000: // mov family - very common
            if ((op & 0x000F) == 0x0003) { // mov Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = r[m];
                return true;
            }
            if ((op & 0x000F) == 0x0002) { // mov.l @Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = ReadMem32(r[m]);
                return true;
            }
            if ((op & 0x000F) == 0x0006) { // mov.l @Rm+,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = ReadMem32(r[m]);
                if (n != m) r[m] += 4;
                return true;
            }
            if ((op & 0x000F) == 0x0001) { // mov.w @Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = (u32)(s32)(s16)ReadMem16(r[m]);
                return true;
            }
            if ((op & 0x000F) == 0x0005) { // mov.w @Rm+,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = (u32)(s32)(s16)ReadMem16(r[m]);
                if (n != m) r[m] += 2;
                return true;
            }
            break;
            
        case 0x7000: // add #imm,Rn - very common
            {
                u32 n = (op >> 8) & 0xF;
                s32 imm = (s32)(s8)(op & 0xFF);
                r[n] += imm;
                return true;
            }
            
        case 0xE000: // mov #imm,Rn - very common
            {
                u32 n = (op >> 8) & 0xF;
                r[n] = (u32)(s32)(s8)(op & 0xFF);
                return true;
            }
            
        case 0x2000: // Memory store operations - common in FMV
            if ((op & 0x000F) == 0x0002) { // mov.l Rm,@Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                WriteMem32(r[n], r[m]);
                return true;
            }
            if ((op & 0x000F) == 0x0006) { // mov.l Rm,@-Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] -= 4;
                WriteMem32(r[n], r[m]);
                return true;
            }
            if ((op & 0x000F) == 0x0001) { // mov.w Rm,@Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                WriteMem16(r[n], r[m]);
                return true;
            }
            if ((op & 0x000F) == 0x0000) { // mov.b Rm,@Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                WriteMem8(r[n], r[m]);
                return true;
            }
            break;
            
        case 0x3000: // Arithmetic operations - common in loops
            if ((op & 0x000F) == 0x000C) { // add Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] += r[m];
                return true;
            }
            if ((op & 0x000F) == 0x0008) { // sub Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] -= r[m];
                return true;
            }
            if ((op & 0x000F) == 0x0000) { // cmp/eq Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                sr.T = (r[n] == r[m]) ? 1 : 0;
                return true;
            }
            if ((op & 0x000F) == 0x0002) { // cmp/hs Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                sr.T = (r[n] >= r[m]) ? 1 : 0;
                return true;
            }
            if ((op & 0x000F) == 0x0006) { // cmp/hi Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                sr.T = (r[n] > r[m]) ? 1 : 0;
                return true;
            }
            break;
            
        case 0x4000: // Single operand operations
            if ((op & 0x00FF) == 0x0000) { // shll Rn
                u32 n = (op >> 8) & 0xF;
                sr.T = r[n] >> 31;
                r[n] <<= 1;
                return true;
            }
            if ((op & 0x00FF) == 0x0001) { // shlr Rn
                u32 n = (op >> 8) & 0xF;
                sr.T = r[n] & 1;
                r[n] >>= 1;
                return true;
            }
            if ((op & 0x00FF) == 0x0020) { // shal Rn
                u32 n = (op >> 8) & 0xF;
                sr.T = r[n] >> 31;
                r[n] = ((s32)r[n]) << 1;
                return true;
            }
            if ((op & 0x00FF) == 0x0021) { // shar Rn
                u32 n = (op >> 8) & 0xF;
                sr.T = r[n] & 1;
                r[n] = ((s32)r[n]) >> 1;
                return true;
            }
            if ((op & 0x00FF) == 0x0010) { // dt Rn
                u32 n = (op >> 8) & 0xF;
                r[n] -= 1;
                sr.T = (r[n] == 0) ? 1 : 0;
                return true;
            }
            break;
            
        case 0x8000: // Conditional branches and immediate operations
            if ((op & 0x0F00) == 0x0B00) { // bf label
                s32 disp = (s32)(s8)(op & 0xFF);
                if (sr.T == 0) {
                    next_pc = next_pc + (disp << 1);
                }
                return true;
            }
            if ((op & 0x0F00) == 0x0900) { // bt label
                s32 disp = (s32)(s8)(op & 0xFF);
                if (sr.T == 1) {
                    next_pc = next_pc + (disp << 1);
                }
                return true;
            }
            break;
            
        case 0x9000: // mov.w @(disp,PC),Rn - PC-relative loads
            {
                u32 n = (op >> 8) & 0xF;
                u32 disp = op & 0xFF;
                r[n] = (u32)(s32)(s16)ReadMem16((disp << 1) + next_pc + 2);
                return true;
            }
            
        case 0xD000: // mov.l @(disp,PC),Rn - PC-relative loads
            {
                u32 n = (op >> 8) & 0xF;
                u32 disp = op & 0xFF;
                r[n] = ReadMem32(((disp << 2) + (next_pc & ~3) + 4));
                return true;
            }
            
        case 0x1000: // mov.l Rm,@(disp,Rn) - displaced stores
            {
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                u32 disp = (op & 0xF) << 2;
                WriteMem32(r[n] + disp, r[m]);
                return true;
            }
            
        case 0x5000: // mov.l @(disp,Rm),Rn - displaced loads
            {
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                u32 disp = (op & 0xF) << 2;
                r[n] = ReadMem32(r[m] + disp);
                return true;
            }
            
        case 0x0000: // Simple operations
            if ((op & 0x00FF) == 0x0009) { // nop
                return true;
            }
            if ((op & 0x000F) == 0x000C) { // mov.l @(R0,Rm),Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = ReadMem32(r[0] + r[m]);
                return true;
            }
            if ((op & 0x000F) == 0x000E) { // mov.l @(R0,Rm),Rn (alternate encoding)
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = ReadMem32(r[0] + r[m]);
                return true;
            }
            break;
            
        case 0xC000: // GBR-relative and immediate operations
            if ((op & 0xFF00) == 0xC800) { // tst #imm,R0
                u32 imm = op & 0xFF;
                sr.T = ((r[0] & imm) == 0) ? 1 : 0;
                return true;
            }
            if ((op & 0xFF00) == 0xC900) { // and #imm,R0
                u32 imm = op & 0xFF;
                r[0] &= imm;
                return true;
            }
            if ((op & 0xFF00) == 0xCA00) { // xor #imm,R0
                u32 imm = op & 0xFF;
                r[0] ^= imm;
                return true;
            }
            if ((op & 0xFF00) == 0xCB00) { // or #imm,R0
                u32 imm = op & 0xFF;
                r[0] |= imm;
                return true;
            }
            break;
    }
    return false;
}

static inline void ExecuteOpcode(u16 op)
{
    // Try fast path first for common operations
    if (__builtin_expect(ExecuteFastPath(op), 0)) {
        BatchedExecuteCycles(op);
        return;
    }
    
    // Standard path for complex operations
    if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
        RaiseFPUDisableException();
    
    OpPtr[op](op);
    BatchedExecuteCyclesStandard(op);
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

// === HOT PATH DETECTION AND OPTIMIZATION ===
static u32 g_last_pc = 0;
static u32 g_hot_path_counter = 0;
static u32 g_sequential_counter = 0;
static bool g_in_fmv_mode = false;

static inline bool IsInHotPath(u32 pc) {
    if (pc == g_last_pc + 2) {
        g_sequential_counter++;
        g_hot_path_counter++;
        
        // More aggressive hot path detection for FMV
        if (g_sequential_counter > 5) {  // Reduced from 10 to 5
            g_in_fmv_mode = true;
            return true;
        }
    } else {
        g_sequential_counter = 0;
        if (g_hot_path_counter > 0) {
            g_hot_path_counter--;  // Gradually reduce counter for non-sequential access
        }
        if (g_hot_path_counter == 0) {
            g_in_fmv_mode = false;
        }
    }
    g_last_pc = pc;
    return g_in_fmv_mode && g_hot_path_counter > 3;
}

// === ENHANCED CACHE PREFETCHING FOR FMV ===
static inline void PrefetchForFMV(u32 pc) {
    if (g_in_fmv_mode) {
        // Prefetch next few instructions for better cache performance
        __builtin_prefetch((void*)(uintptr_t)pc, 0, 3);  // Prefetch for read, high temporal locality
        __builtin_prefetch((void*)(uintptr_t)(pc + 8), 0, 2);  // Prefetch next instruction pair
    }
}

static void Sh4_int_Run()
{
    RestoreHostRoundingMode();

    // Reset instruction cache at start
    g_advanced_icache.reset();
    g_cycle_debt = 0;

    try {
        do
        {
            try {
                // Hot path optimization: reduce overhead in tight loops
                bool in_hot_path = false;
                u32 hot_path_start = next_pc;
                
                do
                {
                    u32 current_pc = next_pc;
                    
                    // Enhanced prefetching for FMV performance
                    PrefetchForFMV(current_pc);
                    
                    // Detect hot paths for additional optimization
                    if (!in_hot_path && IsInHotPath(current_pc)) {
                        in_hot_path = true;
                        hot_path_start = current_pc;
                    }
                    
                    u32 op = ReadNexOp();
                    
                    // Hot path: batch multiple simple operations
                    if (in_hot_path && g_advanced_icache.isHotPath(current_pc)) {
                        // Try to execute multiple simple operations in sequence
                        ExecuteOpcode(op);
                        
                        // Check if we can continue hot path execution
                        if (p_sh4rcb->cntx.cycle_counter <= 0 || 
                            (next_pc - hot_path_start) > 128) { // Increased from 64 to 128 for longer hot paths
                            in_hot_path = false;
                            FlushCycleDebt();
                            break;
                        }
                    } else {
                        // Normal execution path
                        ExecuteOpcode(op);
                        in_hot_path = false;
                    }
                    
                } while (__builtin_expect(p_sh4rcb->cntx.cycle_counter > 0, 1));
                
                // Ensure cycle debt is flushed
                FlushCycleDebt();
                
                p_sh4rcb->cntx.cycle_counter += SH4_TIMESLICE;
                UpdateSystem_INTC();
            } catch (const SH4ThrownException& ex) {
                // Ensure cycle debt is flushed on exception
                FlushCycleDebt();
                Do_Exception(ex.epc, ex.expEvn);
                // an exception requires the instruction pipeline to drain, so approx 5 cycles
                sh4cycles.addCycles(5 * getDynamicCpuRatio());
            }
        } while (__builtin_expect(sh4_int_bCpuRun, 1));
    } catch (const debugger::Stop&) {
        FlushCycleDebt();
    }

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
        FlushCycleDebt();
    } catch (const SH4ThrownException& ex) {
        FlushCycleDebt();
        Do_Exception(ex.epc, ex.expEvn);
        // an exception requires the instruction pipeline to drain, so approx 5 cycles
        sh4cycles.addCycles(5 * getDynamicCpuRatio());
    } catch (const debugger::Stop&) {
        FlushCycleDebt();
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
    
    // Reset advanced instruction cache and optimization state
    g_advanced_icache.reset();
    g_cycle_debt = 0;
    g_last_pc = 0;
    g_hot_path_counter = 0;
    g_sequential_counter = 0;
    g_in_fmv_mode = false;

    INFO_LOG(INTERPRETER, "🚀 OPTIMIZED SH4 Interpreter Reset - Advanced caching and hot path detection active!");
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
        FlushCycleDebt();
    } catch (SH4ThrownException& ex) {
        FlushCycleDebt();
        AdjustDelaySlotException(ex);
        throw ex;
    } catch (const debugger::Stop& e) {
        FlushCycleDebt();
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
        FlushCycleDebt();
    } catch (const SH4ThrownException&) {
        FlushCycleDebt();
        throw FlycastException("Fatal: SH4 exception in RTE delay slot");
    } catch (const debugger::Stop& e) {
        FlushCycleDebt();
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
    g_last_pc = 0;
    g_hot_path_counter = 0;
    g_sequential_counter = 0;
    g_in_fmv_mode = false;
}

static void Sh4_int_Init()
{
    static_assert(sizeof(Sh4cntx) == 448, "Invalid Sh4Cntx size");

    memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
    g_advanced_icache.reset();
    g_cycle_debt = 0;
    g_last_pc = 0;
    g_hot_path_counter = 0;
    g_sequential_counter = 0;
    g_in_fmv_mode = false;
}

static void Sh4_int_Term()
{
    Sh4_int_Stop();
    INFO_LOG(INTERPRETER, "Sh4 Term");
}

#ifndef ENABLE_SH4_CACHED_IR
void Get_Sh4Interpreter(sh4_if* cpu)
{
    INFO_LOG(INTERPRETER, "🚀 ADVANCED-INTERPRETER: Get_Sh4Interpreter called — linking optimized interpreter with hot path detection!");
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
