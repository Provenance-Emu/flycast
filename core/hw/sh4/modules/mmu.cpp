#include "mmu.h"
#include "hw/sh4/sh4_if.h"         // For Sh4ExceptionCode enum and Sh4Context
#include "hw/sh4/sh4_interrupts.h" // For Do_Exception

// External flag from sh4_interrupts.cpp to detect if last exception was Sh4Ex_TlbMissWrite
extern bool g_just_had_tlb_miss_write_exception;
extern bool g_itlb_miss_during_handler_fetch; // True if ITLB miss occurs while SR.BL=1 (fetching handler)
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "debug/gdb_server.h"
#include "serialize.h"
#include "hw/flashrom/nvmem.h"
#include "log/Log.h"

TLB_Entry UTLB[64];
TLB_Entry ITLB[4];
static u32 ITLB_LRU_USE[64];
bool mmuOn;

//SQ fast remap , mainly hackish , assumes 1MB pages
//max 64MB can be remapped on SQ
// Used when FullMMU is off
u32 sq_remap[64];

/*
MMU support code
This is mostly hacked-on as the core was never meant to have mmu support

There are two modes, one with 'full' mmu emulation (for wince/bleem/wtfever)
and a fast-hack mode for 1mb sqremaps (for katana)
*/
#ifndef FAST_MMU
#include "ccn.h"
#endif
#include "hw/sh4/sh4_mem.h"

//#define TRACE_WINCE_SYSCALLS

#ifdef TRACE_WINCE_SYSCALLS
#include "wince.h"
u32 unresolved_ascii_string;
u32 unresolved_unicode_string;
#endif

#define printf_mmu(...) DEBUG_LOG(SH4, __VA_ARGS__)

constexpr u32 ITLB_LRU_OR[4] =
{
	0x00,//000xxx
	0x20,//1xx00x
	0x14,//x1x1x0
	0x0B,//xx1x11
};
constexpr u32 ITLB_LRU_AND[4] =
{
	0x07,//000xxx
	0x39,//1xx00x
	0x3E,//x1x1x0
	0x3F,//xx1x11
};

#ifndef FAST_MMU
//sync mem mapping to mmu , suspend compiled blocks if needed.entry is a UTLB entry # , -1 is for full sync
bool UTLB_Sync(u32 entry)
{
	printf_mmu("UTLB MEM remap %d : 0x%X to 0x%X : %d asid %d size %d", entry, UTLB[entry].Address.VPN << 10, UTLB[entry].Data.PPN << 10, UTLB[entry].Data.V,
			UTLB[entry].Address.ASID, UTLB[entry].Data.SZ0 + UTLB[entry].Data.SZ1 * 2);
	if (UTLB[entry].Data.V == 0)
		return true;

	if ((UTLB[entry].Address.VPN & (0xFC000000 >> 10)) == (0xE0000000 >> 10))
	{
		// Used when FullMMU is off
		u32 vpn_sq = ((UTLB[entry].Address.VPN & 0x7FFFF) >> 10) & 0x3F;//upper bits are always known [0xE0/E1/E2/E3]
		sq_remap[vpn_sq] = UTLB[entry].Data.PPN << 10;

		return true;
	}
	else
	{
		return false;
	}
}
//sync mem mapping to mmu , suspend compiled blocks if needed.entry is a ITLB entry # , -1 is for full sync
void ITLB_Sync(u32 entry)
{
	printf_mmu("ITLB MEM remap %d : 0x%X to 0x%X : %d", entry, ITLB[entry].Address.VPN << 10, ITLB[entry].Data.PPN << 10, ITLB[entry].Data.V);
}
#endif

