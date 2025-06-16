#include "ir_executor.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_core.h" // for SH4ThrownException
#include <cassert>
#include "log/Log.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <utility>
#include "hw/mem/addrspace.h"
#include <cstring>
#include "ir_tables.h" // for Op enum count
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/flashrom/nvmem.h" // for getBiosData()

// Auto-generated opcode name table
#include "ir_opnames.inc"

namespace sh4 {
namespace ir {

// ----------------------------------------------------------------------------
//  Execution statistics, trace buffer, and helpers.
//  These are all defined in an anonymous namespace to keep them local to this
//  file. The DumpTrace function is defined here, and functions that call it
//  (like SetPC) are defined after this namespace block.
// ----------------------------------------------------------------------------
namespace {

// --- statistics ---
constexpr size_t kOpCount = static_cast<size_t>(Op::NUM_OPS);
constexpr size_t kOpNamesCount = sizeof(kOpNames) / sizeof(kOpNames[0]);
static std::array<std::atomic<uint64_t>, kOpCount> g_opExecCounts{};
static std::atomic<uint64_t> g_totalExecCount{0};
constexpr uint64_t kLogInterval = 2'000'000; // log every ~2M instructions

inline const char* GetOpName(size_t idx)
{
    if (idx < kOpNamesCount)
        return kOpNames[idx];
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "OP_%zu", idx);
    return buf;
}

static void MaybeDumpStats()
{
    uint64_t executed = g_totalExecCount.load(std::memory_order_relaxed);
    if (executed == 0 || executed % kLogInterval != 0)
        return;

    static uint64_t lastDump = 0;
    if (executed == lastDump)
        return; // already logged this milestone
    lastDump = executed;

    // Build vector of executed opcodes
    struct Item { size_t idx; uint64_t count; };
    std::vector<Item> items;
    items.reserve(kOpCount);
    for (size_t i = 0; i < kOpCount; ++i)
    {
        uint64_t c = g_opExecCounts[i].load(std::memory_order_relaxed);
        if (c)
            items.push_back({i, c});
    }
    // Sort descending by count
    std::partial_sort(items.begin(), items.begin() + std::min<size_t>(10, items.size()), items.end(),
                      [](const Item& a, const Item& b){ return a.count > b.count; });

    INFO_LOG(SH4, "--- IR opcode execution stats after %llu instructions ---", static_cast<unsigned long long>(executed));
    size_t limit = std::min<size_t>(10, items.size());
    for (size_t i = 0; i < limit; ++i)
    {
        const auto& it = items[i];
        INFO_LOG(SH4, "  %-12s : %llu", GetOpName(it.idx), static_cast<unsigned long long>(it.count));
    }
}

// --- execution trace for post-mortem debugging ---
constexpr size_t kTraceLen = 64;
struct TraceEntry { uint32_t pc; Op op; };
static std::array<TraceEntry, kTraceLen> g_traceBuf{};
static size_t g_tracePos = 0;

inline void TraceLog(uint32_t pc, Op op) {
    g_traceBuf[g_tracePos] = {pc, op};
    g_tracePos = (g_tracePos + 1) % kTraceLen;
}

// This is the one and only definition of DumpTrace.
// It's static, so it's local to this translation unit.
// Functions that call it (SetPC, ExecStub) are defined after this namespace.
static void DumpTrace() {
    INFO_LOG(SH4, "---- Last %zu IR instructions ----", kTraceLen);
    for (size_t i = 0; i < kTraceLen; ++i) {
        size_t idx = (g_tracePos + i) % kTraceLen;
        const auto& e = g_traceBuf[idx];
        if (e.op == Op::NOP && e.pc == 0) continue; // empty slot
        INFO_LOG(SH4, "  %08X : %s", e.pc, GetOpName(static_cast<size_t>(e.op)));
    }
}

} // end anonymous namespace

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------
static inline bool IsTopRegion(uint32_t addr)
{
    // Treat anything from 0xF0000000 upward as suspicious
    return addr >= 0xF0000000u;
}

// -----------------------------------------------------------------------------
//  PC write helper to trap problematic jumps or boundary crossings
// -----------------------------------------------------------------------------
static inline void SetPC(Sh4Context* ctx, uint32_t new_pc, const char* why)
{
    uint32_t old_pc = ctx->pc;
    // Added PR and SR.T to existing SetPC logging
    INFO_LOG(SH4, "SetPC: %08X -> %08X (PR:%08X SR.T:%d) via %s", old_pc, new_pc, ctx->pr, ctx->sr.T & 1, why);

    if (new_pc == 0)
    {
        ERROR_LOG(SH4, "*** SetPC to ZERO from %s", why);
        DumpTrace();
    }
    else if (IsTopRegion(new_pc))
    {
        ERROR_LOG(SH4, "*** SetPC to near-top %08X from %s", new_pc, why);
    }

    // Detect sequential walk crossing into top region (e.g., fall-through past FFFFFFBE)
    if (!IsTopRegion(old_pc) && IsTopRegion(new_pc))
    {
        ERROR_LOG(SH4, "*** PC crossed into top region: %08X -> %08X via %s", old_pc, new_pc, why);
    }

    ctx->pc = new_pc;
}

// Fast pointer fetch for main RAM aliases (P1/P2/P3) and P4 SDRAM mirrors (F8–FE)
// Fast pointer fetch for read-only accesses (includes BIOS)
static inline u8* FastRamPtr(uint32_t addr)
{
    // BIOS ROM 0x00000000–0x001FFFFF (2 MiB) and its mirrors in P1/P2/P3 **and P0 0x40000000**
    if ((addr & 0xFFE00000u) == 0x00000000u || // U0 window
        (addr & 0xFFE00000u) == 0x40000000u || // P0 mirror used by ITLB handler
        (addr & 0xFFE00000u) == 0x80000000u || // P1 mirror
        (addr & 0xFFE00000u) == 0xA0000000u || // P2 mirror
        (addr & 0xFFE00000u) == 0xC0000000u)   // P3 mirror
    {
        return nvmem::getBiosData() + (addr & 0x001FFFFF);
    }

    // Main RAM 0x0C000000–0x0FFFFFFF (26-bit mask)
    if ((addr & 0xFC000000u) == 0x0C000000u)
        return addrspace::ram_base + (addr & 0x03FFFFFF);

    // P4 SDRAM mirrors 0xF8xxxxxx–0xFExxxxxx (8 windows of 16 MiB)
    if (addr >= 0xF8000000u && addr < 0xFF000000u)
        return addrspace::ram_base + 0x0C000000 + (addr & 0x00FFFFFF);

    return nullptr;
}

// Helper to quickly identify BIOS ROM regions (including mirrors)
// Returns true if address lies in the *read-only* body of the 2 MiB boot ROM
// Mirrors in P0/P1/P2/P3 are recognised.  The first 0x200 bytes are excluded
// because on real SH-4 they map to on-chip I/O (store-queue / cache control)
// and are writable after reset.
static inline bool IsBiosAddr(uint32_t addr)
{
    bool in_window = ((addr & 0xFFE00000u) == 0x00000000u ||
                      (addr & 0xFFE00000u) == 0x40000000u ||
                      (addr & 0xFFE00000u) == 0x80000000u ||
                      (addr & 0xFFE00000u) == 0xA0000000u ||
                      (addr & 0xFFE00000u) == 0xC0000000u);
    if (!in_window)
        return false;

    // Offset within 2 MiB ROM image
    uint32_t off = addr & 0x001FFFFF;
    return off >= 0x200; // <0x200 is writable on real HW
}

static void LogIllegalBiosWrite(const Instr& ins, uint32_t addr, uint32_t pc)
{
    static bool first = true;
    if (!first)
        return; // log only the first occurrence to avoid flood
    first = false;
    ERROR_LOG(SH4, "ILLEGAL BIOS WRITE attempt: PC=%08X raw=%04X op=%s addr=%08X", pc, ins.raw, GetOpName(static_cast<size_t>(ins.op)), addr);
}

// Fast pointer fetch for WRITE accesses – excludes BIOS ROM regions (read-only)
static inline u8* FastRamPtrWrite(uint32_t addr)
{
    // Main RAM 0x0C000000–0x0FFFFFFF
    if ((addr & 0xFC000000u) == 0x0C000000u)
        return addrspace::ram_base + (addr & 0x03FFFFFF);

    // P4 SDRAM mirrors
    if (addr >= 0xF8000000u && addr < 0xFF000000u)
        return addrspace::ram_base + 0x0C000000 + (addr & 0x00FFFFFF);

    return nullptr; // everything else is treated via MMU
}

// Per-opcode execution helper signature
using ExecFn = void(*)(const sh4::ir::Instr&, Sh4Context*, uint32_t);

// Generic stub that falls back to IllegalInstr -> legacy interpreter.
static void ExecStub(const sh4::ir::Instr&, Sh4Context*, uint32_t pc)
{
    INFO_LOG(SH4, "IR fallback at PC=%08X", pc);
    DumpTrace();
    throw SH4ThrownException(pc, Sh4Ex_IllegalInstr);
}

// Forward helpers for some hot operations already implemented in switch – duplicate minimal logic
static void Exec_NOP(const sh4::ir::Instr&, Sh4Context*, uint32_t) { /* nothing */ }
static void Exec_ADD_REG(const sh4::ir::Instr& ins, Sh4Context* ctx, uint32_t) { ctx->r[ins.dst.reg] += ctx->r[ins.src1.reg]; }
static void Exec_ADD_IMM(const sh4::ir::Instr& ins, Sh4Context* ctx, uint32_t) { ctx->r[ins.dst.reg] += static_cast<uint32_t>(ins.src1.imm); }
static void Exec_ADDC(const sh4::ir::Instr& ins, Sh4Context* ctx, uint32_t)
{
    uint64_t sum = static_cast<uint64_t>(ctx->r[ins.dst.reg]) + ctx->r[ins.src1.reg] + (ctx->sr.T & 1);
    ctx->r[ins.dst.reg] = static_cast<uint32_t>(sum);
    ctx->sr.T = (sum >> 32) & 1; // carry-out sets T
}

static ExecFn g_exec_table[static_cast<int>(sh4::ir::Op::NUM_OPS)]{};

static void InitExecTable()
{
    static bool init = false;
    if (init) return;
    for (auto& fn : g_exec_table) fn = &ExecStub;
    g_exec_table[static_cast<int>(sh4::ir::Op::NOP)]      = &Exec_NOP;
    g_exec_table[static_cast<int>(sh4::ir::Op::ADD_REG)]  = &Exec_ADD_REG;
    g_exec_table[static_cast<int>(sh4::ir::Op::ADD_IMM)]  = &Exec_ADD_IMM;
    g_exec_table[static_cast<int>(sh4::ir::Op::ADDC)]       = &Exec_ADDC;
    init = true;
}

// Static constructor to initialize table before main
struct ExecTableInit { ExecTableInit(){ InitExecTable(); } } g_exec_table_init;

static inline ExecFn GetExecFn(sh4::ir::Op op)
{
    return g_exec_table[static_cast<int>(op)];
}

// ----------------------------------------------------------------------------
//  Executor
// ----------------------------------------------------------------------------
void Executor::ExecuteBlock(const Block* blk, Sh4Context* ctx)
{
    assert(blk);
    size_t ip = 0;
    bool branch_pending = false;       // we have seen a branch, delay slot ahead
    bool executed_delay = false;       // delay slot has just been executed
    uint32_t branch_target = 0;
    while (ip < blk->code.size())
    {
        if (unlikely(g_exception_was_raised))
        {
            g_exception_was_raised = false;
            return;
        }
        // Current PC before executing this instruction
        uint32_t curr_pc = ctx->pc;
        if (curr_pc == 0)
        {
            ERROR_LOG(SH4, "*** PC reached 0! R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X R6=%08X R7=%08X R8=%08X R9=%08X R10=%08X R11=%08X R12=%08X R13=%08X R14=%08X R15=%08X PR=%08X",
                      ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7], ctx->r[8], ctx->r[9], ctx->r[10], ctx->r[11], ctx->r[12], ctx->r[13], ctx->r[14], ctx->r[15], ctx->pr);
            DumpTrace();
        }
        uint32_t old_pr = ctx->pr; // track PR modifications

        // No branch commit here; we defer committing the branch until after the
        // delay-slot instruction has executed (see logic at bottom of loop).

        const Instr& ins = blk->code[ip++];
        // --- early-boot tracing ------------------------------------------------
        static int boot_trace_lines = 0;
        if (boot_trace_lines < 256 &&                         // just limit spam
            ((curr_pc & 0xF0000000u) == 0xA0000000u ||        // P2 BIOS
             (curr_pc & 0xF0000000u) == 0xC0000000u) )        // P1 BIOS
        {
            INFO_LOG(SH4, "BOOT PC=%08X raw=%04X op=%s",
                     ins.pc, ins.raw, GetOpName(static_cast<size_t>(ins.op)));
            ++boot_trace_lines;
        }
        // ---- statistics & trace ----
        TraceLog(curr_pc, ins.op);
        g_opExecCounts[static_cast<size_t>(ins.op)]++;
        g_totalExecCount++;

        MaybeDumpStats();

        // Try fast table dispatch first
        {
            ExecFn fn = GetExecFn(ins.op);
            if (fn != &ExecStub)
            {
                fn(ins, ctx, curr_pc);
            }
            else
            {
                switch (ins.op)
                {
                case Op::END:
                    // Block finished, jump to the next one.
                    INFO_LOG(SH4, "BLOCK_END: AtPC:%08X (Op:END) PR:%08X SR.T:%d -> TargetNextPC:%08X", ctx->pc, ctx->pr, ctx->sr.T & 1, blk->pcNext);
                    SetPC(ctx, blk->pcNext, "block_end");
                    return;
                case Op::NOP:
                    break;
                case Op::MOV_REG:
                    ctx->r[ins.dst.reg] = ctx->r[ins.src1.reg];
                    INFO_LOG(SH4, "MOV_REG R%u -> R%u (val=%08X) at PC=%08X", ins.src1.reg, ins.dst.reg, ctx->r[ins.dst.reg], curr_pc);
                    if (ins.dst.reg == 0) INFO_LOG(SH4, "R0 updated to %08X", ctx->r[0]);
                    break;
                case Op::MOV_IMM:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(ins.src1.imm);
                    INFO_LOG(SH4, "MOV_IMM loaded %08X into R%u at PC=%08X", ins.src1.imm, ins.dst.reg, curr_pc);
                    if (ins.dst.reg == 0) INFO_LOG(SH4, "R0 updated to %08X", ctx->r[0]);
                    break;
                case Op::ADD_IMM:
                    ctx->r[ins.dst.reg] += static_cast<int32_t>(ins.src1.imm);
                    // TODO: set condition codes
                    break;
                case Op::SHL:
                    ctx->r[ins.dst.reg] <<= ins.extra & 31;
                    // TODO: set condition codes
                    break;
                case Op::SWAP_B:
                {
                    uint32_t v = ctx->r[ins.src1.reg];
                    uint32_t upper = v & 0xFFFF0000u;
                    uint32_t lower = v & 0x0000FFFFu;
                    uint32_t swapped = ((lower & 0x00FF) << 8) | ((lower & 0xFF00) >> 8);
                    ctx->r[ins.dst.reg] = upper | swapped;
                    break;
                }
                case Op::SWAP_W:
                {
                    uint32_t v = ctx->r[ins.src1.reg];
                    uint32_t swapped = (v << 16) | (v >> 16);
                    ctx->r[ins.dst.reg] = swapped;
                    break;
                }
                case Op::AND_REG:
                    ctx->r[ins.dst.reg] &= ctx->r[ins.src1.reg];
                    break;
                case Op::OR_REG:
                    ctx->r[ins.dst.reg] |= ctx->r[ins.src1.reg];
                    break;
                case Op::XOR_REG:
                    ctx->r[ins.dst.reg] ^= ctx->r[ins.src1.reg];
                    break;
                case Op::AND_IMM:
                    ctx->r[ins.dst.reg] &= static_cast<uint32_t>(ins.src1.imm);
                    break;
                case Op::OR_IMM:
                    ctx->r[ins.dst.reg] |= static_cast<uint32_t>(ins.src1.imm);
                    break;
                case Op::XOR_IMM:
                    ctx->r[ins.dst.reg] ^= static_cast<uint32_t>(ins.src1.imm);
                    break;
                case Op::NOT_OP:
                case Op::NOT:
                    ctx->r[ins.dst.reg] = ~ctx->r[ins.src1.reg];
                    break;
                case Op::SHL1:
                    ctx->r[ins.dst.reg] <<= 1;
                    break;
                case Op::SHLL:
                {
                    uint32_t& rn = ctx->r[ins.dst.reg];
                    ctx->sr.T = (rn >> 31) & 1;
                    rn <<= 1;
                    break;
                }
                case Op::SHR1:
                    ctx->r[ins.dst.reg] >>= 1;
                    break;
                case Op::SAR1:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int32_t>(ctx->r[ins.dst.reg]) >> 1);
                    break;
                case Op::SHR_OP:
                    if (ins.extra & 0x80) // rotate right 1
                    {
                        uint32_t v = ctx->r[ins.dst.reg];
                        ctx->r[ins.dst.reg] = (v >> 1) | (v << 31);
                    }
                    else
                    {
                        ctx->r[ins.dst.reg] >>= (ins.extra & 31);
                    }
                    break;
                case Op::SHLD:
                {
                    uint32_t cnt = ctx->r[ins.src1.reg] & 31;
                    if (cnt == 0)
                        ; // no change
                    else
                        ctx->r[ins.dst.reg] = (ctx->r[ins.dst.reg] << cnt) | (ctx->r[ins.src1.reg] >> (32 - cnt));
                    break;
                }
                case Op::SAR_OP:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int32_t>(ctx->r[ins.dst.reg]) >> (ins.extra & 31));
                    break;
                case Op::ADD_REG:
                    ctx->r[ins.dst.reg] += ctx->r[ins.src1.reg];
                    break;
                case Op::LOAD8:
                {
                    uint32_t addr;
                    // Current instruction's PC for logging
                    const uint32_t current_instr_pc = ins.pc;
                    if (!ins.src2.isImm && ins.src2.type != RegType::NONE) // MOV.B @(Rm,Rn),R0. Emitter: ins.src1.reg=Rm, ins.src2.reg=Rn, ins.src2.type should be GPR
                    {
                        addr = ctx->r[ins.src1.reg] + ctx->r[ins.src2.reg];
                        INFO_LOG(SH4, "IR_EXEC: LOAD8 @(R%d,R%d),R0. PC=0x%08X. R%d(base)=0x%08X, R%d(offs)=0x%08X, Addr=0x%08X",
                                 ins.src1.reg, ins.src2.reg, current_instr_pc,
                                 ins.src1.reg, ctx->r[ins.src1.reg],
                                 ins.src2.reg, ctx->r[ins.src2.reg], addr);
                    }
                    else // MOV.B @(disp,Rm),R0. Emitter: ins.src1.reg=Rm, ins.extra=disp (or ins.src2.imm for other forms)
                    {
                        // Assuming 'extra' is used for displacement by the emitter for this specific LOAD8 form.
                        // If other LOAD8 forms use src2.imm, that needs to be handled by emitter or here.
                        addr = ctx->r[ins.src1.reg] + ins.extra;
                        INFO_LOG(LogTypes::SH4, "IR_EXEC: LOAD8 @(0x%X,R%d),R0. PC=0x%08X. R%d(base)=0x%08X, Addr=0x%08X",
                                 ins.extra, ins.src1.reg, current_instr_pc,
                                 ins.src1.reg, ctx->r[ins.src1.reg], addr);
                    }

                    u8 val;
                    if (u8* p = FastRamPtr(addr))
                        val = *p;
                    else
                        val = mmu_ReadMem<u8>(addr);
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int8_t>(val)); // Sign-extend byte
                    break;
                }
                case Op::LOAD16:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + static_cast<uint32_t>(ins.extra);
                    u16 val;
                    if (u8* p = FastRamPtrWrite(addr))
                        val = *reinterpret_cast<u16*>(p);
                    else
                        val = mmu_ReadMem<u16>(addr);
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int16_t>(val));
                    break;
                }
                case Op::LOAD32:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtrWrite(addr))
                        ctx->r[ins.dst.reg] = *reinterpret_cast<u32*>(p);
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(addr);
                    break;
                }
                case Op::MOV_B_REG_PREDEC:
                {
                    uint32_t& rn = ctx->r[ins.dst.reg];
                    rn -= 1;
                    // Use standard MMU write to respect protection and avoid invalid host pointers.
                    mmu_WriteMem<u8>(rn, static_cast<u8>(ctx->r[ins.src1.reg]));
                    break;
                }
                case Op::LOAD8_GBR:
                {
                    u8 val = mmu_ReadMem<u8>(ctx->gbr + static_cast<uint32_t>(ins.extra));
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int8_t>(val));
                    break;
                }
                case Op::LOAD16_GBR:
                {
                    u16 val = mmu_ReadMem<u16>(ctx->gbr + static_cast<uint32_t>(ins.extra));
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int16_t>(val));
                    break;
                }
                case Op::LOAD32_GBR:
                    ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(ctx->gbr + static_cast<uint32_t>(ins.extra));
                    break;
                case Op::LOAD8_POST:
                {
                    uint32_t addr = ctx->r[ins.src1.reg];
                    u8 val;
                    if (u8* p = FastRamPtrWrite(addr))
                        val = *p;
                    else
                        val = mmu_ReadMem<u8>(addr);
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int8_t>(val));
                    ctx->r[ins.src1.reg] += 1;
                    break;
                }
                case Op::LOAD16_POST:
                {
                    uint32_t addr = ctx->r[ins.src1.reg];
                    u16 val;
                    if (u8* p = FastRamPtrWrite(addr))
                        val = *reinterpret_cast<u16*>(p);
                    else
                        val = mmu_ReadMem<u16>(addr);
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int16_t>(val));
                    ctx->r[ins.src1.reg] += 2;
                    break;
                }
                case Op::LOAD32_POST:
                {
                    uint32_t addr = ctx->r[ins.src1.reg];
                    if (u8* p = FastRamPtrWrite(addr))
                        ctx->r[ins.dst.reg] = *reinterpret_cast<u32*>(p);
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(addr);
                    ctx->r[ins.src1.reg] += 4;
                    break;
                }
                case Op::STORE8:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtrWrite(addr))
                        *p = static_cast<u8>(ctx->r[ins.src1.reg]);
                    else if (IsBiosAddr(addr)) {
                         LogIllegalBiosWrite(ins, addr, curr_pc);
                     } else
                         mmu_WriteMem<u8>(addr, ctx->r[ins.src1.reg]);
                    break;
                }
                case Op::STORE16:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtrWrite(addr))
                        *reinterpret_cast<u16*>(p) = static_cast<u16>(ctx->r[ins.src1.reg]);
                    else if (IsBiosAddr(addr)) {
                         LogIllegalBiosWrite(ins, addr, curr_pc);
                     } else
                         mmu_WriteMem<u16>(addr, static_cast<uint16_t>(ctx->r[ins.src1.reg]));
                    break;
                }
                case Op::STORE32:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtrWrite(addr))
                        *reinterpret_cast<u32*>(p) = ctx->r[ins.src1.reg];
                    else if (IsBiosAddr(addr)) {
                         LogIllegalBiosWrite(ins, addr, curr_pc);
                     } else
                         mmu_WriteMem<u32>(addr, ctx->r[ins.src1.reg]);
                    break;
                }
                case Op::STORE8_POST:
                {
                    uint32_t addr = ctx->r[ins.dst.reg];
                    if (u8* p = FastRamPtrWrite(addr))
                        *p = static_cast<u8>(ctx->r[ins.src1.reg]);
                    else if (IsBiosAddr(addr)) {
                         LogIllegalBiosWrite(ins, addr, curr_pc);
                     } else
                         mmu_WriteMem<u8>(addr, ctx->r[ins.src1.reg]);
                    ctx->r[ins.dst.reg] += 1;
                    break;
                }
                case Op::STORE16_POST:
                {
                    uint32_t addr = ctx->r[ins.dst.reg];
                    if (u8* p = FastRamPtrWrite(addr))
                        *reinterpret_cast<u16*>(p) = static_cast<u16>(ctx->r[ins.src1.reg]);
                    else if (IsBiosAddr(addr)) {
                         LogIllegalBiosWrite(ins, addr, curr_pc);
                     } else
                         mmu_WriteMem<u16>(addr, static_cast<uint16_t>(ctx->r[ins.src1.reg]));
                    ctx->r[ins.dst.reg] += 2;
                    break;
                }
                case Op::STORE32_POST:
                {
                    uint32_t addr = ctx->r[ins.dst.reg];
                    if (u8* p = FastRamPtrWrite(addr))
                        *reinterpret_cast<u32*>(p) = ctx->r[ins.src1.reg];
                    else if (IsBiosAddr(addr)) {
                         LogIllegalBiosWrite(ins, addr, curr_pc);
                     } else
                         mmu_WriteMem<u32>(addr, ctx->r[ins.src1.reg]);
                    ctx->r[ins.dst.reg] += 4;
                    break;
                }
                case Op::STORE8_GBR:
                    if (!IsBiosAddr(ctx->gbr + static_cast<uint32_t>(ins.extra)))
                        mmu_WriteMem<u8>(ctx->gbr + static_cast<uint32_t>(ins.extra), ctx->r[ins.src1.reg]);
                    break;
                case Op::STORE16_GBR:
                    if (!IsBiosAddr(ctx->gbr + static_cast<uint32_t>(ins.extra)))
                        mmu_WriteMem<u16>(ctx->gbr + static_cast<uint32_t>(ins.extra), static_cast<uint16_t>(ctx->r[ins.src1.reg]));
                    break;
                case Op::STORE32_GBR:
                    if (!IsBiosAddr(ctx->gbr + static_cast<uint32_t>(ins.extra)))
                        mmu_WriteMem<u32>(ctx->gbr + static_cast<uint32_t>(ins.extra), ctx->r[ins.src1.reg]);
                    break;
                case Op::STORE8_R0:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + ctx->r[0];
                    if (u8* p = FastRamPtrWrite(addr))
                        *p = static_cast<u8>(ctx->r[ins.src1.reg]);
                    else if (IsBiosAddr(addr)) {
                         LogIllegalBiosWrite(ins, addr, curr_pc);
                     } else
                         mmu_WriteMem<u8>(addr, ctx->r[ins.src1.reg]);
                    break;
                }
                case Op::STORE16_R0:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + ctx->r[0];
                    if (u8* p = FastRamPtrWrite(addr))
                        *reinterpret_cast<u16*>(p) = static_cast<u16>(ctx->r[ins.src1.reg]);
                    else
                        mmu_WriteMem<u16>(addr, static_cast<uint16_t>(ctx->r[ins.src1.reg]));
                    break;
                }
                case Op::STORE32_R0:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + ctx->r[0];
                    if (u8* p = FastRamPtrWrite(addr))
                        *reinterpret_cast<u32*>(p) = ctx->r[ins.src1.reg];
                    else
                        mmu_WriteMem<u32>(addr, ctx->r[ins.src1.reg]);
                    break;
                }
                case Op::LOAD8_R0:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + ctx->r[0];
                    u8 val;
                    if (u8* p = FastRamPtrWrite(addr))
                        val = *p;
                    else
                        val = mmu_ReadMem<u8>(addr);
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int8_t>(val));
                    break;
                }
                case Op::LOAD16_R0:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + ctx->r[0];
                    u16 val;
                    if (u8* p = FastRamPtrWrite(addr))
                        val = *reinterpret_cast<u16*>(p);
                    else
                        val = mmu_ReadMem<u16>(addr);
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int16_t>(val));
                    break;
                }
                case Op::LOAD32_R0:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + ctx->r[0];
                    if (u8* p = FastRamPtrWrite(addr))
                        ctx->r[ins.dst.reg] = *reinterpret_cast<u32*>(p);
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(addr);
                    break;
                }

                    mmu_WriteMem<u32>(ctx->gbr + static_cast<uint32_t>(ins.extra), ctx->r[ins.src1.reg]);
                    break;
                case Op::LOAD16_IMM:
                {
                    u16 val = mmu_ReadMem<u16>(static_cast<uint32_t>(ins.src1.imm));
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int16_t>(val));
                    break;
                }
                case Op::LOAD32_IMM:
                    ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(static_cast<uint32_t>(ins.src1.imm));
                    break;
                case Op::JSR:
                    INFO_LOG(SH4, "BR JSR from %08X -> %08X (r%u)", curr_pc, ctx->r[ins.src1.reg], ins.src1.reg);
                    if (IsTopRegion(ctx->r[ins.src1.reg]))
                        ERROR_LOG(SH4, "*** HIGH-FF JSR target at %08X : R%u=%08X", curr_pc, ins.src1.reg, ctx->r[ins.src1.reg]);
                    ctx->pr = curr_pc + 4; // address after delay slot
                    branch_target = ctx->r[ins.src1.reg];
                    if (IsTopRegion(branch_target))
                        ERROR_LOG(SH4, "*** HIGH-FF branch target set by JMP at %08X -> %08X (r%u)", curr_pc, branch_target, ins.src1.reg);
                    branch_pending = true;
                    executed_delay = false; // ensure delay flag reset
                    break;
                case Op::JMP:
                    // Dump full GPR set for debugging the reset jump
                    INFO_LOG(SH4, "JMP @R%u at %08X  R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X R6=%08X R7=%08X",
                             ins.src1.reg, curr_pc,
                             ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3],
                             ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7]);
                    INFO_LOG(SH4, "                      R8=%08X R9=%08X R10=%08X R11=%08X R12=%08X R13=%08X R14=%08X R15=%08X",
                             ctx->r[8], ctx->r[9], ctx->r[10], ctx->r[11],
                             ctx->r[12], ctx->r[13], ctx->r[14], ctx->r[15]);
                    INFO_LOG(SH4, "BR JMP from %08X -> %08X (r%u)", curr_pc, ctx->r[ins.src1.reg], ins.src1.reg);
                    if (IsTopRegion(ctx->r[ins.src1.reg]))
                        ERROR_LOG(SH4, "*** HIGH-FF JMP target at %08X : R%u=%08X", curr_pc, ins.src1.reg, ctx->r[ins.src1.reg]);
                    branch_target = ctx->r[ins.src1.reg];
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::RTS:
                    INFO_LOG(SH4, "BR RTS from %08X -> %08X", curr_pc, ctx->pr);
                    branch_target = ctx->pr;
                    if (IsTopRegion(branch_target))
                        ERROR_LOG(SH4, "*** HIGH-FF RTS target at %08X -> %08X (PR)", curr_pc, branch_target);
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BRA:
                    INFO_LOG(SH4, "BR BRA from %08X -> %08X (disp=%d)", curr_pc, curr_pc + 4 + ins.extra, ins.extra);
                    branch_target = curr_pc + 4 + ins.extra;
                    if (IsTopRegion(branch_target))
                        ERROR_LOG(SH4, "*** HIGH-FF BRA target at %08X -> %08X (disp=%d)", curr_pc, branch_target, ins.extra);
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BT:
                    if (ctx->sr.T)
                    {
                        uint32_t target = curr_pc + 4 + ins.extra;
                        INFO_LOG(SH4, "BR BT  from %08X -> %08X (disp=%d)", curr_pc, target, ins.extra);
                        if (IsTopRegion(target))
                            ERROR_LOG(SH4, "*** HIGH-FF BT target at %08X -> %08X (disp=%d)", curr_pc, target, ins.extra);
                        // No delay slot: set PC to target immediately (account for +2 at loop bottom)
                        SetPC(ctx, target - 2, "BT/BF");
                    }
                    break;
                case Op::BF:
                    if (!ctx->sr.T)
                    {
                        uint32_t target = curr_pc + 4 + ins.extra;
                        INFO_LOG(SH4, "BR BF  from %08X -> %08X (disp=%d)", curr_pc, target, ins.extra);
                        if (IsTopRegion(target))
                            ERROR_LOG(SH4, "*** HIGH-FF BF target at %08X -> %08X (disp=%d)", curr_pc, target, ins.extra);
                        SetPC(ctx, target - 2, "BT/BF");
                    }
                    break;
                case Op::CMP_PL:
                    ctx->sr.T = ((static_cast<int32_t>(ctx->r[ins.src1.reg]) > 0) ? 1 : 0);
                    break;
                case Op::TST_IMM:
                    ctx->sr.T = ((ctx->r[0] & static_cast<uint32_t>(ins.src1.imm)) == 0);
                    break;
                case Op::TST_REG:
                    ctx->sr.T = ((ctx->r[ins.dst.reg] & ctx->r[ins.src1.reg]) == 0);
                    break;
                case Op::MOVT:
                    ctx->r[ins.dst.reg] = ctx->sr.T;
                    break;
                case Op::CMP_EQ:
                    ctx->sr.T = (ctx->r[ins.dst.reg] == ctx->r[ins.src1.reg]);
                    break;
                case Op::CMP_HI:
                    ctx->sr.T = (ctx->r[ins.dst.reg] > ctx->r[ins.src1.reg]);
                    break;
                case Op::CMP_HS:
                    ctx->sr.T = (ctx->r[ins.dst.reg] >= ctx->r[ins.src1.reg]);
                    break;
                case Op::CMP_GE:
                    ctx->sr.T = (static_cast<int32_t>(ctx->r[ins.dst.reg]) >= static_cast<int32_t>(ctx->r[ins.src1.reg]));
                    break;
                case Op::CMP_GT:
                    ctx->sr.T = (static_cast<int32_t>(ctx->r[ins.dst.reg]) > static_cast<int32_t>(ctx->r[ins.src1.reg]));
                    break;
                case Op::CMP_STR:
                {
                    uint32_t v = ctx->r[ins.dst.reg] ^ ctx->r[ins.src1.reg];
                    bool match = ((v & 0x000000FFu) == 0) || ((v & 0x0000FF00u) == 0) || ((v & 0x00FF0000u) == 0) || ((v & 0xFF000000u) == 0);
                    ctx->sr.T = match;
                    break;
                }
                case Op::GET_MACH:
                    ctx->r[ins.dst.reg] = ctx->mac.h;
                    break;
                case Op::GET_MACL:
                    ctx->r[ins.dst.reg] = ctx->mac.l;
                    break;
                case Op::GET_PR:
                    ctx->r[ins.dst.reg] = ctx->pr;
                    break;
                case Op::MOVA:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(ins.src1.imm);
                    {
                        uint32_t imm = static_cast<uint32_t>(ins.src1.imm);
                        uint32_t base = (curr_pc & ~3u) + 4u;
                        uint32_t disp_calc = (imm - base) >> 2;
                        uint32_t lit = 0xDEADBEEF;
                        if (imm < 0x00200000)
                            lit = *reinterpret_cast<const u32*>(nvmem::getBiosData() + (imm & 0x001FFFFF));
                        DEBUG_LOG(SH4, "MOVA disp=%02X addr=%08X literal=%08X at PC=%08X", disp_calc & 0xFFu, imm, lit, curr_pc);
                    }
                    break;
                case Op::SUB:
                    ctx->r[ins.dst.reg] -= ctx->r[ins.src1.reg];
                    break;
                case Op::SUBV: // Rn = Rn - Rm, set T on signed overflow
                {
                    int32_t rn = static_cast<int32_t>(ctx->r[ins.dst.reg]);
                    int32_t rm = static_cast<int32_t>(ctx->r[ins.src1.reg]);
                    int32_t res = rn - rm;
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(res);
                    // overflow occurs if operands have different signs and sign of result differs from sign of Rn
                    uint32_t ov = ((rn ^ rm) & (rn ^ res)) >> 31;
                    ctx->sr.T = ov & 1;
                    break;
                }
                case Op::NEG:
                    ctx->r[ins.dst.reg] = -static_cast<int32_t>(ctx->r[ins.src1.reg]);
                    break;
                case Op::EXTU_B:
                    ctx->r[ins.dst.reg] = ctx->r[ins.src1.reg] & 0xFFu;
                    break;
                case Op::EXTU_W:
                    ctx->r[ins.dst.reg] = ctx->r[ins.src1.reg] & 0xFFFFu;
                    break;
                case Op::EXTS_B:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int8_t>(ctx->r[ins.src1.reg] & 0xFFu));
                    break;
                case Op::EXTS_W:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int16_t>(ctx->r[ins.src1.reg] & 0xFFFFu));
                    break;
                case Op::ADDC:
                {
                    uint64_t sum = static_cast<uint64_t>(ctx->r[ins.dst.reg]) + ctx->r[ins.src1.reg] + (ctx->sr.T & 1);
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(sum);
                    ctx->sr.T = (sum >> 32) & 1;
                    break;
                }
                case Op::BSR:
                    INFO_LOG(SH4, "BR BSR from %08X -> %08X (disp=%d)", curr_pc, curr_pc + 4 + ins.extra, ins.extra);
                    ctx->pr = curr_pc + 4; // return address after delay slot
                    branch_target = curr_pc + 4 + ins.extra;
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BRAF:
                    INFO_LOG(SH4, "BR BRAF from %08X -> %08X (r%u)", curr_pc, curr_pc + 4 + ctx->r[ins.src1.reg], ins.src1.reg);
                    branch_target = curr_pc + 4 + ctx->r[ins.src1.reg];
                    if (IsTopRegion(branch_target))
                        ERROR_LOG(SH4, "*** HIGH-FF BRAF target at %08X : target=%08X R%u=%08X", curr_pc, branch_target, ins.src1.reg, ctx->r[ins.src1.reg]);
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BSRF:
                    INFO_LOG(SH4, "BR BSRF from %08X -> %08X (r%u)", curr_pc, curr_pc + 4 + ctx->r[ins.src1.reg], ins.src1.reg);
                    ctx->pr = curr_pc + 4;
                    branch_target = curr_pc + 4 + ctx->r[ins.src1.reg];
                    if (IsTopRegion(branch_target))
                        ERROR_LOG(SH4, "*** HIGH-FF BSRF target at %08X : target=%08X R%u=%08X", curr_pc, branch_target, ins.src1.reg, ctx->r[ins.src1.reg]);
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BT_S:
                    if (ctx->sr.T)
                    {
                        INFO_LOG(SH4, "BR BT/S from %08X -> %08X (disp=%d)", curr_pc, curr_pc + 4 + ins.extra, ins.extra);
                        branch_target = curr_pc + 4 + ins.extra;
                        if (IsTopRegion(branch_target))
                            ERROR_LOG(SH4, "*** HIGH-FF BT/S target at %08X -> %08X (disp=%d)", curr_pc, branch_target, ins.extra);
                        branch_pending = true;
                        executed_delay = false;
                    }
                    break;
                case Op::BF_S:
                    if (!ctx->sr.T)
                    {
                        uint32_t target = curr_pc + 4 + ins.extra;
                        INFO_LOG(SH4, "BR BF/S from %08X -> %08X (disp=%d)", curr_pc, target, ins.extra);
                        if (IsTopRegion(target))
                            ERROR_LOG(SH4, "*** HIGH-FF BF/S target at %08X -> %08X (disp=%d)", curr_pc, target, ins.extra);
                        branch_target = target;
                        branch_pending = true;
                        executed_delay = false;
                    }
                    break;
                case Op::LDS_PR_L:
                    ctx->pr = mmu_ReadMem<u32>(ctx->r[ins.src1.reg]);
                    INFO_LOG(SH4, "LDS.L  PR @%08X -> %08X", ctx->r[ins.src1.reg], ctx->pr);
                    if (IsTopRegion(ctx->pr))
                        ERROR_LOG(SH4, "*** HIGH-FF PR value loaded %08X via LDS.L at PC=%08X", ctx->pr, curr_pc);
                    ctx->r[ins.src1.reg] += 4;
                    break;
                case Op::STS_PR_L:
                {
                    uint32_t new_addr = ctx->r[ins.dst.reg] - 4;
                    ctx->r[ins.dst.reg] = new_addr;
                    mmu_WriteMem<u32>(new_addr, ctx->pr);
                    INFO_LOG(SH4, "STS.L  PR @%08X  PR=%08X", new_addr, ctx->pr);
                    if (IsTopRegion(ctx->pr))
                        ERROR_LOG(SH4, "*** HIGH-FF PR value stored %08X via STS.L at PC=%08X", ctx->pr, curr_pc);
                    break;
                }
                case Op::LDC_SR_L: // LDC.L @Rm+, SR
                {
                    uint8_t rm_idx = ins.src1.reg;
                    uint32_t addr = ctx->r[rm_idx];
                    uint32_t value = mmu_ReadMem<u32>(addr);
                    ctx->sr.setFull(value);
                    UpdateSR(); // Apply SR changes (interrupts, mode, etc.)
                    ctx->r[rm_idx] += 4;
                    INFO_LOG(SH4, "LDC.L SR <- %08X from @%08X (R%u) at PC=%08X", value, addr, rm_idx, curr_pc);
                    break;
                }
                case Op::RTE:
                    INFO_LOG(SH4, "RTE from %08X -> %08X", curr_pc, ctx->spc);
                    if (IsTopRegion(ctx->spc))
                        ERROR_LOG(SH4, "*** HIGH-FF RTE target at %08X -> %08X", curr_pc, ctx->spc);
                    ctx->sr.setFull(ctx->ssr);
                    UpdateSR();
                    branch_target = ctx->spc;
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::LDC_SSR_L:
                    ctx->ssr = mmu_ReadMem<u32>(ctx->r[ins.src1.reg]);
                    ctx->r[ins.src1.reg] += 4;
                    break;
                case Op::LDC_SPC_L:
                {
                    uint32_t val = mmu_ReadMem<u32>(ctx->r[ins.src1.reg]);
                    INFO_LOG(SH4, "LDC.L  SPC <- %08X from @%08X (R%u)", val, ctx->r[ins.src1.reg], ins.src1.reg);
                    if (IsTopRegion(val))
                        ERROR_LOG(SH4, "*** HIGH-FF SPC value loaded %08X via LDC.L at PC=%08X", val, curr_pc);
                    ctx->spc = val;
                    // ctx->sr.Set(ctx->r[ins.src1.reg]); // This was incorrect for LDC_SPC_L
                    break;
                }
                case Op::STC: // STC <CR>, Rn
                {
                    uint32_t val_to_store = 0;
                    // Emitter sets ins.extra:
                    // 0:SR, 1:GBR, 2:VBR, 3:SSR, 4:SPC
                    // 0xF (15): DBR
                    // 8-14: R0_BANK-R6_BANK (extra = 8 + bank_idx)
                    // 15: R7_BANK (extra = 8 + 7, distinct from DBR's 0xF due to emitter order)
                    switch (ins.extra) {
                        case 0:  val_to_store = ctx->sr.getFull(); break; // Use getFull()
                        case 1:  val_to_store = ctx->gbr; break;
                        case 2:  val_to_store = ctx->vbr; break;
                        case 3:  val_to_store = ctx->ssr; break;
                        case 4:  val_to_store = ctx->spc; break;
                        case 7:  val_to_store = ctx->dbr; break; // DBR (emitter now uses 7)
                        case 8:  val_to_store = ctx->r_bank[0]; break; // R0_BANK
                        case 9:  val_to_store = ctx->r_bank[1]; break; // R1_BANK
                        case 10: val_to_store = ctx->r_bank[2]; break; // R2_BANK
                        case 11: val_to_store = ctx->r_bank[3]; break; // R3_BANK
                        case 12: val_to_store = ctx->r_bank[4]; break; // R4_BANK
                        case 13: val_to_store = ctx->r_bank[5]; break; // R5_BANK
                        case 14: val_to_store = ctx->r_bank[6]; break; // R6_BANK
                        case 15: val_to_store = ctx->r_bank[7]; break; // R7_BANK
                        default:
                            ERROR_LOG(SH4, "STC: Unhandled ins.extra=0x%X for Rn=R%d at PC=0x%08X", ins.extra, ins.dst.reg, curr_pc);
                            throw SH4ThrownException(curr_pc, Sh4Ex_IllegalInstr);
                    }
                    ctx->r[ins.dst.reg] = val_to_store;
                    DEBUG_LOG(SH4, "Executor: STC [extra=0x%X] -> R%d (val=0x%08X) at PC=0x%08X", ins.extra, ins.dst.reg, val_to_store, curr_pc);
                    break;
                }
                case Op::LDC: // LDC Rm, <CR>
                {
                    uint32_t val_to_load = ctx->r[ins.src1.reg]; // Value from Rm
                    // Emitter for LDC (0x4mcE) sets ins.extra:
                    // c_val (middle nibble of opcode) if not Rn_BANK: 0:SR, 1:GBR, 2:VBR, 3:SSR, 4:SPC, 6:SGR, 7:DBR
                    // 8 + bank_idx (0-7) if Rn_BANK (c_val=5): 8:R0_BANK .. 15:R7_BANK
                    DEBUG_LOG(SH4, "Executor: LDC R%d (val=0x%08X) -> [extra=0x%X] at PC=0x%08X", ins.src1.reg, val_to_load, ins.extra, curr_pc);

                    switch (ins.extra) {
                        case 0: // SR
                            ctx->sr.setFull(val_to_load); // Use setFull()
                            UpdateSR();
                            break;
                        case 1: // GBR
                            ctx->gbr = val_to_load;
                            break;
                        case 2: // VBR
                            ctx->vbr = val_to_load;
                            break;
                        case 3: // SSR
                            ctx->ssr = val_to_load;
                            break;
                        case 4: // SPC
                            if (IsTopRegion(val_to_load))
                                ERROR_LOG(SH4, "*** HIGH-FF SPC value %08X loaded via LDC from R%u at PC=%08X", val_to_load, ins.src1.reg, curr_pc);
                            ctx->spc = val_to_load;
                            break;
                        case 6: // SGR
                            ctx->sgr = val_to_load;
                            break;
                        case 7: // DBR
                            ctx->dbr = val_to_load;
                            break;
                        // R0_BANK to R7_BANK
                        case 8: ctx->r_bank[0] = val_to_load; break;
                        case 9: ctx->r_bank[1] = val_to_load; break;
                        case 10: ctx->r_bank[2] = val_to_load; break;
                        case 11: ctx->r_bank[3] = val_to_load; break;
                        case 12: ctx->r_bank[4] = val_to_load; break;
                        case 13: ctx->r_bank[5] = val_to_load; break;
                        case 14: ctx->r_bank[6] = val_to_load; break;
                        case 15: ctx->r_bank[7] = val_to_load; break;
                        default:
                            ERROR_LOG(SH4, "LDC: Unhandled ins.extra=0x%X for Rm=R%d at PC=0x%08X", ins.extra, ins.src1.reg, curr_pc);
                            // Consider throwing an exception for truly unhandled CRs if strictness is desired.
                            break;
                    }
                    break;
                }

                case Op::LDTLB:
                {
                    // Mirror interpreter behaviour: load current PTE registers into UTLB[URC]
                    UTLB[CCN_MMUCR.URC].Data       = CCN_PTEL;
                    UTLB[CCN_MMUCR.URC].Address    = CCN_PTEH;
                    UTLB[CCN_MMUCR.URC].Assistance = CCN_PTEA;
                    UTLB_Sync(CCN_MMUCR.URC);
                    break;
                }
                case Op::DT: // DT Rn (R[n]--; T = (R[n]==0))
                    ctx->r[ins.dst.reg]--;
                    ctx->sr.T = (ctx->r[ins.dst.reg] == 0);
                    break;
                case Op::FADD:
                    ctx->fr[ins.dst.reg] = ctx->fr[ins.dst.reg] + ctx->fr[ins.src1.reg];
                    break;
                case Op::FSUB:
                    ctx->fr[ins.dst.reg] = ctx->fr[ins.dst.reg] - ctx->fr[ins.src1.reg];
                    break;
                case Op::FMUL:
                    ctx->fr[ins.dst.reg] = ctx->fr[ins.dst.reg] * ctx->fr[ins.src1.reg];
                    break;
                case Op::FDIV:
                    ctx->fr[ins.dst.reg] = ctx->fr[ins.dst.reg] / ctx->fr[ins.src1.reg];
                    break;
                case Op::FSQRT:
                    ctx->fr[ins.dst.reg] = std::sqrtf(ctx->fr[ins.dst.reg]);
                    break;
                case Op::FSTS: // FSTS FPUL,FRn
                    ctx->fr[ins.src1.reg] = ctx->fpul;
                    break;
                case Op::FABS:
                    ctx->fr[ins.dst.reg] = std::fabsf(ctx->fr[ins.src1.reg]);
                    break;
                case Op::FLDS: // Move FRm -> FPUL (store as int bits)
                    ctx->fpul = *reinterpret_cast<uint32_t*>(&ctx->fr[ins.src1.reg]);
                    break;
                case Op::FLDI0:
                    ctx->fr[ins.dst.reg] = 0.0f;
                    break;
                case Op::FLDI1:
                    ctx->fr[ins.dst.reg] = 1.0f;
                    break;
                case Op::FTRC: // Truncate float to int, store in FPUL
                {
                    float f = ctx->fr[ins.src1.reg];
                    ctx->fpul = static_cast<uint32_t>(static_cast<int32_t>(f));
                    break;
                }
                case Op::FNEG:
                    ctx->fr[ins.dst.reg] = -ctx->fr[ins.src1.reg];
                    break;
                case Op::FRCHG:
                    // Toggle FR bit (bit 21) in FPSCR
                    ctx->fpscr.full ^= (1 << 21);
                    Sh4Context::UpdateFPSCR(ctx); // Update FPSCR after modification
                    break;
                case Op::FCMP_EQ:
                    ctx->sr.T = (ctx->fr[ins.dst.reg] == ctx->fr[ins.src1.reg]);
                    break;
                case Op::FCMP_GT:
                    ctx->sr.T = (ctx->fr[ins.dst.reg] > ctx->fr[ins.src1.reg]);
                    break;
                case Op::LOAD32_PC:
                {
                    uint32_t disp8 = static_cast<uint32_t>(ins.extra);
                    uint32_t base = (curr_pc & ~3u) + 4u;
                    uint32_t mem_addr = base + (disp8 << 2);
                    uint32_t val;
                    if (u8* p = FastRamPtr(mem_addr))
                        val = *reinterpret_cast<u32*>(p);
                    else
                        val = mmu_ReadMem<u32>(mem_addr);
                    ctx->r[ins.dst.reg] = val;
                    if (ins.dst.reg == 0) {
                        INFO_LOG(SH4, "LOAD32_PC: Loaded 0x%08X into R0 from addr 0x%08X (PC=%08X, disp=%d)", val, mem_addr, curr_pc, disp8);
                    }
                    if (ins.dst.reg == 0) INFO_LOG(SH4, "R0 updated to %08X", ctx->r[0]);
                    break;
                }
                case Op::MOVA_PC:
                {
                    uint32_t disp8 = static_cast<uint32_t>(ins.extra);
                    uint32_t base = (curr_pc & ~3u) + 4u;
                    uint32_t effective_address = base + (disp8 << 2);
                    ctx->r[0] = effective_address; // MOVA @(disp,PC),R0 always targets R0
                    DEBUG_LOG(SH4, "MOVA_PC disp=%02X base_pc=%08X -> effective_addr=%08X into R0 at PC=%08X", disp8, base - 4, effective_address, curr_pc);
                    break;
                }
                case Op::FMOV_LOAD_R0:
                {
                    uint32_t addr = ctx->r[0] + ctx->r[ins.src1.reg];
                    uint32_t val;
                    if (u8* p = FastRamPtrWrite(addr))
                        val = *reinterpret_cast<u32*>(p);
                    else
                        val = mmu_ReadMem<u32>(addr);
                    ctx->fr[ins.dst.reg] = *reinterpret_cast<float*>(&val);
                    break;
                }
                case Op::FMOV_STORE_R0:
                {
                    uint32_t addr = ctx->r[0] + ctx->r[ins.dst.reg];
                    uint32_t val = *reinterpret_cast<u32*>(&ctx->fr[ins.src1.reg]);
                    if (u8* p = FastRamPtrWrite(addr))
                        *reinterpret_cast<u32*>(p) = val;
                    else
                        mmu_WriteMem<u32>(addr, val);
                    break;
                }
                case Op::CLRMAC:
                    ctx->mac.h = 0;
                    ctx->mac.l = 0;
                    break;
                case Op::LDC_L: // LDC.L @Rm+, <CR>
                {
                    uint32_t addr = ctx->r[ins.src1.reg];
                    uint32_t val = mmu_ReadMem<u32>(addr);
                    ctx->r[ins.src1.reg] += 4;

                    // ins.extra contains the control register ID
                    // 0:SR, 1:GBR, 2:VBR, 3:SSR, 4:SPC, (5:Rn_BANK - not used by LDC.L),
                    // 6:SGR, 7:DBR
                    switch (ins.extra) {
                        case 0: // SR
                            INFO_LOG(SH4, "LDC.L SR <- %08X from @%08X (R%u) at PC=%08X", val, addr, ins.src1.reg, curr_pc);
                            ctx->sr.setFull(val);
                            UpdateSR(); // Essential after SR change
                            break;
                        case 1: // GBR
                            INFO_LOG(SH4, "LDC.L GBR <- %08X from @%08X (R%u) at PC=%08X", val, addr, ins.src1.reg, curr_pc);
                            ctx->gbr = val;
                            break;
                        case 2: // VBR
                            INFO_LOG(SH4, "LDC.L VBR <- %08X from @%08X (R%u) at PC=%08X", val, addr, ins.src1.reg, curr_pc);
                            ctx->vbr = val;
                            break;
                        case 3: // SSR
                            INFO_LOG(SH4, "LDC.L SSR <- %08X from @%08X (R%u) at PC=%08X", val, addr, ins.src1.reg, curr_pc);
                            ctx->ssr = val;
                            break;
                        case 4: // SPC
                            INFO_LOG(SH4, "LDC.L SPC <- %08X from @%08X (R%u) at PC=%08X", val, addr, ins.src1.reg, curr_pc);
                            ctx->spc = val;
                            break;
                        // TODO: Add cases for SGR (6) and DBR (7) if they become necessary
                        default:
                            ERROR_LOG(SH4, "LDC.L to unhandled CR id %d (val %08X) from @%08X (R%u) at PC=%08X", ins.extra, val, addr, ins.src1.reg, curr_pc);
                            // Consider throwing an exception for truly unhandled CRs if strictness is desired.
                            break;
                    }
                    break;
                }
                // FPU Operations - many will fall through to interpreter for now
                case Op::FMOV:
                {
                    if (ins.dst.type == RegType::FGR && ins.src1.type == RegType::FGR) {
                        // Register-to-register move: FRn <- FRm
                        ctx->fr[ins.dst.reg] = ctx->fr[ins.src1.reg];
                    } else {
                        // Memory load variants. Currently we only need @Rm+ -> FRn.
                        uint32_t addr = ctx->r[ins.src1.reg];
                        uint32_t val = mmu_ReadMem<u32>(addr);
                        ctx->fr[ins.dst.reg] = *reinterpret_cast<float*>(&val);
                        if (ins.extra == 1) // post-increment flag set by emitter
                            ctx->r[ins.src1.reg] += 4;
                    }
                    break;
                }
                case Op::ILLEGAL:
                      {
                          uint16_t raw16 = mmu_ReadMem<u16>(curr_pc);
                          if (Sh4Interpreter::Instance)
                          {
                              INFO_LOG(SH4, "IR executor delegating ILLEGAL raw=%04X at PC=%08X to interpreter", raw16, curr_pc);
                              Sh4Interpreter::Instance->ExecuteOpcode(raw16);
                              return;
                          }
                          ERROR_LOG(SH4, "ILLEGAL opcode raw=%04X at PC=%08X with no interpreter fallback", raw16, curr_pc);
                          sh4::ir::DumpTrace();
                          throw SH4ThrownException(curr_pc, Sh4Ex_IllegalInstr);
                      }
                default:
                      {
                          uint16_t raw16 = mmu_ReadMem<u16>(curr_pc);
                          // If this is an FPU group opcode (0xFxxx) and interpreter is available, use fallback
                          if ((raw16 & 0xF000) == 0xF000 && Sh4Interpreter::Instance)
                          {
                              DEBUG_LOG(SH4, "IR fallback to interpreter FPU for raw=%04X at PC=%08X", raw16, curr_pc);
                              Sh4Interpreter::Instance->ExecuteOpcode(raw16); // advances PC internally
                              return; // leave block; new block will be fetched next tick
                          }
                          ERROR_LOG(SH4, "IR executor fell through: raw=%04X pc=%08X", raw16, curr_pc);
                          ERROR_LOG(SH4, "IR executor unimplemented opcode %s (%zu) at %08X", GetOpName(static_cast<size_t>(ins.op)), static_cast<size_t>(ins.op), curr_pc);
                          sh4::ir::DumpTrace();
                          throw SH4ThrownException(curr_pc, Sh4Ex_IllegalInstr);
                      }
                } // end switch (ins.op)
            } // end else dispatch
        } // end else dispatch

        // PR write tracking – log any change made by the executed instruction
        if (ctx->pr != old_pr)
        {
            INFO_LOG(SH4, "PR write: %08X -> %08X at PC=%08X op=%s",
                     old_pr, ctx->pr, curr_pc, GetOpName(static_cast<size_t>(ins.op)));
        }

        // --------- Idle filler detection ---------------------------------------
        static int idle_run = 0;
        bool is_idle_op = (ins.op == Op::NOP || ins.op == Op::END);
        if (is_idle_op && !branch_pending && ctx->pr == old_pr)
        {
            ++idle_run;
            if (idle_run > 8)
            {
                ERROR_LOG(SH4, "*** Executed %d consecutive END/NOP from %08X – likely ran off real code", idle_run, curr_pc);
                sh4::ir::DumpTrace();
                throw SH4ThrownException(curr_pc, Sh4Ex_IllegalInstr);
            }
        }
        else
        {
            idle_run = 0;
        }

        // Advance PC by 2 for the next sequential instruction; this positions the
        // delay-slot instruction (if any) at pc+2. If a branch is pending, the
        // commit step at the top of the next iteration will overwrite pc with
        // the branch target after the delay slot has run.
        // ctx->pc here is the PC of the instruction just executed.
        INFO_LOG(SH4, "SEQUENTIAL_ADVANCE: AtPC:%08X PR:%08X SR.T:%d -> TargetNextPC:%08X", ctx->pc, ctx->pr, ctx->sr.T & 1, ctx->pc + 2);
        SetPC(ctx, ctx->pc + 2, "seq");

        // Branch delay-slot/commit bookkeeping ----------------------------------
        // If a branch is pending we need to keep track of whether we have just
        // executed its delay-slot instruction. The first iteration where
        // branch_pending is set is the branch instruction itself; the very next
        // iteration is the delay slot. We therefore:
        //  • set executed_delay ("have executed delay slot") after the branch
        //    instruction (first iteration when branch_pending becomes true),
        //  • on the following iteration (executed_delay already true) commit the
        //    branch and finish the block.

        if (branch_pending)
        {
            if (executed_delay)
            {
                // Delay slot just executed – about to commit the branch.
                INFO_LOG(SH4, "BR COMMIT %08X", branch_target);
                uint16_t raw16_prev = mmu_ReadMem<u16>(curr_pc - 2);
                if (branch_target == 0)
                {
                    ERROR_LOG(SH4, "*** ZERO-TARGET commit! branch raw=%04X src_pc=%08X", raw16_prev, curr_pc - 2);
                    sh4::ir::DumpTrace();
                }
                else if (IsTopRegion(branch_target))
                {
                    ERROR_LOG(SH4, "*** HIGH-FF branch commit! raw=%04X src_pc=%08X -> %08X", raw16_prev, curr_pc - 2, branch_target);
                }
                // Jump to the branch target and finish executing this block so
                // the dispatcher can start a new one from the destination.
                SetPC(ctx, branch_target, "branch_commit");
                return;
            }
            else
            {
                // This was the branch instruction; mark that the next
                // instruction we execute will be the delay slot.
                executed_delay = true;
            }
        }
    } // end for (const auto& ins : blk->code)
} // end Executor::ExecuteBlock

} // namespace ir
} // namespace sh4
