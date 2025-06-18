#include "sh4_ir_interpreter.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h" // for SH4ThrownException
#include "log/Log.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/addrspace.h" // for ram_base fast access
#include "hw/flashrom/nvmem.h" // for BIOS pointer

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
    // Set PC to reset vector; reinitialise general regs and VBR
    for (int i = 0; i < 16; ++i)
        ctx_->r[i] = 0;
    ctx_->vbr = 0x8C000000;
    ctx_->sr.T = 0;
    ctx_->sh4_sched_next = 0; // Reset scheduler/cycle count for IR
    ctx_->pc = 0xA0000000;     // Set PC to reset vector (0xA0000000 for BIOS)
    ResetCache();
}

// Helper similar to Executor::FastRamPtr (local copy for interpreter)
static inline u8* FastPtr(uint32_t addr)
{
    // BIOS ROM (2 MiB) and mirrors (include P0 0x4000 0000 for ITLB-miss handler)
    if ((addr & 0xFFE00000u) == 0x00000000u ||
        (addr & 0xFFE00000u) == 0x40000000u ||
        (addr & 0xFFE00000u) == 0x80000000u ||
        (addr & 0xFFE00000u) == 0xA0000000u ||
        (addr & 0xFFE00000u) == 0xC0000000u)
        return nvmem::getBiosData() + (addr & 0x001FFFFF);

    // Main RAM 0x0C000000–0x0FFFFFFF
    if ((addr & 0xFC000000u) == 0x0C000000u)
        return addrspace::ram_base + (addr & 0x03FFFFFF);

    // P4 SDRAM mirrors
    if (addr >= 0xF8000000u && addr < 0xFF000000u)
        return addrspace::ram_base + 0x0C000000 + (addr & 0x00FFFFFF);

    return nullptr;
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
            if (blk->code.size() == 2 && blk->code[0].op == ir::Op::NOP)
            {
                // Fast-skip over large stretches of 0x0000 instructions that
                // the BIOS uses for memory clear stubs. We only do this when
                // we have a direct pointer into either RAM or the BIOS ROM –
                // this guarantees the memory is valid and avoids hiding real
                // mapping bugs.
                u32 pc_scan = ctx_->pc;
                if (u8* base = FastPtr(pc_scan))
                {
                    const u16* w = reinterpret_cast<const u16*>(base);
                    while (*w == 0 || *w == 0x0009)
                    {
                        ++w;
                        pc_scan += 2;
                        if ((pc_scan - ctx_->pc) >= 0x100000)
                            break;
                    }
                    ctx_->pc = pc_scan;
                }
                else
                {
                    // No direct pointer; fall back to reading via MMU
                    const uint32_t limit = pc_scan + 0x100000; // 1 MiB max
                    while (pc_scan < limit)
                    {
                        u16 iw = mmu_IReadMem16(pc_scan);
                        if (iw != 0x0000 && iw != 0x0009) break;
                        pc_scan += 2;
                    }
                    ctx_->pc = pc_scan;
                }
            }
            ++step_counter;
            if ((step_counter & 0x1FFFF) == 0) // every 131072 blocks
            {
                INFO_LOG(SH4, "PC=%08X", ctx_->pc);
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
        if (blk->code.size() == 2 && blk->code[0].op == ir::Op::NOP)
        {
            u32 pc_scan = ctx_->pc;
            if (u8* base = FastPtr(pc_scan))
            {
                const u16* w = reinterpret_cast<const u16*>(base);
                while (*w == 0 || *w == 0x0009)
                {
                    ++w;
                    pc_scan += 2;
                    if ((pc_scan - ctx_->pc) >= 0x100000)
                        break;
                }
                ctx_->pc = pc_scan;
            }
            else
            {
                const uint32_t limit = pc_scan + 0x100000;
                while (pc_scan < limit)
                {
                    u16 iw = mmu_IReadMem16(pc_scan);
                    if (iw != 0x0000 && iw != 0x0009) break;
                    pc_scan += 2;
                }
                ctx_->pc = pc_scan;
            }
        }
    } catch (const SH4ThrownException& ex) {
        Do_Exception(ex.epc, ex.expEvn);
    }
}

} // namespace ir
} // namespace sh4

#ifdef SH4_IR_ENABLED
Sh4Executor* Get_Sh4Interpreter()
{
    fprintf(stderr, "[DEBUG_PRINTF] IR Get_Sh4Interpreter() called THE NEW ONE\n");
    printf("[DEBUG_PRINTF] IR Get_Sh4Interpreter() called. THE NEW ONE\n");
    fflush(stderr);
    return new sh4::ir::Sh4IrInterpreter();
}
#endif // SH4_IR_ENABLED