template<typename F>
static void mmuException(MmuError mmu_error, u32 address, u32 am, F raise)
{
	printf_mmu("MMU exception -> pc = 0x%X : ", Sh4cntx.pc);
	CCN_TEA = address;
	CCN_PTEH.VPN = address >> 10;

	switch (mmu_error)
	{
	case MmuError::NONE:
		die("Error: mmu_error == MmuError::NONE)");
		return;

    case MmuError::TLB_MISS:
        // This case now specifically handles UTLB (Data TLB) misses.
        // ITLB misses are handled by MmuError::ITLB_MISS.
        printf_mmu("MmuError::TLB_MISS (UTLB - Data Access) fault_pc=0x%08X, fault_addr=0x%08X, VBR=0x%08X, access_type=%s. Raising SH4 exception.", Sh4cntx.pc, address, Sh4cntx.vbr, (am == MMU_TT_DWRITE ? "write" : "read"));
        DEBUG_LOG(SH4, "mmuException: TLB_MISS. Before raise. Sh4cntx.pc=0x%08X, SR.BL=%d, fault_addr=0x%08X, access_type=%s, event to be passed=0x%X", Sh4cntx.pc, Sh4cntx.sr.BL, address, (am == MMU_TT_DWRITE ? "write" : "read"), (am == MMU_TT_DWRITE ? Sh4Ex_TlbMissWrite : Sh4Ex_TlbMissRead));
        if (am == MMU_TT_DWRITE)
            raise(Sh4Ex_TlbMissWrite);
        else
            raise(Sh4Ex_TlbMissRead);
        return;

    case MmuError::ITLB_MISS:
        // Handles Instruction TLB misses.
        printf_mmu("MmuError::ITLB_MISS (Instruction Fetch) fault_pc=0x%08X, fault_addr=0x%08X, VBR=0x%08X. Raising SH4 exception.", Sh4cntx.pc, address, Sh4cntx.vbr);
        DEBUG_LOG(SH4, "mmuException: ITLB_MISS. Before raise. Sh4cntx.pc=0x%08X, SR.BL=%d, fault_addr=0x%08X, raising Sh4Ex_TMU0 (0x400)", Sh4cntx.pc, Sh4cntx.sr.BL, address);
        raise(Sh4Ex_TMU0); // Event 0x400 for ITLB Miss (Instruction TLB Miss Exception)
        return;



    case MmuError::TLB_MHIT:
		ERROR_LOG(SH4, "MmuError::TLB_MHIT @ 0x%X", address);
		raise(Sh4Ex_TlbMultiHit);
		break;

	//Mem is read/write protected (depends on translation type)
	case MmuError::PROTECTED:
		printf_mmu("MmuError::PROTECTED 0x%X, handled", address);
		if (am == MMU_TT_DWRITE)
			raise(Sh4Ex_TlbProtViolWrite);
		else
			raise(Sh4Ex_TlbProtViolRead);
		return;

	//Mem is write protected , firstwrite
	case MmuError::FIRSTWRITE:
		printf_mmu("MmuError::FIRSTWRITE");
		verify(am == MMU_TT_DWRITE);
		raise(Sh4Ex_TlbInitPageWrite);
		return;

	//data read/write misaligned
	case MmuError::BADADDR:
		if (am == MMU_TT_DWRITE)			//WADDERR - Write Data Address Error
		{
			printf_mmu("MmuError::BADADDR(dw) 0x%X", address);
			raise(Sh4Ex_AddressErrorWrite);
		}
		else if (am == MMU_TT_DREAD)		//RADDERR - Read Data Address Error
		{
			printf_mmu("MmuError::BADADDR(dr) 0x%X", address);
			raise(Sh4Ex_AddressErrorRead);
		}
		else							//IADDERR - Instruction Address Error
		{
#ifdef TRACE_WINCE_SYSCALLS
			if (!print_wince_syscall(address))
#endif
				printf_mmu("MmuError::BADADDR(i) 0x%X", address);
			raise(Sh4Ex_AddressErrorRead);
		}
		return;

	default:
		die("Unknown mmu_error");
	}
}

[[noreturn]] void mmu_raise_exception(MmuError mmu_error, u32 address, u32 am)
{
	mmuException(mmu_error, address, am, [](Sh4ExceptionCode event) {
		debugger::debugTrap(event);	// FIXME CCN_TEA and CCN_PTEH have been updated already

		throw SH4ThrownException(Sh4cntx.pc, event);
	});
	die("Unknown mmu_error");
}


void DoMMUException(u32 address, MmuError mmu_error, u32 access_type)
{
	DEBUG_LOG(SH4, "DoMMUException: fault_addr=0x%08X, mmu_error=%d, access_type=%d, current_Sh4cntx.pc=0x%08X, SR.BL=%d", address, static_cast<int>(mmu_error), access_type, Sh4cntx.pc, Sh4cntx.sr.BL);

	mmuException(mmu_error, address, access_type, [](Sh4ExceptionCode event) {
		DEBUG_LOG(SH4, "DoMMUException: About to call Do_Exception with event=0x%X, Sh4cntx.pc=0x%08X", event, Sh4cntx.pc);
		Do_Exception(Sh4cntx.pc, event);
	});
}

bool mmu_match(u32 va, CCN_PTEH_type Address, CCN_PTEL_type Data)
{
	if (Data.V == 0)
		return false;

	u32 sz = Data.SZ1 * 2 + Data.SZ0;
	u32 mask = mmu_mask[sz];

	if ((((Address.VPN << 10) & mask) == (va & mask)))
	{
		bool needAsidMatch = Data.SH == 0 && (Sh4cntx.sr.MD == 0 || CCN_MMUCR.SV == 0);

		if (!needAsidMatch || Address.ASID == CCN_PTEH.ASID)
			return true;
	}

	return false;
}

