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

// Auto-generated opcode name table
#include "ir_opnames.inc"

namespace sh4 {
namespace ir {

// ----------------------------------------------------------------------------
//  Execution statistics helpers
// ----------------------------------------------------------------------------
namespace {
constexpr size_t kOpCount = static_cast<size_t>(Op::NUM_OPS);
static std::array<std::atomic<uint64_t>, kOpCount> g_opExecCounts{};
static std::atomic<uint64_t> g_totalExecCount{0};
constexpr uint64_t kLogInterval = 2'000'000; // log every ~2M instructions

inline const char* GetOpName(size_t idx)
{
    if (idx < kOpCount)
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

// Fast pointer fetch for main RAM aliases (P1/P2/P3) and P4 SDRAM mirrors (F8–FE)
static inline u8* FastRamPtr(uint32_t addr)
{
    // Main RAM 0x0C000000–0x0FFFFFFF (26-bit mask)
    if ((addr & 0xFC000000) == 0x0C000000)
        return addrspace::ram_base + (addr & 0x03FFFFFF);

    // P4 SDRAM mirrors 0xF8xxxxxx–0xFExxxxxx (8 windows of 16 MiB)
    if (addr >= 0xF8000000u && addr < 0xFF000000u)
        return addrspace::ram_base + 0x0C000000 + (addr & 0x00FFFFFF);

    return nullptr;
}

// Per-opcode execution helper signature
using ExecFn = void(*)(const sh4::ir::Instr&, Sh4Context*, uint32_t);

// Generic stub that falls back to IllegalInstr -> legacy interpreter
static void ExecStub(const sh4::ir::Instr&, Sh4Context*, uint32_t pc)
{
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

} // end anonymous namespace

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
        // Current PC before executing this instruction
        uint32_t curr_pc = ctx->pc;

        // If we have just executed the delay slot, commit the pending branch now
        if (executed_delay)
        {
            ctx->pc = branch_target;
            return; // block finished after branch commit
        }

        const Instr& ins = blk->code[ip++];
        // ---- statistics ----
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
                    // normal block end; PC already at next sequential instruction
                    return;
                case Op::NOP:
                    break;
                case Op::MOV_REG:
                    ctx->r[ins.dst.reg] = ctx->r[ins.src1.reg];
                    break;
                case Op::MOV_IMM:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(ins.src1.imm);
                    break;
                case Op::ADD_IMM:
                    ctx->r[ins.dst.reg] += static_cast<int32_t>(ins.src1.imm);
                    // TODO: set condition codes
                    break;
                case Op::SHL:
                    ctx->r[ins.dst.reg] <<= ins.extra & 31;
                    // TODO: update SR flags (T,C) appropriately
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
                case Op::DT:
                {
                    uint32_t v = --ctx->r[ins.dst.reg];
                    ctx->sr.T = (v == 0);
                    break;
                }
                case Op::SHL1:
                    ctx->r[ins.dst.reg] <<= 1;
                    break;
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
                case Op::SAR_OP:
                    ctx->r[ins.dst.reg] = static_cast<uint32_t>(static_cast<int32_t>(ctx->r[ins.dst.reg]) >> (ins.extra & 31));
                    break;
                case Op::ADD_REG:
                    ctx->r[ins.dst.reg] += ctx->r[ins.src1.reg];
                    break;
                case Op::LOAD8:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtr(addr))
                        ctx->r[ins.dst.reg] = *p;
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u8>(addr);
                    break;
                }
                case Op::LOAD16:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtr(addr))
                        ctx->r[ins.dst.reg] = *reinterpret_cast<u16*>(p);
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u16>(addr);
                    break;
                }
                case Op::LOAD32:
                {
                    uint32_t addr = ctx->r[ins.src1.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtr(addr))
                        ctx->r[ins.dst.reg] = *reinterpret_cast<u32*>(p);
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(addr);
                    break;
                }
                case Op::LOAD8_GBR:
                    ctx->r[ins.dst.reg] = mmu_ReadMem<u8>(ctx->gbr + static_cast<uint32_t>(ins.extra));
                    break;
                case Op::LOAD16_GBR:
                    ctx->r[ins.dst.reg] = mmu_ReadMem<u16>(ctx->gbr + static_cast<uint32_t>(ins.extra));
                    break;
                case Op::LOAD32_GBR:
                    ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(ctx->gbr + static_cast<uint32_t>(ins.extra));
                    break;
                case Op::LOAD8_POST:
                {
                    uint32_t addr = ctx->r[ins.src1.reg];
                    if (u8* p = FastRamPtr(addr))
                        ctx->r[ins.dst.reg] = *p;
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u8>(addr);
                    ctx->r[ins.src1.reg] += 1;
                    break;
                }
                case Op::LOAD16_POST:
                {
                    uint32_t addr = ctx->r[ins.src1.reg];
                    if (u8* p = FastRamPtr(addr))
                        ctx->r[ins.dst.reg] = *reinterpret_cast<u16*>(p);
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u16>(addr);
                    ctx->r[ins.src1.reg] += 2;
                    break;
                }
                case Op::LOAD32_POST:
                {
                    uint32_t addr = ctx->r[ins.src1.reg];
                    if (u8* p = FastRamPtr(addr))
                        ctx->r[ins.dst.reg] = *reinterpret_cast<u32*>(p);
                    else
                        ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(addr);
                    ctx->r[ins.src1.reg] += 4;
                    break;
                }
                case Op::STORE8:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtr(addr))
                        *p = static_cast<u8>(ctx->r[ins.src1.reg]);
                    else
                        mmu_WriteMem<u8>(addr, ctx->r[ins.src1.reg]);
                    break;
                }
                case Op::STORE16:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtr(addr))
                        *reinterpret_cast<u16*>(p) = static_cast<u16>(ctx->r[ins.src1.reg]);
                    else
                        mmu_WriteMem<u16>(addr, static_cast<uint16_t>(ctx->r[ins.src1.reg]));
                    break;
                }
                case Op::STORE32:
                {
                    uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
                    if (u8* p = FastRamPtr(addr))
                        *reinterpret_cast<u32*>(p) = ctx->r[ins.src1.reg];
                    else
                        mmu_WriteMem<u32>(addr, ctx->r[ins.src1.reg]);
                    break;
                }
                case Op::STORE8_POST:
                {
                    uint32_t addr = ctx->r[ins.dst.reg];
                    if (u8* p = FastRamPtr(addr))
                        *p = static_cast<u8>(ctx->r[ins.src1.reg]);
                    else
                        mmu_WriteMem<u8>(addr, ctx->r[ins.src1.reg]);
                    ctx->r[ins.dst.reg] += 1;
                    break;
                }
                case Op::STORE16_POST:
                {
                    uint32_t addr = ctx->r[ins.dst.reg];
                    if (u8* p = FastRamPtr(addr))
                        *reinterpret_cast<u16*>(p) = static_cast<u16>(ctx->r[ins.src1.reg]);
                    else
                        mmu_WriteMem<u16>(addr, static_cast<uint16_t>(ctx->r[ins.src1.reg]));
                    ctx->r[ins.dst.reg] += 2;
                    break;
                }
                case Op::STORE32_POST:
                {
                    uint32_t addr = ctx->r[ins.dst.reg];
                    if (u8* p = FastRamPtr(addr))
                        *reinterpret_cast<u32*>(p) = ctx->r[ins.src1.reg];
                    else
                        mmu_WriteMem<u32>(addr, ctx->r[ins.src1.reg]);
                    ctx->r[ins.dst.reg] += 4;
                    break;
                }
                case Op::STORE8_GBR:
                    mmu_WriteMem<u8>(ctx->gbr + static_cast<uint32_t>(ins.extra), ctx->r[ins.src1.reg]);
                    break;
                case Op::STORE16_GBR:
                    mmu_WriteMem<u16>(ctx->gbr + static_cast<uint32_t>(ins.extra), static_cast<uint16_t>(ctx->r[ins.src1.reg]));
                    break;
                case Op::STORE32_GBR:
                    mmu_WriteMem<u32>(ctx->gbr + static_cast<uint32_t>(ins.extra), ctx->r[ins.src1.reg]);
                    break;
                case Op::LOAD16_IMM:
                    ctx->r[ins.dst.reg] = mmu_ReadMem<u16>(static_cast<uint32_t>(ins.src1.imm));
                    break;
                case Op::LOAD32_IMM:
                    ctx->r[ins.dst.reg] = mmu_ReadMem<u32>(static_cast<uint32_t>(ins.src1.imm));
                    break;
                case Op::JSR:
                    DEBUG_LOG(SH4, "BR JSR from %08X -> %08X (r%u)", curr_pc, ctx->r[ins.src1.reg], ins.src1.reg);
                    ctx->pr = curr_pc + 4; // address after delay slot
                    branch_target = ctx->r[ins.src1.reg];
                    branch_pending = true;
                    executed_delay = false; // ensure delay flag reset
                    break;
                case Op::JMP:
                    DEBUG_LOG(SH4, "BR JMP from %08X -> %08X (r%u)", curr_pc, ctx->r[ins.src1.reg], ins.src1.reg);
                    branch_target = ctx->r[ins.src1.reg];
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::RTS:
                    DEBUG_LOG(SH4, "BR RTS from %08X -> %08X", curr_pc, ctx->pr);
                    branch_target = ctx->pr;
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BRA:
                    DEBUG_LOG(SH4, "BR BRA from %08X -> %08X (disp=%d)", curr_pc, curr_pc + ins.extra, ins.extra);
                    branch_target = curr_pc + ins.extra;
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BT:
                    if (ctx->sr.T)
                    {
                        DEBUG_LOG(SH4, "BR BT  from %08X -> %08X (disp=%d)", curr_pc, curr_pc + ins.extra, ins.extra);
                        branch_target = curr_pc + ins.extra;
                        branch_pending = true;
                        executed_delay = false;
                    }
                    break;
                case Op::BF:
                    if (!ctx->sr.T)
                    {
                        DEBUG_LOG(SH4, "BR BF  from %08X -> %08X (disp=%d)", curr_pc, curr_pc + ins.extra, ins.extra);
                        branch_target = curr_pc + ins.extra;
                        branch_pending = true;
                        executed_delay = false;
                    }
                    break;
                case Op::CMP_EQ:
                    ctx->sr.T = (ctx->r[ins.dst.reg] == ctx->r[ins.src1.reg]);
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
                case Op::CMP_HI:
                    ctx->sr.T = (ctx->r[ins.dst.reg] > ctx->r[ins.src1.reg]);
                    break;
                case Op::CMP_HS:
                    ctx->sr.T = (ctx->r[ins.dst.reg] >= ctx->r[ins.src1.reg]);
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
                    break;
                case Op::SUB:
                    ctx->r[ins.dst.reg] -= ctx->r[ins.src1.reg];
                    break;
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
                    DEBUG_LOG(SH4, "BR BSR from %08X -> %08X (disp=%d)", curr_pc, curr_pc + ins.extra, ins.extra);
                    ctx->pr = curr_pc + 4; // address after the delay slot
                    branch_target = curr_pc + ins.extra;
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BRAF:
                    DEBUG_LOG(SH4, "BR BRAF from %08X -> %08X (r%u)", curr_pc, curr_pc + 2 + ctx->r[ins.src1.reg], ins.src1.reg);
                    branch_target = curr_pc + 2 + ctx->r[ins.src1.reg];
                    branch_pending = true;
                    executed_delay = false;
                    break;
                case Op::BSRF:
                    DEBUG_LOG(SH4, "BR BSRF from %08X -> %08X (r%u)", curr_pc, curr_pc + 2 + ctx->r[ins.src1.reg], ins.src1.reg);
                    ctx->pr = curr_pc + 2; // return after delay slot (address after current instr)
                    branch_target = curr_pc + 2 + ctx->r[ins.src1.reg];
                    branch_pending = true;
                    executed_delay = false;
                    break;
                default:
                    // Unimplemented opcode – fall back to legacy interpreter via IllegalInstr exception
                    ERROR_LOG(SH4, "IR executor unimplemented opcode Op %d at %08X", static_cast<int>(ins.op), curr_pc);
                    g_opExecCounts[static_cast<size_t>(ins.op)] = 0;
                    throw SH4ThrownException(curr_pc, Sh4Ex_IllegalInstr);
                }
            } // end switch
        } // end else dispatch

        // Advance PC by 2 for the next sequential instruction; this positions the
        // delay-slot instruction (if any) at pc+2. If a branch is pending, the
        // commit step at the top of the next iteration will overwrite pc with
        // the branch target after the delay slot has run.
        ctx->pc += 2;

        // If a branch is pending and we just executed the delay slot (i.e., branch_pending already true
        // and executed_delay was false entering this iteration), mark that the delay slot has executed so
        // the next loop will commit the branch.
        if (branch_pending && !executed_delay)
        {
            executed_delay = true;
        }
    } // end outer dispatch block
} // end Executor::ExecuteBlock

} // namespace ir
} // namespace sh4
