#pragma once

#include <cstdint>
#include <vector>

namespace sh4 {
namespace ir {

// Simple IR opcode enumeration — extend as needed
enum class Op : uint8_t {
    NOP,
    ADD,
    SUB,
    MOV,
    LOAD8,
    LOAD16,
    LOAD32,
    STORE8,
    STORE16,
    STORE32,
    AND,
    OR,
    XOR,
    SHL,
    SHR,
    BRANCH,
    END
};

// Operand kinds – for now just register index or immediate flag
struct Operand {
    bool isImm = false;
    uint8_t reg = 0;   // if !isImm, general-purpose reg index
    int32_t imm = 0;   // valid when isImm
};

struct Instr {
    Op      op {Op::NOP};
    Operand dst {};
    Operand src1 {};
    Operand src2 {};
    int32_t extra = 0;   // displacement / branch target etc.
};

struct Block {
    uint32_t pcStart = 0;
    std::vector<Instr> code;
    uint32_t pcNext   = 0;  // fall-through
};

} // namespace ir
} // namespace sh4
