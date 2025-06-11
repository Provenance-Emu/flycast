#include "ir_executor.h"
#include <cassert>

namespace sh4 {
namespace ir {

void Executor::ExecuteBlock(const Block* blk, Sh4Context* ctx)
{
    assert(blk);
    size_t ip = 0;
    while (ip < blk->code.size())
    {
        const Instr& ins = blk->code[ip++];
        switch (ins.op)
        {
        case Op::END:
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
        case Op::ADD_REG:
            ctx->r[ins.dst.reg] += ctx->r[ins.src1.reg];
            break;
        case Op::BRA:
            ctx->pc += ins.extra; // pc+4+disp already encoded
            return; // leave block
        default:
            break;
        }
    }
}

} // namespace ir
} // namespace sh4
