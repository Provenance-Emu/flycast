#include "sh4_ir_interpreter.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h" // for SH4ThrownException

namespace sh4 {
namespace ir {

Sh4IrInterpreter::Sh4IrInterpreter()
{
    ctx_ = &p_sh4rcb->cntx; // assume global struct exists as in legacy interpreter
}

void Sh4IrInterpreter::Init()
{
    // Zero context similar to legacy init
    memset(ctx_, 0, sizeof(*ctx_));
    ctx_->vbr = 0x8C000000; // place exception vectors in unmapped P2 region
}

void Sh4IrInterpreter::Reset(bool /*hard*/)
{
    // Just clear register state for now
    memset(ctx_, 0, sizeof(*ctx_));
    ctx_->vbr = 0x8C000000;
    ResetCache();
}

void Sh4IrInterpreter::Run()
{
    running_ = true;
    while (running_)
    {
        uint32_t pc = ctx_->pc;
        try {
            const Block* blk = emitter_.BuildBlock(pc);
            executor_.ExecuteBlock(blk, ctx_);
            ctx_->pc = blk->pcNext;
        } catch (const SH4ThrownException& ex) {
            Do_Exception(ex.epc, ex.expEvn);
        }
    }
}

void Sh4IrInterpreter::Step()
{
    uint32_t pc = ctx_->pc;
    try {
        const Block* blk = emitter_.BuildBlock(pc);
        executor_.ExecuteBlock(blk, ctx_);
        ctx_->pc = blk->pcNext;
    } catch (const SH4ThrownException& ex) {
        Do_Exception(ex.epc, ex.expEvn);
    }
}

} // namespace ir
} // namespace sh4

#ifdef ENABLE_SH4_IR
Sh4Executor* Get_Sh4Interpreter()
{
    return new sh4::ir::Sh4IrInterpreter();
}
#endif
