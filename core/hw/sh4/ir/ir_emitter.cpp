#include "ir_emitter.h"
#include "hw/sh4/sh4_if.h"  // for Sh4Context, etc.

namespace sh4 {
namespace ir {

Block& Emitter::CreateNew(uint32_t pc) {
    auto [it, inserted] = cache_.emplace(pc, Block{});
    Block& blk = it->second;
    if (inserted)
    {
        blk.pcStart = pc;
        // In this stub we emit a single END op so execution returns immediately.
        Instr i{};
        i.op = Op::END;
        blk.code.push_back(i);
        blk.pcNext = pc; // Keep same PC until real decoding added to avoid runaway cache growth
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