#ifndef FAST_MMU
//Do a full lookup on the UTLB entry's
MmuError mmu_full_lookup(u32 va, const TLB_Entry** tlb_entry_ret, u32& rv)
{
	CCN_MMUCR.URC++;
	if (CCN_MMUCR.URB == CCN_MMUCR.URC)
		CCN_MMUCR.URC = 0;

	*tlb_entry_ret = nullptr;
	for (const TLB_Entry& tlb_entry : UTLB)
	{
		if (mmu_match(va, tlb_entry.Address, tlb_entry.Data))
		{
			if (*tlb_entry_ret != nullptr)
				return MmuError::TLB_MHIT;
			*tlb_entry_ret = &tlb_entry;
			u32 sz = tlb_entry.Data.SZ1 * 2 + tlb_entry.Data.SZ0;
			u32 mask = mmu_mask[sz];
			//VPN->PPN | low bits
			rv = ((tlb_entry.Data.PPN << 10) & mask) | (va & ~mask);
		}
	}

	if (*tlb_entry_ret == nullptr)
		return MmuError::TLB_MISS;
	else
		return MmuError::NONE;
}

//Simple QACR translation for mmu (when AT is off)
static u32 mmu_QACR_SQ(u32 va)
{
	int sqi = (va >> 5) & 1;
	u32 addr = (sqi ? CCN_QACR1.Area : CCN_QACR0.Area) << 26;
	addr |= va & 0x03ffffe0;

	return addr;
}

template<u32 translation_type>
MmuError mmu_full_SQ(u32 va, u32& rv)
{
	// Fast bypass for BIOS block copies using the store queue. Any SQ address
	// that targets the first 256 MiB of virtual space is mapped directly to
	// main SDRAM without touching the TLB. This mirrors the behaviour we now
	// apply to ordinary reads/writes and prevents early UTLB-MISS exceptions
	// when the BIOS clears or copies RAM before MMU initialisation.
	if ((va & 0xE0000000) == 0xE0000000)       // SQ window selected
	{
		u32 dst = va & 0x1FFFFFFF;             // strip SQ selector bits
		if (dst < 0x10000000)                 // within 0-0x0FFF_FFFF
		{
			rv = 0x0C000000 | (dst & 0x00FFFFFF); // mirror into first 16 MB
			return MmuError::NONE;
		}
	}

	if (translation_type == MMU_TT_IREAD) {
		if (va & 1)
			return MmuError::BADADDR;
	} else {
		if (va & 3)
			return MmuError::BADADDR;
	}

	// SQMD applies only to the first 512 bytes of the SQ space
	if ((va & 0xFFFFF200) == 0xE0000000 && CCN_MMUCR.SQMD == 1 && Sh4cntx.sr.MD == 0)
		return MmuError::BADADDR;

	if (CCN_MMUCR.AT)
	{
		//Address=Dest&0xFFFFFFE0;

		const TLB_Entry *entry;
		MmuError lookup = mmu_full_lookup(va, &entry, rv);

		rv &= ~31;//lower 5 bits are forced to 0

		if (lookup != MmuError::NONE)
			return lookup;

		u32 md = entry->Data.PR >> 1;

		//Priv mode protection
		if (md == 0 && Sh4cntx.sr.MD == 0)
			return MmuError::PROTECTED;

		//Write Protection (Lock or FW)
		if (translation_type == MMU_TT_DWRITE)
		{
			if ((entry->Data.PR & 1) == 0)
				return MmuError::PROTECTED;
			else if (entry->Data.D == 0)
				return MmuError::FIRSTWRITE;
		}
	}
	else
	{
		rv = mmu_QACR_SQ(va);
	}
	return MmuError::NONE;
}
template MmuError mmu_full_SQ<MMU_TT_DREAD>(u32 va, u32& rv);
template MmuError mmu_full_SQ<MMU_TT_DWRITE>(u32 va, u32& rv);

