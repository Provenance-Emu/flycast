#pragma once

#include "hw/sh4/sh4_if.h"
#include "ir_emitter.h"
#include "ir_executor.h"

namespace sh4 {
namespace ir {

class Sh4IrInterpreter : public Sh4Executor {
public:
    Sh4IrInterpreter();
    ~Sh4IrInterpreter() override = default;

    void Run() override;
    void Start() override { running_ = true; }
    void Stop() override { running_ = false; }
    void Step() override;
    void Reset(bool hard) override;
    void Init() override;
    void Term() override {}
    void ResetCache() override { emitter_ = Emitter{}; }
    bool IsCpuRunning() override { return running_; }
    Sh4Context* GetContext() override { return ctx_; }
    
    // Invalidate a block at the specified address
    void InvalidateBlock(u32 addr);

private:
    bool running_ = false;
    Sh4Context* ctx_ = nullptr; // provided by core elsewhere
    Emitter emitter_;
    Executor executor_;
};

} // namespace ir
} // namespace sh4
