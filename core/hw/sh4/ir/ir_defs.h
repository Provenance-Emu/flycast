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
    STORE8_PREDEC,
    STORE16_PREDEC,
    STORE32_PREDEC,
    GET_MACH,
    GET_MACL,
    GET_PR,
    AND,
    OR,
    XOR,
    SHL,
    SHR,
    SAR,
    SWAP_B,
    SWAP_W,
    SHR_OP,
    SAR_OP,
    DT,
    AND_IMM,
    OR_IMM,
    XOR_IMM,
    NOT_OP,
    SHL1,
    SHR1,
    SAR1,
    AND_REG,
    OR_REG,
    XOR_REG,
    LOAD8_GBR,
    LOAD16_GBR,
    LOAD32_GBR,
    STORE8_GBR,
    STORE16_GBR,
    STORE32_GBR,
    MOVA,
    MULU_W,
    MULS_W,
    CMP_STR,
    BRANCH,
    END,
    MOV_REG,   // dst = src (both registers)
    MOV_IMM,   // dst = imm32 (sign-extended immediate)
    ADD_REG,   // dst += src
    ADD_IMM,   // dst += imm32
    BRA,       // pc = pc + disp
    JSR,       // subroutine call
    RTS,       // return from subroutine
    LOAD16_IMM, // dst = *(u16*)addr
    LOAD32_IMM,  // dst = *(u32*)addr
    ILLEGAL,      // illegal opcode placeholder
    CMP_EQ,
    CMP_PL,
    CMP_HI,
    CMP_HS,
    TST_IMM,
    TST_REG,
    MOVT,
    BF,
    BT
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