template<u32 translation_type>
MmuError mmu_data_translation(u32 va, u32& rv)
{
	// Cascade: Added general entry log
	INFO_LOG(MMU, "mmu_data_translation: Entry. va=0x%08X, type=%s, mmuOn=%d, CCN_MMUCR.AT=%d, SR.MD=%d",
	         va, (translation_type == MMU_TT_DWRITE ? "DWRITE" : "DREAD"),
	         mmuOn, CCN_MMUCR.AT, Sh4cntx.sr.MD);

	/* // Original va==0 logging, can be re-enabled if needed
	if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) {
		INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): Entry. SR.MD=%d", Sh4cntx.sr.MD);
	}
	*/

	// P4 area (on-chip I/O, ROM) is always direct-mapped regardless of MMU enable state.
	if ((va & 0xE0000000) == 0xE0000000)
	{
		// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): P4 direct map check.");
		rv = va;
		return MmuError::NONE;
	}

	if (translation_type == MMU_TT_DWRITE)
	{
		if ((va & 0xFC000000) == 0xE0000000) // Store Queues (0xE0xxxxxx - 0xE3xxxxxx)
		{
			// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): SQ Write check.");
			MmuError lookup = mmu_full_SQ<MMU_TT_DWRITE>(va, rv);
			if (lookup != MmuError::NONE)
				return lookup;

			rv = va;	//SQ writes are not translated, only write backs are.
			return MmuError::NONE;
		}
	}

	// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): Evaluating CCN_MMUCR.AT check.");
	if (CCN_MMUCR.AT == 0)
	{
		if (va < 0x02000000)
		{
			rv = va;
			return MmuError::NONE;
		}

		if ((va & 0xE0000000) == 0xE0000000)
		{
			// P4 direct
			rv = va;
		}
		else if ((va & 0x1C000000) == 0x1C000000)
		{
			// Map 0x1C000000-0x1FFFFFFF (store queues) into P4 MMIO window
			rv = va | 0xF0000000;
		}
		else if ((va & 0xE0000000) == 0x80000000 || (va & 0xE0000000) == 0xA0000000)
		{
			// Map P1/P2 to main RAM window (mirror)
			rv = 0x0C000000 | (va & 0x00FFFFFF);
		}
		else
		{
			rv = 0x0C000000 | (va & 0x00FFFFFF);
		}
		return MmuError::NONE;
	}

	// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): Privileged mode user space access check (MD=%d, va_top_bit=%d).", Sh4cntx.sr.MD, (va & 0x80000000) != 0);
	if (Sh4cntx.sr.MD == 0 && (va & 0x80000000) != 0) // User mode trying to access P1-P4 (privileged space)
		//if on kernel, and not SQ addr -> error // This comment seems to refer to SR.MD=1 (kernel) but condition is SR.MD=0 (user)
		return MmuError::BADADDR; // Protection Violation (User mode, Pn access)

	// P1 (0x80000000-0x9FFFFFFF) and P2 (0xA0000000-BFFFFFFF) are always direct-mapped when AT=1
	// This logic is for AT=1. If AT=0, P1/P2 are handled in the AT=0 block.
	if ((va & 0xE0000000) == 0x80000000) // P1
	{
		// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): P1 direct map check.");
		rv = 0x0C000000 | (va & 0x00FFFFFF); // mirror to main RAM window
		return MmuError::NONE;
	}

	if ((va & 0xE0000000) == 0xA0000000) // P2
	{
		// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): P2 direct map check.");
		rv = 0x0C000000 | (va & 0x00FFFFFF);
		return MmuError::NONE;
	}

	// P0/U0 0x7C000000 - 0x7FFFFFFF not translated (when AT=1)
	if ((va & 0xFC000000) == 0x7C000000)
	{
		// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): P0/U0 non-translated (0x7Cxxxxxx) check.");
		rv = va;
		return MmuError::NONE;
	}

	// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): fast_reg_lut[0] check (value=%d).", fast_reg_lut[0]);
	// fast_reg_lut is for FAST_MMU, this is #ifndef FAST_MMU block.
	// This check seems out of place or needs #ifdef FAST_MMU. Assuming it's meant for general P1/P2/P4 bypass if AT=1.
	// However, P1, P2, P4 are already handled above for AT=1 or by the P4 check at the function start.
	// This might be redundant or for a specific configuration.
	// For safety, let's assume it's intended for some edge case if AT=1.
	if (fast_reg_lut[va >> 29] != 0) // Checks top 3 bits for P1,P2,P4 (0x8,0xA,0xE,0xF)
	{
		rv = va;
		return MmuError::NONE;
	}

	// Cascade: Added log before mmu_full_lookup
	INFO_LOG(MEMORY, "mmu_data_translation: PRE_LOOKUP. va=0x%08X, type=%s, mmuOn=%d, CCN_MMUCR.AT=%d, SR.MD=%d", // Changed to MEMORY channel
	         va, (translation_type == MMU_TT_DWRITE ? "DWRITE" : "DREAD"),
	         mmuOn, CCN_MMUCR.AT, Sh4cntx.sr.MD);

	const TLB_Entry *entry;
	// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): Calling mmu_full_lookup.");
	MmuError lookup = mmu_full_lookup(va, &entry, rv);
	// if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): mmu_full_lookup returned %d.", static_cast<int>(lookup));

	if (lookup != MmuError::NONE)
		return lookup;

	u32 md = entry->Data.PR >> 1;

	//0X  & User mode-> protection violation
	//Priv mode protection
	if (md == 0 && Sh4cntx.sr.MD == 0)
		return MmuError::PROTECTED;

	//X0 -> read olny
	//X1 -> read/write , can be FW

	//Write Protection (Lock or FW)
	if (translation_type == MMU_TT_DWRITE)
	{
		if ((entry->Data.PR & 1) == 0)
		{
			if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): Write protected (PR&1 == 0). Entry PR=0x%X", entry->Data.PR);
			return MmuError::PROTECTED;
		}
		else if (entry->Data.D == 0)
		{
			if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): First write (D=0). Entry PR=0x%X, D=%d", entry->Data.PR, entry->Data.D);
			return MmuError::FIRSTWRITE;
		}
	}
	if ((rv & 0x1C000000) == 0x1C000000)
		// map 1C000000-1FFFFFFF to P4 memory-mapped registers
		rv |= 0xF0000000;

	if (va == 0 && translation_type == MMU_TT_DWRITE && CCN_MMUCR.AT == 1) INFO_LOG(MMU, "mmu_data_translation(va=0, DWRITE, AT=1): Returning NONE (successful translation at end of function).");
	return MmuError::NONE;
}
template MmuError mmu_data_translation<MMU_TT_DREAD>(u32 va, u32& rv);
template MmuError mmu_data_translation<MMU_TT_DWRITE>(u32 va, u32& rv);

