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

#include <array>
#include <algorithm>

// === ULTRA-AGGRESSIVE FMV ARCHITECTURE ===
extern int getDynamicCpuRatio();

Sh4ICache icache;
Sh4OCache ocache;

// === MASSIVE INSTRUCTION CACHE - OPTIMIZED ===
#define ICACHE_SIZE 512  // smaller cache helps with slowdowns
#define ICACHE_MASK (ICACHE_SIZE - 1)

// Cache-line aligned structure for better performance
struct alignas(64) UltraCache {
    // Separate arrays for better cache locality
    alignas(64) std::array<u32, ICACHE_SIZE> pc;
    alignas(64) std::array<u16, ICACHE_SIZE> opcode;
    alignas(64) std::array<u32, ICACHE_SIZE> access_count;
    alignas(64) std::array<u8, ICACHE_SIZE> estimated_cycles; // Pre-calculated cycle estimates
    
    void reset() {
        pc.fill(0xFFFFFFFF);
        std::memset(access_count.data(), 0, access_count.size() * sizeof(u32));
        estimated_cycles.fill(1);
        opcode.fill(0);
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
static u32 g_cycles_since_aica_check = 0;

// AICA runs every 4535 cycles at 44.1 KHz - we must respect this!
static const u32 AICA_TICK_INTERVAL = 4535;
static const u32 AICA_SAFETY_MARGIN = 200;  // Flush cycles early to ensure AICA timing

// Aggressive batch sizes, but AICA-aware
static const u32 FMV_CYCLE_BATCH_SIZE = 512;      // Large batches for FMV performance
static const u32 NORMAL_CYCLE_BATCH_SIZE = 128;   // Smaller batches for normal code
static const u32 AICA_AWARE_BATCH_SIZE = 64;      // Small batches when AICA needs attention

static inline void addCyclesUltraFast(u8 cycles) {
    g_cycle_debt += cycles;
    g_cycles_since_aica_check += cycles;
    g_instruction_count++;
}

static inline void flushCyclesIfNeeded() {
    // CRITICAL: AICA needs to run every 4535 cycles - don't let batching delay it!
    // Check if we're approaching AICA's scheduling deadline
    u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));
    
    // Determine appropriate batch size based on AICA timing needs
    u32 batch_size;
    
    // If AICA needs to run soon, force immediate cycle flush
    if (cycles_until_aica <= AICA_SAFETY_MARGIN || g_cycles_since_aica_check >= (AICA_TICK_INTERVAL - AICA_SAFETY_MARGIN)) {
        // FORCE IMMEDIATE FLUSH - don't let large batches delay AICA!
        if (g_cycle_debt > 0) {
            sh4cycles.addCycles(g_cycle_debt);
            p_sh4rcb->cntx.cycle_counter -= g_cycle_debt;
            g_cycles_since_aica_check = 0;  // Reset AICA timing
            g_cycle_debt = 0;
            return;
        }
        batch_size = 1;  // Minimal batching when AICA is critical
    } else if (g_instruction_count > 100) {
        batch_size = FMV_CYCLE_BATCH_SIZE;   // Large batches for FMV scenarios  
    } else {
        batch_size = NORMAL_CYCLE_BATCH_SIZE; // Normal batches
    }
    
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
        g_cycles_since_aica_check = 0; // Reset AICA tracking
    }
}

// === CONSERVATIVE FMV DETECTION SYSTEM ===
static u32 g_consecutive_instructions = 0;
static u32 g_last_pc = 0;
static u32 g_fmv_mode_timer = 0;
static bool g_in_fmv_mode = false;

