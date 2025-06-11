#include "ir_executor.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_core.h" // for SH4ThrownException
#include <cassert>
#include "log/Log.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <utility>

namespace sh4 {
namespace ir {

// ----------------------------------------------------------------------------
//  Execution statistics helpers
// ----------------------------------------------------------------------------
namespace {
constexpr size_t kOpCount = static_cast<size_t>(Op::XTRCT) + 1;
static std::array<std::atomic<uint64_t>, kOpCount> g_opExecCounts{};
static std::atomic<uint64_t> g_totalExecCount{0};
constexpr uint64_t kLogInterval = 200; // log every ~200 instructions

// string names for enumeration – keep in sync with Op enum via X macro style list in one place.
// For now provide a fallback minimal array so stats compile; full names generation to be added later.
static const char* OpNames[kOpCount] = {"NOP", "ADD", "SUB", "MOV", "LOAD8", "LOAD16", "LOAD32", "STORE8", "STORE16", "STORE32"};

inline const char* GetOpName(size_t idx)
{
    if (idx < kOpCount && OpNames[idx])
        return OpNames[idx];
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "OP_%zu", idx);
    return buf;
}

static void MaybeDumpStats()
{
    uint64_t executed = g_totalExecCount.load(std::memory_order_relaxed);
    if (executed == 0 || executed % kLogInterval != 0)
        return;

    INFO_LOG(SH4, "--- IR opcode execution stats after %llu instructions ---", static_cast<unsigned long long>(executed));
    // Print the 10 most executed opcodes
    struct Item { size_t idx; uint64_t count; };
    std::array<Item, 10> top{{}};

    for (size_t i = 0; i < kOpCount; ++i)
    {
        uint64_t count = g_opExecCounts[i].load(std::memory_order_relaxed);
        if (count == 0)
            continue;

        for (auto& slot : top)
        {
            if (count > slot.count)
            {
                std::swap(count, slot.count);
                std::swap(i, slot.idx);
            }
        }
    }

    for (const auto& item : top)
    {
        if (item.count == 0)
            break;
        INFO_LOG(SH4, "  %-12s : %llu", GetOpName(item.idx), static_cast<unsigned long long>(item.count));
    }
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
    uint32_t curr_pc = ctx->pc; // PC of current instruction being executed
    while (ip < blk->code.size())
    {
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
            ctx->r[ins.dst.reg] = mmu_ReadMem<u8>(addr);
            break;
        }
        case Op::LOAD16:
        {
            uint32_t addr = ctx->r[ins.src1.reg] + static_cast<uint32_t>(ins.extra);
            ctx->r[ins.dst.reg] = mmu_ReadMem<u16>(addr);
            break;
        }
        case Op::LOAD32:
        {
            uint32_t addr = ctx->r[ins.src1.reg] + static_cast<uint32_t>(ins.extra);
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
        case Op::STORE8:
        {
            uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
            mmu_WriteMem<u8>(addr, ctx->r[ins.src1.reg]);
            break;
        }
        case Op::STORE16:
        {
            uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
            mmu_WriteMem<u16>(addr, static_cast<uint16_t>(ctx->r[ins.src1.reg]));
            break;
        }
        case Op::STORE32:
        {
            uint32_t addr = ctx->r[ins.dst.reg] + static_cast<uint32_t>(ins.extra);
            mmu_WriteMem<u32>(addr, ctx->r[ins.src1.reg]);
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
        default:
            // Unimplemented opcode – fall back to legacy interpreter via IllegalInstr exception
            ERROR_LOG(SH4, "IR executor unimplemented opcode Op %d at %08X", static_cast<int>(ins.op), curr_pc);
            g_opExecCounts[static_cast<size_t>(ins.op)] = 0;
            throw SH4ThrownException(curr_pc, Sh4Ex_IllegalInstr);
        }

        // Advance PC by 2 for the next sequential instruction unless a branch is pending
        if (!branch_pending)
        {
            ctx->pc += 2;
        }

        // If a branch is pending and we just executed the delay slot (i.e., branch_pending already true
        // and executed_delay was false entering this iteration), mark that the delay slot has executed so
        // the next loop will commit the branch.
        if (branch_pending && !executed_delay)
        {
            executed_delay = true;
        }

        curr_pc = ctx->pc; // update for next loop iteration
    }
}

} // namespace ir
} // namespace sh4