MmuError mmu_instruction_translation(u32 va, u32& rv)
{
	// Always map Areas 0–3 (0x0000_0000–0x5FFF_FFFF) directly to SDRAM regardless
	// of MMU state. This avoids UTLB misses during the very early boot stages when
	// instruction fetches occur from P0/P2 before the ITLB is populated.
	if (va < 0x60000000)
	{
		rv = 0x0C000000 | (va & 0x00FFFFFF);
		return MmuError::NONE;
	}

	if (CCN_MMUCR.AT == 0)
	{
		if ((va & 0xE0000000) == 0xE0000000)
		{
			// P4 direct
			rv = va;
		}
		else if ((va & 0x1C000000) == 0x1C000000)
		{
			// Store queue window -> mirror into P4
			rv = va | 0xF0000000;
		}
		else if ((va & 0xE0000000) == 0x80000000 || (va & 0xE0000000) == 0xA0000000)
		{
			// P1/P2 mirror to main RAM window
			rv = 0x0C000000 | (va & 0x00FFFFFF);
		}
		else
		{
			// P0/U0 mirror into first 16MB
			rv = va & 0x0FFFFFFF;
		}
		return MmuError::NONE;
	}

	if (Sh4cntx.sr.MD == 0 && (va & 0x80000000) != 0)
		// User mode on kernel address
		return MmuError::BADADDR;

	if ((va >> 29) == 7)
		// P4 not executable
		return MmuError::BADADDR;

	if (fast_reg_lut[va >> 29] != 0)
	{
		// P1 and P2 mirror to main RAM window
		if ((va & 0xE0000000) == 0x80000000 || (va & 0xE0000000) == 0xA0000000)
		{
			rv = 0x0C000000 | (va & 0x00FFFFFF);
		}
		else
		{
			rv = va;
		}
		return MmuError::NONE;
	}

	const TLB_Entry *entry;
	MmuError lookup = mmu_instruction_lookup(va, &entry, rv);
	INFO_LOG(SH4, "MMU_ITRANS_CHECK: va=0x%08X, current SR.BL=%d", va, Sh4cntx.sr.BL);
	if (Sh4cntx.sr.BL == 1) {
		INFO_LOG(SH4, "MMU_ITRANS: SR.BL=1 active, va=0x%08X. mmu_instruction_lookup returned %d. Translated rv=0x%08X", va, static_cast<int>(lookup), rv);
	}
	if (lookup != MmuError::NONE) {
        // Placed before SR.BL check to log for initial faults too.
        if (lookup == MmuError::TLB_MISS) {
            INFO_LOG(SH4, "mmu_instruction_translation: Detected ITLB Miss (MmuError::TLB_MISS from lookup) for va=0x%08X. ASID=0x%X, SR.BL=%d", va, CCN_PTEH.ASID, Sh4cntx.sr.BL);
        } else if (lookup == MmuError::BADADDR) {
            INFO_LOG(SH4, "mmu_instruction_translation: Detected Address Error (MmuError::BADADDR) for va=0x%08X. SR.BL=%d", va, Sh4cntx.sr.BL);
        } else if (lookup == MmuError::PROTECTED) {
            INFO_LOG(SH4, "mmu_instruction_translation: Detected Protection Violation (MmuError::PROTECTED) for va=0x%08X. SR.BL=%d, SR.MD=%d", va, Sh4cntx.sr.BL, Sh4cntx.sr.MD);
        } else if (lookup == MmuError::TLB_MHIT) {
            INFO_LOG(SH4, "mmu_instruction_translation: Detected TLB Multi-Hit (MmuError::TLB_MHIT) for va=0x%08X. SR.BL=%d", va, Sh4cntx.sr.BL);
        } else {
            INFO_LOG(SH4, "mmu_instruction_translation: Detected MmuError %d for va=0x%08X. SR.BL=%d", static_cast<int>(lookup), va, Sh4cntx.sr.BL);
        }

		// If any MMU error occurs during instruction translation AND we are already in an exception (SR.BL=1)
		if (Sh4cntx.sr.BL == 1) {
			DEBUG_LOG(SH4, "MMU: Error (type %d) at 0x%08X while fetching handler for previous exception (SR.BL=1). Setting g_itlb_miss_during_handler_fetch.", static_cast<int>(lookup), va);
			g_itlb_miss_during_handler_fetch = true;
		}
		return lookup;
	}

	u32 md = entry->Data.PR >> 1;

	//0X  & User mode-> protection violation
	//Priv mode protection
	if (md == 0 && Sh4cntx.sr.MD == 0)
		return MmuError::PROTECTED;

	return MmuError::NONE;
}
#endif

