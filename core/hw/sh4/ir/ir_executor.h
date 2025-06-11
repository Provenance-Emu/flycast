#pragma once

#include "ir_defs.h"
#include "hw/sh4/sh4_if.h"

namespace sh4 {
namespace ir {

class Executor {
public:
    // Execute a single IR block using provided SH4 context.
    void ExecuteBlock(const Block* blk, Sh4Context* ctx);
};

} // namespace ir
} // namespace sh4
