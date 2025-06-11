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
        default:
            // Unimplemented opcodes just skip for now
            break;
        }
    }
}

} // namespace ir
} // namespace sh4