MmuError mmu_instruction_lookup(u32 va, const TLB_Entry** tlb_entry_ret, u32& rv)
{
	// PREVIOUS HACK REMOVED - ineffective due to mmu_IReadMem16 fast path

	bool mmach = false;
retry_ITLB_Match:
	*tlb_entry_ret = nullptr;
	for (const TLB_Entry& entry : ITLB)
	{
		if (entry.Data.V == 0)
			continue;
		u32 sz = entry.Data.SZ1 * 2 + entry.Data.SZ0;
		u32 mask = mmu_mask[sz];

		if ((((entry.Address.VPN << 10) & mask) == (va & mask)))
		{
			bool needAsidMatch = entry.Data.SH == 0 && (Sh4cntx.sr.MD == 0 || CCN_MMUCR.SV == 0);

			if (!needAsidMatch || entry.Address.ASID == CCN_PTEH.ASID)
			{
				if (*tlb_entry_ret != nullptr)
					return MmuError::TLB_MHIT;
				*tlb_entry_ret = &entry;
				//VPN->PPN | low bits
				rv = ((entry.Data.PPN << 10) & mask) | (va & ~mask);
			}
		}
	}

	if (*tlb_entry_ret == nullptr)
	{
#ifndef FAST_MMU
        // No ITLB entry matched; this is a pure ITLB miss (instruction fetch), distinguish from UTLB.
        return MmuError::ITLB_MISS;
#else
		// the matching may be approximative
		if (mmach)
            return MmuError::ITLB_MISS;
#endif
		const TLB_Entry *tlb_entry;
		MmuError lookup = mmu_full_lookup(va, &tlb_entry, rv);
		if (lookup != MmuError::NONE)
			return lookup;

		u32 replace_index = ITLB_LRU_USE[CCN_MMUCR.LRUI];
		verify(replace_index != 0xFFFFFFFF);
		ITLB[replace_index] = *tlb_entry;
		ITLB_Sync(replace_index);
		mmach = true;
		goto retry_ITLB_Match;
	}

	CCN_MMUCR.LRUI &= ITLB_LRU_AND[*tlb_entry_ret - ITLB];
	CCN_MMUCR.LRUI |= ITLB_LRU_OR[*tlb_entry_ret - ITLB];

	return MmuError::NONE;
}

void mmu_set_state()
{
	if (CCN_MMUCR.AT == 1)
	{
		// Detect if we're running Windows CE
		static const char magic[] = { 'S', 0, 'H', 0, '-', 0, '4', 0, ' ', 0, 'K', 0, 'e', 0, 'r', 0, 'n', 0, 'e', 0, 'l', 0 };
		if (memcmp(GetMemPtr(0x8c0110a8, 4), magic, sizeof(magic)) == 0
				|| memcmp(GetMemPtr(0x8c011118, 4), magic, sizeof(magic)) == 0)
		{
			mmuOn = true;
			NOTICE_LOG(SH4, "Enabling Full MMU support");
		}
	}
	else
	{
		mmuOn = false;
	}

	SetMemoryHandlers();
	setSqwHandler();
}

#ifdef FAST_MMU
u32 mmuAddressLUT[0x100000];
#endif

