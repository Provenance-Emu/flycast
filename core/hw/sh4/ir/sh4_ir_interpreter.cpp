#include "sh4_ir_interpreter.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h" // for SH4ThrownException
#include "log/Log.h"

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
    ctx_->vbr = 0x8C000000; // exception vectors
    ctx_->pc  = 0xA0000000; // BIOS entry point in P2
}

void Sh4IrInterpreter::Reset(bool /*hard*/)
{
    // Preserve PC; reinitialise general regs
    for (int i = 0; i < 16; ++i)
        ctx_->r[i] = 0;
    ctx_->vbr = 0x8C000000;
    ctx_->sr.T = 0;
    ResetCache();
}

void Sh4IrInterpreter::Run()
{
    running_ = true;
    while (running_)
    {
        uint32_t pc = ctx_->pc;
        uint32_t old_pc = pc;
        static uint64_t step_counter = 0;
        try {
            const Block* blk = emitter_.BuildBlock(pc);
            executor_.ExecuteBlock(blk, ctx_);
            if (ctx_->pc == old_pc)
                ctx_->pc = blk->pcNext;
            ++step_counter;
            if ((step_counter & 0x1FFFF) == 0) // every 131072 blocks
            {
                WARN_LOG(SH4, "IR step %llu PC=%08X", static_cast<unsigned long long>(step_counter), ctx_->pc);
            }
        } catch (const SH4ThrownException& ex) {
            Do_Exception(ex.epc, ex.expEvn);
        }
    }
}

void Sh4IrInterpreter::Step()
{
    uint32_t pc = ctx_->pc;
    uint32_t old_pc = pc;
    try {
        const Block* blk = emitter_.BuildBlock(pc);
        executor_.ExecuteBlock(blk, ctx_);
        if (ctx_->pc == old_pc)
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
