#include "ir_emitter.h"
#include "hw/sh4/sh4_if.h"  // for Sh4Context, etc.
#include "hw/sh4/modules/mmu.h"
#include "log/Log.h"
#include "hw/sh4/sh4_core.h" // for SH4ThrownException and codes
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
static FILE* g_ir_log_file = nullptr;
static std::mutex g_ir_log_mutex;

void log_to_file(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_ir_log_mutex);
    if (!g_ir_log_file)
    {
        g_ir_log_file = fopen("~/sh4_ir.log", "a");
        if (!g_ir_log_file)
            return; // give up silently
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_ir_log_file, fmt, ap);
    va_end(ap);
    fflush(g_ir_log_file);
}
}

namespace sh4 {
namespace ir {

Block& Emitter::CreateNew(uint32_t pc) {
    auto [it, inserted] = cache_.emplace(pc, Block{});
    Block& blk = it->second;
    if (inserted)
    {
        blk.pcStart = pc;

        uint16_t raw = mmu_IReadMem16(pc);

        // Simple decode for a few key opcodes
        uint8_t n = (raw >> 8) & 0xF;
        uint8_t m = (raw >> 4) & 0xF;

        Instr ins{};
        // clear operands
        ins.dst = {};
        ins.src1 = {};
        ins.src2 = {};
        bool decoded = false;

        // NOP : treat MOV R0,R0 (0x0009) and 0x0000 as NOP
        if (raw == 0x0009 || raw == 0x0000)
        {
            ins.op = Op::NOP;
            decoded = true;
            blk.pcNext = pc + 2;
        }
        // MOV Rm -> Rn  (0x6nm3) pattern 0110 nnnn mmmm 0011
        else if ((raw & 0xF00F) == 0x6003)
        {
            ins.op = Op::MOV_REG;
            ins.dst.isImm = false;
            ins.dst.reg = n;
            ins.src1.isImm = false;
            ins.src1.reg = m;
            decoded = true;
            blk.pcNext = pc + 2;
        }
        // MOV #imm,Rn  (0xE000 | Rn<<8 | imm8)
        else if ((raw & 0xF000) == 0xE000)
        {
            ins.op = Op::MOV_IMM;
            ins.dst.isImm = false;
            ins.dst.reg = n;
            ins.src1.isImm = true;
            ins.src1.imm = static_cast<int8_t>(raw & 0xFF);
            decoded = true;
            blk.pcNext = pc + 2;
        }
        // ADD #imm,Rn  (0x7000 | Rn<<8 | imm8)
        else if ((raw & 0xF000) == 0x7000)
        {
            ins.op = Op::ADD_IMM;
            ins.dst.isImm = false;
            ins.dst.reg = n;
            ins.src1.isImm = true;
            ins.src1.imm = static_cast<int8_t>(raw & 0xFF);
            decoded = true;
            blk.pcNext = pc + 2;
        }
        // BRA disp12  (0xA000 | disp)
        else if ((raw & 0xF000) == 0xA000)
        {
            int32_t disp = static_cast<int32_t>(raw & 0x0FFF);
            if (disp & 0x800) // sign bit of 12-bit
                disp |= ~0xFFF; // sign extend to 32-bit
            disp <<= 1; // disp*2 since pc+4 later
            ins.op = Op::BRA;
            ins.extra = disp + 4; // pc-relative (add after delay slot)
            decoded = true;
            blk.pcNext = pc + 2; // executor updates
        }

        if (!decoded)
        {
            ERROR_LOG(SH4, "IR: unhandled opcode %04X at %08X", raw, pc);
            log_to_file("unhandled opcode %04X at %08X\n", raw, pc);
            // throw so interpreter handles as illegal instruction (will be caught in Run/Step)
            throw SH4ThrownException(pc, Sh4Ex_IllegalInstr);
        }

        blk.code.push_back(ins);
        // add END terminator so executor knows when to stop
        Instr end{};
        end.op = Op::END;
        blk.code.push_back(end);
    }
    return blk;
}

const Block* Emitter::BuildBlock(uint32_t pc)
{
    auto it = cache_.find(pc);
    if (it != cache_.end())
        return &it->second;
    return &CreateNew(pc);
}

void Emitter::EmitInstr(Block&, uint16_t) {
    // TODO: real decode later
}

} // namespace ir
} // namespace sh4