void MMU_init()
{
	memset(ITLB_LRU_USE, 0xFF, sizeof(ITLB_LRU_USE));
	for (u32 e = 0; e<4; e++)
	{
		u32 match_key = ((~ITLB_LRU_AND[e]) & 0x3F);
		u32 match_mask = match_key | ITLB_LRU_OR[e];
		for (u32 i = 0; i < std::size(ITLB_LRU_USE); i++)
		{
			if ((i & match_mask) == match_key)
			{
				verify(ITLB_LRU_USE[i] == 0xFFFFFFFF);
				ITLB_LRU_USE[i] = e;
			}
		}
	}
	mmu_set_state();
#ifdef FAST_MMU
	// pre-fill kernel memory
	for (u32 vpn = std::size(mmuAddressLUT) / 2; vpn < std::size(mmuAddressLUT); vpn++)
		mmuAddressLUT[vpn] = vpn << 12;
#endif
}


void MMU_reset()
{
	memset(UTLB, 0, sizeof(UTLB));
	memset(ITLB, 0, sizeof(ITLB));
	mmu_set_state();
	mmu_flush_table();
	memset(sq_remap, 0, sizeof(sq_remap));
}

void MMU_term()
{
}

#ifndef FAST_MMU
void mmu_flush_table()
{
	for (TLB_Entry& entry : ITLB)
		entry.Data.V = 0;
	for (TLB_Entry& entry : UTLB)
		entry.Data.V = 0;
}
#endif

template<typename T>
T DYNACALL mmu_ReadMem(u32 adr)
{
	// Fast path for Boot ROM data reads (2 MiB BIOS mirrored into P0/P1/P2/P3).
	// This avoids costly MMU translation and — more importantly — stops execution
	// from running into zero-padded space beyond the real ROM.
	auto is_bios = [](u32 a, u32& offs) {
		u32 base = a & 0xE0000000u;
		if (base == 0x00000000u || base == 0x80000000u || base == 0xA0000000u || base == 0xC0000000u)
		{
			offs = a & 0x001FFFFFu; // 2 MiB mirror
			return true;
		}
		return false;
	};

	u32 bios_offs;
	if (is_bios(adr, bios_offs))
	{
		// BIOS is word-aligned; unaligned byte/halfword reads are still legal because
		// the SH-4 core provides little-endian ordering. Fetch the native-width value
		// directly from the ROM buffer using the pre-masked offset.
		if (bios_offs >= settings.platform.bios_size)
		{
			// Read past real BIOS contents – raise BADADDR so the core faults instead
			// of reading 0-fill and eventually falling through to PC = 0.
			mmu_raise_exception(MmuError::BADADDR, adr, MMU_TT_DREAD);
		}
		return *reinterpret_cast<const T*>(nvmem::getBiosData() + bios_offs);
	}

	// Fast path for SDRAM access in cached/uncached areas (0x0000_0000–0x5FFF_FFFF).
	// When the MMU is enabled early in boot there are no valid TLB entries for
	// these regions yet; however the SH-4 still performs a simple address mask
	// to physical SDRAM. Mirror the behaviour by short-circuiting translation and
	// mapping the address into physical area C (0x0C00_0000).
	if (adr < 0x60000000)
	{
		// Map to physical SDRAM window. Guard against out-of-bounds offsets so a bad
		// guest address cannot crash the host.
		u32 offset = adr & 0x00FFFFFF; // 16-MB window used by Dreamcast BIOS init
		const u32 ram_size = settings.platform.ram_size ? settings.platform.ram_size : 0x01000000; // fallback 16 MiB
		if (offset >= ram_size)
			mmu_raise_exception(MmuError::BADADDR, adr, MMU_TT_DREAD);

		u32 phys = 0x0C000000 | offset;
		return addrspace::readt<T>(phys);
	}

	// Unaligned check — skip it for P4 MMIO space (0xE000_0000-0xFFFF_FFFF)
	if ((adr & 0xE0000000) != 0xE0000000)
	{
		if (adr & (std::min((int)sizeof(T), 4) - 1))
			mmu_raise_exception(MmuError::BADADDR, adr, MMU_TT_DREAD);
	}

	u32 phys;
	MmuError rv = mmu_data_translation<MMU_TT_DREAD>(adr, phys);
	if (rv != MmuError::NONE)
		mmu_raise_exception(rv, adr, MMU_TT_DREAD);

	return addrspace::readt<T>(phys);
}
template u8 mmu_ReadMem(u32 adr);
template u16 mmu_ReadMem(u32 adr);
template u32 mmu_ReadMem(u32 adr);
template u64 mmu_ReadMem(u32 adr);