static inline bool isInFMVMode() {
    // Much more conservative FMV detection to avoid gameplay false positives
    u32 current_pc = next_pc;
    
    if (current_pc == g_last_pc + 2) {
        g_consecutive_instructions++;
        g_last_pc = current_pc;
        
        // Very strict criteria for FMV mode to avoid gameplay interference
        bool potential_fmv = g_consecutive_instructions > 300 && // Much higher threshold
                            g_ultra_cache.isUltraHot(current_pc) &&
                            (current_pc & 0xFF000000) == 0x8C000000; // Main RAM only, not system areas
        
        if (potential_fmv && !g_in_fmv_mode) {
            g_in_fmv_mode = true;
            g_fmv_mode_timer = 0;
            return true;
        } else if (g_in_fmv_mode) {
            g_fmv_mode_timer++;
            // Auto-exit FMV mode after 5000 instructions to prevent gameplay slowdowns
            if (g_fmv_mode_timer > 5000) {
                g_in_fmv_mode = false;
                g_fmv_mode_timer = 0;
                return false;
            }
            return true;
        }
        
        return false;
    } else {
        // Non-consecutive execution - immediately exit FMV mode
        g_consecutive_instructions = 0;
        g_last_pc = current_pc;
        g_in_fmv_mode = false;
        g_fmv_mode_timer = 0;
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
    const u32 MEGA_BATCH_SIZE = 512; // Large batches but with AICA safety checks
    
    for (u32 i = 0; i < MEGA_BATCH_SIZE; i++) {
        // Check timeslice every 16 instructions
        if (__builtin_expect((i & 15) == 0 && p_sh4rcb->cntx.cycle_counter <= 0, 0)) {
            break;
        }
        
        // CRITICAL: Check AICA timing every 64 instructions during FMV
        // This prevents audio starvation during aggressive FMV optimization
        if ((i & 63) == 63) {
            u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));
            if (cycles_until_aica <= AICA_SAFETY_MARGIN) {
                // AICA deadline approaching - flush cycles immediately and break out
                forceFlushCycles();
                break;  // Exit mega-batch to allow AICA scheduling
            }
        }
        
        u8 estimated_cycles;
        u16 op = fetchInstructionUltraFast(&estimated_cycles);
        
        // Execute instruction
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        
        OpPtr[op](op);
        addCyclesUltraFast(estimated_cycles);
        
        // Check cycles every 32 instructions for regular cycle management
        if ((i & 31) == 31) {
            flushCyclesIfNeeded();
        }
    }
}

// Fast execution for hot paths
static inline void executeHotBatch() {
    const u32 HOT_BATCH_SIZE = 64;  // Moderate batches with AICA safety
    
    for (u32 i = 0; i < HOT_BATCH_SIZE && p_sh4rcb->cntx.cycle_counter > 0; i++) {
        // CRITICAL: Check AICA timing during hot path execution
        if ((i & 31) == 31) {
            u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));
            if (cycles_until_aica <= AICA_SAFETY_MARGIN) {
                // AICA deadline approaching - exit hot batch to allow scheduling
                forceFlushCycles();
                break;
            }
        }
        
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
    const u32 NORMAL_BATCH_SIZE = 32;  // Small batches with AICA protection
    
    for (u32 i = 0; i < NORMAL_BATCH_SIZE && p_sh4rcb->cntx.cycle_counter > 0; i++) {
        // CRITICAL: Check AICA timing even during normal execution
        if ((i & 15) == 15) {
            u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));
            if (cycles_until_aica <= AICA_SAFETY_MARGIN) {
                // AICA deadline approaching - exit normal batch to allow scheduling
                forceFlushCycles();
                break;
            }
        }
        
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
    g_cycles_since_aica_check = 0;  // Reset AICA timing tracking
    g_consecutive_instructions = 0;
    g_last_pc = 0;
    g_fmv_mode_timer = 0;
    g_in_fmv_mode = false;

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
    g_cycles_since_aica_check = 0;  // Reset AICA timing tracking
    g_consecutive_instructions = 0;
    g_last_pc = 0;
    g_fmv_mode_timer = 0;
    g_in_fmv_mode = false;
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
