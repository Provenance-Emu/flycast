/*
	Ultra-aggressive SH4 interpreter optimized specifically for FMV performance
	Maximizes CPU utilization through massive batching and minimal overhead
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

// === ULTRA-AGGRESSIVE FMV ARCHITECTURE ===
extern int getDynamicCpuRatio();

Sh4ICache icache;
Sh4OCache ocache;

// === MASSIVE INSTRUCTION CACHE ===
#define ICACHE_SIZE 512  // smaller cache helps with slowdowns
#define ICACHE_MASK (ICACHE_SIZE - 1)

struct UltraCache {
    u32 pc[ICACHE_SIZE];
    u16 opcode[ICACHE_SIZE];
    u32 access_count[ICACHE_SIZE];
    u8 estimated_cycles[ICACHE_SIZE]; // Pre-calculated cycle estimates
    
    void reset() {
        std::fill(pc, pc + ICACHE_SIZE, 0xFFFFFFFF);
        std::memset(access_count, 0, sizeof(access_count[0]) * ICACHE_SIZE);
        std::fill(estimated_cycles, estimated_cycles + ICACHE_SIZE, 1);
    }
    
    u16 fetch(u32 addr, u8* cycles_out) {
        u32 index = (addr >> 1) & ICACHE_MASK;
        
        if (__builtin_expect(pc[index] == addr, 1)) {
            access_count[index]++;
            *cycles_out = estimated_cycles[index];
            return opcode[index];
        }
        
        u16 op = IReadMem16(addr);
        pc[index] = addr;
        opcode[index] = op;
        access_count[index] = 1;
        
        // Pre-calculate estimated cycles for this instruction
        u8 est_cycles = estimateInstructionCycles(op);
        estimated_cycles[index] = est_cycles;
        *cycles_out = est_cycles;
        
        return op;
    }
    
    bool isUltraHot(u32 addr) {
        u32 index = (addr >> 1) & ICACHE_MASK;
        return pc[index] == addr && access_count[index] > 20; // Much higher threshold
    }
    
private:
    u8 estimateInstructionCycles(u16 op) {
        // Fast cycle estimation without full opcode decode
        switch (op & 0xF000) {
            case 0x6000: // mov family - often memory operations
                if ((op & 0x000F) <= 0x0003) return 2; // Memory load/store
                return 1;
            case 0x2000: // Memory stores
                return 2;
            case 0x8000: // Conditional branches
            case 0xA000: // Branch
            case 0xB000: // Branch
                return 2;
            case 0xF000: // FPU operations
                return 3;
            default:
                return 1; // Most arithmetic/logic operations
        }
    }
};

static UltraCache g_ultra_cache;

// === ULTRA-AGGRESSIVE CYCLE MANAGEMENT ===
static u32 g_cycle_debt = 0;
static u32 g_instruction_count = 0;

// For FMV scenarios, we batch MUCH more aggressively
static const u32 FMV_CYCLE_BATCH_SIZE = 512;      // Massive cycle batches
static const u32 NORMAL_CYCLE_BATCH_SIZE = 128;   // Still large for normal code

static inline void addCyclesUltraFast(u8 cycles) {
    g_cycle_debt += cycles;
    g_instruction_count++;
}

static inline void flushCyclesIfNeeded() {
    u32 batch_size = g_instruction_count > 100 ? FMV_CYCLE_BATCH_SIZE : NORMAL_CYCLE_BATCH_SIZE;
    
    if (__builtin_expect(g_cycle_debt >= batch_size, 0)) {
        sh4cycles.addCycles(g_cycle_debt);
        p_sh4rcb->cntx.cycle_counter -= g_cycle_debt;
        g_cycle_debt = 0;
    }
}

static inline void forceFlushCycles() {
    if (g_cycle_debt > 0) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

// === FMV DETECTION SYSTEM ===
static u32 g_consecutive_instructions = 0;
static u32 g_last_pc = 0;

static inline bool isInFMVMode() {
    // Detect FMV-like scenarios: consecutive execution with hot cache
    u32 current_pc = next_pc;
    
    if (current_pc == g_last_pc + 2) {
        g_consecutive_instructions++;
        g_last_pc = current_pc;
        
        // FMV mode: long sequences of consecutive instructions with hot cache
        return g_consecutive_instructions > 50 && g_ultra_cache.isUltraHot(current_pc);
    } else {
        g_consecutive_instructions = 0;
        g_last_pc = current_pc;
        return false;
    }
}

// === ULTRA-FAST INSTRUCTION FETCHING ===
static inline u16 fetchInstructionUltraFast(u8* cycles_out) {
    if (__builtin_expect(!mmu_enabled() && (next_pc & 1), 0))
        throw SH4ThrownException(next_pc, Sh4Ex_AddressErrorRead);

    u32 addr = next_pc;
    next_pc += 2;
    return g_ultra_cache.fetch(addr, cycles_out);
}

// === MEGA-BATCH EXECUTION ENGINES ===

// Ultra-fast execution for FMV scenarios
static inline void executeFMVMegaBatch() {
    const u32 MEGA_BATCH_SIZE = 256; // Execute 256 instructions in one go!
    
    for (u32 i = 0; i < MEGA_BATCH_SIZE; i++) {
        // Check timeslice only occasionally
        if (__builtin_expect((i & 63) == 0 && p_sh4rcb->cntx.cycle_counter <= 0, 0)) {
            break;
        }
        
        u8 estimated_cycles;
        u16 op = fetchInstructionUltraFast(&estimated_cycles);
        
        // Execute instruction
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        
        OpPtr[op](op);
        addCyclesUltraFast(estimated_cycles);
        
        // Minimal overhead cycle checking - only every 32 instructions
        if ((i & 31) == 31) {
            flushCyclesIfNeeded();
        }
    }
}

// Fast execution for hot paths
static inline void executeHotBatch() {
    const u32 HOT_BATCH_SIZE = 64;
    
    for (u32 i = 0; i < HOT_BATCH_SIZE && p_sh4rcb->cntx.cycle_counter > 0; i++) {
        u8 estimated_cycles;
        u16 op = fetchInstructionUltraFast(&estimated_cycles);
        
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        
        OpPtr[op](op);
        addCyclesUltraFast(estimated_cycles);
        
        // Check cycles every 16 instructions
        if ((i & 15) == 15) {
            flushCyclesIfNeeded();
        }
    }
}

// Normal execution
static inline void executeNormalBatch() {
    const u32 NORMAL_BATCH_SIZE = 16;
    
    for (u32 i = 0; i < NORMAL_BATCH_SIZE && p_sh4rcb->cntx.cycle_counter > 0; i++) {
        u8 estimated_cycles;
        u16 op = fetchInstructionUltraFast(&estimated_cycles);
        
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        
        OpPtr[op](op);
        addCyclesUltraFast(estimated_cycles);
        
        // Check cycles every 8 instructions
        if ((i & 7) == 7) {
            flushCyclesIfNeeded();
        }
    }
}

// === ADAPTIVE EXECUTION ENGINE ===
static inline void executeUltraAdaptive() {
    if (isInFMVMode()) {
        // FMV mode: maximum performance, minimal checks
        executeFMVMegaBatch();
    } else if (g_ultra_cache.isUltraHot(next_pc)) {
        // Hot path: high performance
        executeHotBatch();
    } else {
        // Normal code: standard batching
        executeNormalBatch();
    }
}

// === MAIN EXECUTION LOOP ===
static void Sh4_int_Run()
{
    RestoreHostRoundingMode();

    g_ultra_cache.reset();
    g_cycle_debt = 0;
    g_instruction_count = 0;

    try {
        do {
            try {
                do {
                    executeUltraAdaptive();
                } while (__builtin_expect(p_sh4rcb->cntx.cycle_counter > 0, 1));
                
                // Always flush remaining cycles at end of timeslice
                forceFlushCycles();
                
                p_sh4rcb->cntx.cycle_counter += SH4_TIMESLICE;
                UpdateSystem_INTC();
                
            } catch (const SH4ThrownException& ex) {
                forceFlushCycles();
                Do_Exception(ex.epc, ex.expEvn);
                addCyclesUltraFast(5 * getDynamicCpuRatio());
                forceFlushCycles();
            }
        } while (__builtin_expect(sh4_int_bCpuRun, 1));
    } catch (const debugger::Stop&) {
        forceFlushCycles();
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
    forceFlushCycles();
}

void Sh4_int_Step()
{
    verify(!sh4_int_bCpuRun);

    RestoreHostRoundingMode();
    try {
        u8 estimated_cycles;
        u16 op = fetchInstructionUltraFast(&estimated_cycles);
        
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        
        OpPtr[op](op);
        addCyclesUltraFast(estimated_cycles);
        forceFlushCycles();
    } catch (const SH4ThrownException& ex) {
        forceFlushCycles();
        Do_Exception(ex.epc, ex.expEvn);
        addCyclesUltraFast(5 * getDynamicCpuRatio());
        forceFlushCycles();
    } catch (const debugger::Stop&) {
        forceFlushCycles();
    }
}

static void Sh4_int_Reset(bool hard)
{
    verify(!sh4_int_bCpuRun);

    if (hard) {
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
    
    g_ultra_cache.reset();
    g_cycle_debt = 0;
    g_instruction_count = 0;
    g_consecutive_instructions = 0;
    g_last_pc = 0;

    INFO_LOG(INTERPRETER, "🚀 ULTRA-AGGRESSIVE FMV OPTIMIZER - Massive batching for maximum CPU utilization!");
}

static bool Sh4_int_IsCpuRunning()
{
    return sh4_int_bCpuRun;
}

void ExecuteDelayslot()
{
    try {
        u8 estimated_cycles;
        u16 op = fetchInstructionUltraFast(&estimated_cycles);
        
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        
        OpPtr[op](op);
        addCyclesUltraFast(estimated_cycles);
        forceFlushCycles();
    } catch (SH4ThrownException& ex) {
        forceFlushCycles();
        AdjustDelaySlotException(ex);
        throw ex;
    } catch (const debugger::Stop& e) {
        forceFlushCycles();
        next_pc -= 2;
        throw e;
    }
}

void ExecuteDelayslot_RTE()
{
    try {
        u8 estimated_cycles;
        u16 op = fetchInstructionUltraFast(&estimated_cycles);
        sh4_sr_SetFull(ssr);
        
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        OpPtr[op](op);
        addCyclesUltraFast(estimated_cycles);
        forceFlushCycles();
        
    } catch (const SH4ThrownException&) {
        forceFlushCycles();
        throw FlycastException("Fatal: SH4 exception in RTE delay slot");
    } catch (const debugger::Stop& e) {
        forceFlushCycles();
        next_pc -= 2;
        throw e;
    }
}

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
    g_ultra_cache.reset();
    g_cycle_debt = 0;
    g_instruction_count = 0;
    g_consecutive_instructions = 0;
    g_last_pc = 0;
}

static void Sh4_int_Init()
{
    static_assert(sizeof(Sh4cntx) == 448, "Invalid Sh4Cntx size");
    memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
    sh4_int_resetcache();
}

static void Sh4_int_Term()
{
    Sh4_int_Stop();
    INFO_LOG(INTERPRETER, "Sh4 Term");
}

#ifndef ENABLE_SH4_CACHED_IR
void Get_Sh4Interpreter(sh4_if* cpu)
{
    INFO_LOG(INTERPRETER, "🚀 ULTRA-AGGRESSIVE FMV OPTIMIZER - Massive batching for maximum CPU utilization!");
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