u16 DYNACALL mmu_IReadMem16(u32 vaddr)
{
    INFO_LOG(SH4, "mmu_IReadMem16: Entry. vaddr=0x%08X, mmuOn=%d, CCN_MMUCR.AT=%d", vaddr, mmuOn, CCN_MMUCR.AT);
	// BIOS ROM is only visible in the A0/C0 regions (cached + uncached) and their P1/P2 mirrors.
    // Accesses in P0 (0x00000000–0x7FFFFFFF) should point to SDRAM, NOT to the BIOS. Using the BIOS
    // there caused the bogus wrap-to-zero bug we are chasing.
    u32 area = vaddr & 0xE0000000;
    if ((area == 0x00000000 && mmuOn == 0) || area == 0xA0000000 || area == 0xC0000000 || area == 0x80000000)
    {
        // Mirror mask to 2 MiB window for all ROM aliases.
        u32 bios_offset = vaddr & 0x001FFFFF;
        if (bios_offset < settings.platform.bios_size)
        {
            return *reinterpret_cast<const u16*>(nvmem::getBiosData() + bios_offset);
        }
        // Out of range – log once and raise BADADDR so the core doesn’t silently consume NOPs.
        INFO_LOG(SH4, "BIOS fetch OOB @ %08X (offset %X / size %u)", vaddr, bios_offset, settings.platform.bios_size);
        mmu_raise_exception(MmuError::BADADDR, vaddr, MMU_TT_IREAD);
        return 0;
    }

	// If not a BIOS read, proceed with standard MMU translation.
	u32 paddr;
    MmuError err = mmu_instruction_translation(vaddr, paddr);
    if (err != MmuError::NONE)
    {
        // If mmu_instruction_translation returns an error (e.g. page fault, protection violation, etc.,
        // even if AT is on), then raise the appropriate MMU exception.
        mmu_raise_exception(err, vaddr, MMU_TT_IREAD);
        return 0;
    }

	return *reinterpret_cast<const u16*>(&mem_b[paddr]);
}

template<typename T>
void DYNACALL mmu_WriteMem(u32 adr, T data)
{
    // Standard path for all writes. MMU translation and protection are handled by mmu_data_translation.
    INFO_LOG(MEMORY, "mmu_WriteMem: adr=0x%08X, mmuOn=%d, CCN_MMUCR.AT=%d, sizeof(T)=%zu", adr, mmuOn, CCN_MMUCR.AT, sizeof(T));

    // 1. Alignment Check (on Virtual Address for relevant regions)
    // Applicable to U0, P0, P1, P2, P3 (VA < 0xC0000000)
    if (adr < 0xC0000000) // Equivalent to (adr & 0xC0000000) != 0xC0000000
    {
        if (adr & (sizeof(T) - 1)) // Corrected alignment check for any power-of-2 sizeof(T)
        {
            mmu_raise_exception(MmuError::BADADDR, adr, MMU_TT_DWRITE);
            return; // Exception raised, stop further processing.
        }
    }

    // 2. MMU Translation
    u32 physical_address; // To store the translated physical address
    MmuError translation_status = mmu_data_translation<MMU_TT_DWRITE>(adr, physical_address);

    if (translation_status != MmuError::NONE)
    {
        mmu_raise_exception(translation_status, adr, MMU_TT_DWRITE);
        return; // Exception was raised by mmu_raise_exception.
    }

    // 3. Write to Physical Address
    // If mmu_data_translation succeeded, physical_address is valid.
    addrspace::writet<T>(physical_address, data);
}
template void mmu_WriteMem(u32 adr, u8 data);
template void mmu_WriteMem(u32 adr, u16 data);
template void mmu_WriteMem(u32 adr, u32 data);
template void mmu_WriteMem(u32 adr, u64 data);

void mmu_TranslateSQW(u32 adr, u32 *out)
{
	if (!mmuOn)
	{
		//This will only work for 1 mb pages .. hopefully nothing else is used
		//*FIXME* to work for all page sizes ?

		*out = sq_remap[(adr >> 20) & 0x3F] | (adr & 0xFFFE0);
	}
	else
	{
		u32 addr;
		MmuError tv = mmu_full_SQ<MMU_TT_DREAD>(adr, addr);
		if (tv != MmuError::NONE)
			mmu_raise_exception(tv, adr, MMU_TT_DREAD);

		*out = addr;
	}
}

void mmu_serialize(Serializer& ser)
{
	ser << UTLB;
	ser << ITLB;
	ser << sq_remap;
}

void mmu_deserialize(Deserializer& deser)
{
	deser.skip(8, Deserializer::V33);	// CCN_QACR_TR

	deser >> UTLB;
	deser >> ITLB;

	deser >> sq_remap;
	deser.skip(64 * 4, Deserializer::V23); // ITLB_LRU_USE
}
