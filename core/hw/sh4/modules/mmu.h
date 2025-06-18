#pragma once
#include "types.h"
#include "hw/sh4/sh4_if.h" // For Sh4ExceptionCode enum and Sh4Context
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/dyna/ngen.h"

//Translation Types
//Opcode read
#define MMU_TT_IREAD 0
//Data write
#define MMU_TT_DWRITE 1
//Data write
#define MMU_TT_DREAD 2

enum class MmuError
{
    //Translation was successful
    NONE,
    //UTLB miss (Data TLB)
    TLB_MISS,
    //ITLB miss (Instruction TLB)
    ITLB_MISS,
	//TLB Multihit
	TLB_MHIT,
	//Mem is read/write protected (depends on translation type)
	PROTECTED,
	//Mem is write protected , firstwrite
	FIRSTWRITE,
	//data-Opcode read/write misaligned
	BADADDR
};

struct TLB_Entry
{
	CCN_PTEH_type Address;
	CCN_PTEL_type Data;
	CCN_PTEA_type Assistance;
};

extern TLB_Entry UTLB[64];
extern TLB_Entry ITLB[4];
extern bool mmuOn;

constexpr u32 fast_reg_lut[8] =
{
	0, 0, 0, 0	//P0-U0
	, 1		//P1
	, 1		//P2
	, 0		//P3
	, 1		//P4
};

constexpr u32 mmu_mask[4] =
{
	((0xFFFFFFFF) >> 10) << 10,	//1 kb page
	((0xFFFFFFFF) >> 12) << 12,	//4 kb page
	((0xFFFFFFFF) >> 16) << 16,	//64 kb page
	((0xFFFFFFFF) >> 20) << 20	//1 MB page
};

bool UTLB_Sync(u32 entry);
void ITLB_Sync(u32 entry);

bool mmu_match(u32 va, CCN_PTEH_type Address, CCN_PTEL_type Data);
void mmu_set_state();
void mmu_flush_table();
[[noreturn]] void mmu_raise_exception(MmuError mmu_error, u32 address, u32 am);

static inline bool mmu_enabled()
{
	return mmuOn;
}

MmuError mmu_full_lookup(u32 va, const TLB_Entry **entry, u32& rv);
MmuError mmu_instruction_lookup(u32 va, const TLB_Entry **entry, u32& rv);
template<u32 translation_type>
MmuError mmu_full_SQ(u32 va, u32& rv);

#ifdef FAST_MMU
static inline MmuError mmu_instruction_translation(u32 va, u32& rv)
{
	// Low physical memory (0x00000000-0x01FFFFFF) is identity-mapped even when MMU is enabled on DC BIOS.
	if (va < 0x02000000)
	{
		rv = va;
		return MmuError::NONE;
	}

	// P4 area (on-chip I/O, ROM) is always direct-mapped, MMU never translates
	if ((va & 0xE0000000) == 0xE0000000)
	{
		rv = va;
		return MmuError::NONE;
	}

	// Cached areas P1/P2 behave differently based on MMU state
    if ((va & 0xE0000000) == 0x80000000 || (va & 0xE0000000) == 0xA0000000)
    {
        if (CCN_MMUCR.AT == 0)
        {
            // MMU disabled: identity mapping (virtual == physical)
            rv = va;
        }
        else
        {
            // MMU enabled: identity-mapped (physical == virtual)
            rv = va;
        }
        return MmuError::NONE;
    }

	// P3 (0xC0000000) remains mirrored to SDRAM window as before.
	if ((va & 0xE0000000) == 0xC0000000)
	{
		rv = 0x0C000000 | (va & 0x00FFFFFF);
		return MmuError::NONE;
	}

	// P4 SDRAM mirrors: 0xF8xxxxxx–0xFExxxxxx map to same SDRAM offset
	if (va >= 0xF8000000u && va < 0xFF000000u)
	{
		rv = 0x0C000000 | (va & 0x00FFFFFF);
		return MmuError::NONE;
	}

	if (fast_reg_lut[va >> 29] != 0)
	{
		rv = va;
		return MmuError::NONE;
	}

	return mmu_full_lookup(va, nullptr, rv);
}
#else
MmuError mmu_instruction_translation(u32 va, u32& rv);
#endif

template<u32 translation_type>
MmuError mmu_data_translation(u32 va, u32& rv);
void DoMMUException(u32 addr, MmuError mmu_error, u32 access_type);

inline static bool mmu_is_translated(u32 va, u32 size)
{
#ifndef FAST_MMU
	if (va & (std::min(size, 4u) - 1))
		return true;
#endif

	if (fast_reg_lut[va >> 29] != 0)
		return false;

	if ((va & 0xFC000000) == 0x7C000000)
		// On-chip RAM area isn't translated
		return false;

	return true;
}

template<typename T> T DYNACALL mmu_ReadMem(u32 adr);
u16 DYNACALL mmu_IReadMem16(u32 addr);

template<typename T> void DYNACALL mmu_WriteMem(u32 adr, T data);

void mmu_TranslateSQW(u32 adr, u32* out);

#ifdef FAST_MMU
// maps 4K virtual page number to physical address
extern u32 mmuAddressLUT[0x100000];

static inline void mmuAddressLUTFlush(bool full)
{
	if (full)
		memset(mmuAddressLUT, 0, sizeof(mmuAddressLUT) / 2);	// flush user memory
	else
	{
		constexpr u32 slotPages = (32 * 1024 * 1024) >> 12;
		memset(mmuAddressLUT, 0, slotPages * sizeof(u32));		// flush slot 0
	}
}
#endif

#if FEAT_SHREC == DYNAREC_JIT
static inline u32 DYNACALL mmuDynarecLookup(u32 vaddr, u32 write, u32 pc)
{
	u32 paddr;
	MmuError rv;
	// TODO pass access size so that alignment errors are raised
	if (write)
		rv = mmu_data_translation<MMU_TT_DWRITE>(vaddr, paddr);
	else
		rv = mmu_data_translation<MMU_TT_DREAD>(vaddr, paddr);
	if (unlikely(rv != MmuError::NONE))
	{
		Sh4cntx.pc = pc;
		DoMMUException(vaddr, rv, write ? MMU_TT_DWRITE : MMU_TT_DREAD);
		host_context_t ctx;
		sh4Dynarec->handleException(ctx);
		((void (*)())ctx.pc)();
		// not reached
		return 0;
	}
#ifdef FAST_MMU
	if (vaddr >> 31 == 0)
		mmuAddressLUT[vaddr >> 12] = paddr & ~0xfff;
#endif

	return paddr;
}
#endif

void MMU_init();
void MMU_reset();
void MMU_term();

void mmu_serialize(Serializer& ser);
void mmu_deserialize(Deserializer& deser);
