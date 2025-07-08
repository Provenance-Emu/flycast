/*
	High-performance SH4 interpreter optimized for iOS ARM64
	Focuses on eliminating decode/dispatch overhead through batching
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

// === CORE ARCHITECTURE ===
extern int getDynamicCpuRatio();

Sh4ICache icache;
Sh4OCache ocache;

// === INSTRUCTION CACHE FOR REDUCING MEMORY READS ===
#define ICACHE_SIZE 2048
#define ICACHE_MASK (ICACHE_SIZE - 1)

struct HighPerformanceCache {
    u32 pc[ICACHE_SIZE];
    u16 opcode[ICACHE_SIZE];
    u32 access_count[ICACHE_SIZE];
    
    void reset() {
        for (int i = 0; i < ICACHE_SIZE; i++) {
            pc[i] = 0xFFFFFFFF;
            access_count[i] = 0;
        }
    }
    
    u16 fetch(u32 addr) {
        u32 index = (addr >> 1) & ICACHE_MASK;
        
        if (__builtin_expect(pc[index] == addr, 1)) {
            access_count[index]++;
            return opcode[index];
        }
        
        u16 op = IReadMem16(addr);
        pc[index] = addr;
        opcode[index] = op;
        access_count[index] = 1;
        return op;
    }
    
    bool isHot(u32 addr) {
        u32 index = (addr >> 1) & ICACHE_MASK;
        return pc[index] == addr && access_count[index] > 10;
    }
};

static HighPerformanceCache g_cache;

// === ULTRA-AGGRESSIVE CYCLE BATCHING ===
static u32 g_cycle_debt = 0;
static const u32 BATCH_THRESHOLD = 50; // Very large batches

static inline void addCycles(u32 cycles) {
    g_cycle_debt += cycles;
}

static inline void flushCycles() {
    if (g_cycle_debt > 0) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

static inline void checkAndFlushCycles() {
    if (__builtin_expect(g_cycle_debt >= BATCH_THRESHOLD, 0)) {
        sh4cycles.addCycles(g_cycle_debt);
        g_cycle_debt = 0;
    }
}

// === FAST INSTRUCTION EXECUTION ===
static inline bool executeFast(u16 op) {
    switch (op & 0xF000) {
        case 0x6000: // mov family
            if ((op & 0x000F) == 0x0003) { // mov Rm,Rn
                u32 m = (op >> 4) & 0xF;
                u32 n = (op >> 8) & 0xF;
                r[n] = r[m];
                addCycles(1);
                return true;
            }
            break;
            
        case 0x7000: // add #imm,Rn
            {
                u32 n = (op >> 8) & 0xF;
                s32 imm = (s32)(s8)(op & 0xFF);
                r[n] += imm;
                addCycles(1);
                return true;
            }
            
        case 0xE000: // mov #imm,Rn
            {
                u32 n = (op >> 8) & 0xF;
                r[n] = (u32)(s32)(s8)(op & 0xFF);
                addCycles(1);
                return true;
            }
            
        case 0x0000: // nop
            if ((op & 0x00FF) == 0x0009) {
                addCycles(1);
                return true;
            }
            break;
    }
    return false;
}

static inline u16 fetchInstruction() {
    if (__builtin_expect(!mmu_enabled() && (next_pc & 1), 0))
        throw SH4ThrownException(next_pc, Sh4Ex_AddressErrorRead);

    u32 addr = next_pc;
    next_pc += 2;
    return g_cache.fetch(addr);
}

// === BATCH EXECUTION ENGINE ===
// The key insight: execute multiple instructions before checking timeslice
static inline void executeBatch() {
    const u32 BATCH_SIZE = 16; // Execute this many instructions before checking timeslice
    
    for (u32 i = 0; i < BATCH_SIZE; i++) {
        // Early exit if timeslice exhausted
        if (__builtin_expect(p_sh4rcb->cntx.cycle_counter <= 0, 0)) {
            break;
        }
        
        u32 op = fetchInstruction();
        
        // Try fast path first
        if (__builtin_expect(executeFast(op), 1)) {
            // Fast path succeeded, continue
            continue;
        }
        
        // Fall back to standard execution
        if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
            RaiseFPUDisableException();
        
        OpPtr[op](op);
        addCycles(sh4cycles.countCycles(op));
        
        // Check if we should flush cycles occasionally
        checkAndFlushCycles();
    }
}

// === HOT SEQUENCE DETECTION ===
static u32 g_last_pc = 0;
static u32 g_hot_counter = 0;

static inline bool isInHotSequence() {
    u32 current_pc = next_pc;
    
    if (current_pc == g_last_pc + 2) {
        g_hot_counter++;
        g_last_pc = current_pc;
        return g_hot_counter > 8 && g_cache.isHot(current_pc);
    } else {
        g_hot_counter = 0;
        g_last_pc = current_pc;
        return false;
    }
}

// === MAIN EXECUTION LOOP ===
static void Sh4_int_Run()
{
    RestoreHostRoundingMode();

    g_cache.reset();
    g_cycle_debt = 0;

    try {
        do {
            try {
                do {
                    // For hot sequences, use larger batches to reduce loop overhead
                    if (isInHotSequence()) {
                        // Execute larger batches for hot code (like FMVs)
                        const u32 HOT_BATCH_SIZE = 32;
                        
                        for (u32 i = 0; i < HOT_BATCH_SIZE && p_sh4rcb->cntx.cycle_counter > 0; i++) {
                            u32 op = fetchInstruction();
                            
                            if (!executeFast(op)) {
                                // Complex instruction in hot path - fall back to normal execution
                                if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
                                    RaiseFPUDisableException();
                                OpPtr[op](op);
                                addCycles(sh4cycles.countCycles(op));
                            }
                        }
                        
                        // Flush cycles less frequently in hot paths
                        if (g_cycle_debt >= BATCH_THRESHOLD * 2) {
                            flushCycles();
                        }
                    } else {
                        // Normal batch execution
                        executeBatch();
                    }
                    
                } while (__builtin_expect(p_sh4rcb->cntx.cycle_counter > 0, 1));
                
                // Always flush cycles at end of timeslice
                flushCycles();
                
                p_sh4rcb->cntx.cycle_counter += SH4_TIMESLICE;
                UpdateSystem_INTC();
                
            } catch (const SH4ThrownException& ex) {
                flushCycles();
                Do_Exception(ex.epc, ex.expEvn);
                sh4cycles.addCycles(5 * getDynamicCpuRatio());
            }
        } while (__builtin_expect(sh4_int_bCpuRun, 1));
    } catch (const debugger::Stop&) {
        flushCycles();
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
    flushCycles();
}

void Sh4_int_Step()
{
    verify(!sh4_int_bCpuRun);

    RestoreHostRoundingMode();
    try {
        u32 op = fetchInstruction();
        
        if (!executeFast(op)) {
            if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
                RaiseFPUDisableException();
            OpPtr[op](op);
            addCycles(sh4cycles.countCycles(op));
        }
        
        flushCycles();
        
    } catch (const SH4ThrownException& ex) {
        flushCycles();
        Do_Exception(ex.epc, ex.expEvn);
        sh4cycles.addCycles(5 * getDynamicCpuRatio());
    } catch (const debugger::Stop&) {
        flushCycles();
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
    
    g_cache.reset();
    g_cycle_debt = 0;
    g_last_pc = 0;
    g_hot_counter = 0;

    INFO_LOG(INTERPRETER, "🚀 HIGH-PERFORMANCE SH4 Interpreter - Batch execution with aggressive cycle batching!");
}

static bool Sh4_int_IsCpuRunning()
{
    return sh4_int_bCpuRun;
}

void ExecuteDelayslot()
{
    try {
        u32 op = fetchInstruction();
        
        if (!executeFast(op)) {
            if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
                RaiseFPUDisableException();
            OpPtr[op](op);
            addCycles(sh4cycles.countCycles(op));
        }
        
        flushCycles();
        
    } catch (SH4ThrownException& ex) {
        flushCycles();
        AdjustDelaySlotException(ex);
        throw ex;
    } catch (const debugger::Stop& e) {
        flushCycles();
        next_pc -= 2;
        throw e;
    }
}

void ExecuteDelayslot_RTE()
{
    try {
        u32 op = fetchInstruction();
        sh4_sr_SetFull(ssr);
        
        if (!executeFast(op)) {
            if (__builtin_expect(sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
                RaiseFPUDisableException();
            OpPtr[op](op);
            addCycles(sh4cycles.countCycles(op));
        }
        
        flushCycles();
        
    } catch (const SH4ThrownException&) {
        flushCycles();
        throw FlycastException("Fatal: SH4 exception in RTE delay slot");
    } catch (const debugger::Stop& e) {
        flushCycles();
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
    g_cache.reset();
    g_cycle_debt = 0;
    g_last_pc = 0;
    g_hot_counter = 0;
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
    INFO_LOG(INTERPRETER, "🚀 HIGH-PERFORMANCE-INTERPRETER: Batch execution engine active!");
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
