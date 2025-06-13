#pragma once
#include "types.h"
#include "hw/hwreg.h"

template<u32 idx>
void CCN_QACR_write(u32 addr, u32 value);

// CCN register block is 0x100 bytes (256 / 4 = 64 u32 words). We reserve full area to avoid out-of-bound logs.
extern u32 CCN[64];

class CCNRegisters : public RegisterBank<CCN, 64>
{
	using super = RegisterBank<CCN, 64>;

public:
	void init();
};
extern CCNRegisters ccn;
