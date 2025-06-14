/*
	Interrupt list caching and handling

	SH4 has a very flexible interrupt controller. In order to handle it efficiently, a sorted
	interrupt bitfield is build from the set interrupt priorities. Higher priorities get allocated
	into higher bits, and a simple mask is kept. In order to check for pending interrupts a simple
	!=0 test works, and to identify the pending interrupt bsr(pend) will give the sorted id. As
	this is a single cycle operation on most platforms, the interrupt checking/identification
	is very fast !
*/


// Flag to indicate if the last exception was Sh4Ex_TlbMissWrite
// Defined here, declared extern in mmu.cpp for diagnostics
bool g_just_had_tlb_miss_write_exception = false;
bool g_itlb_miss_during_handler_fetch = false; // True if ITLB miss occurs while SR.BL=1 (fetching handler)
#include "types.h"
#include "sh4_interrupts.h"
#include "sh4_core.h"
#include "hw/sh4/modules/mmu.h"
#include "sh4_mmr.h"
#include "oslib/oslib.h"
#include "debug/gdb_server.h"
#include "serialize.h"
#include "emulator.h"
#include <cassert>

//these are fixed
const u16 IRLPriority = 0x0246;
#define IRLP9 &IRLPriority,0
#define IRLP11 &IRLPriority,4
#define IRLP13 &IRLPriority,8

#define GIPA(p) &INTC_IPRA.reg_data,4*p
#define GIPB(p) &INTC_IPRB.reg_data,4*p
#define GIPC(p) &INTC_IPRC.reg_data,4*p

struct InterptSourceList_Entry
{
	const u16* PrioReg;
	u32 Shift;
	Sh4ExceptionCode IntEvnCode;

	int GetPrLvl() const { return (*PrioReg >> Shift) & 0xF; }
};

static const InterptSourceList_Entry InterruptSourceList[sh4_INT_ID_COUNT] =
{
	//IRL
	{ IRLP9, Sh4Ex_ExtInterrupt9 },		//sh4_IRL_9
	{ IRLP11, Sh4Ex_ExtInterruptB },	//sh4_IRL_11
	{ IRLP13, Sh4Ex_ExtInterruptD },	//sh4_IRL_13

	//HUDI
	{ GIPC(0), Sh4Ex_HUDI },			//sh4_HUDI_HUDI

	//GPIO (missing on dc ?)
	{ GIPC(3), Sh4Ex_GPIO },			//sh4_GPIO_GPIOI

	//DMAC
	{ GIPC(2), Sh4Ex_DMAC_DTME0 },		//sh4_DMAC_DMTE0
	{ GIPC(2), Sh4Ex_DMAC_DTME1 },		//sh4_DMAC_DMTE1
	{ GIPC(2), Sh4Ex_DMAC_DTME2 },		//sh4_DMAC_DMTE2
	{ GIPC(2), Sh4Ex_DMAC_DTME3 },		//sh4_DMAC_DMTE3
	{ GIPC(2), Sh4Ex_DMAC_DMAE },		//sh4_DMAC_DMAE

	//TMU
	{ GIPA(3), Sh4Ex_TMU0 },			//sh4_TMU0_TUNI0
	{ GIPA(2), Sh4Ex_TMU1 },			//sh4_TMU1_TUNI1
	{ GIPA(1), Sh4Ex_TMU2 },			//sh4_TMU2_TUNI2
	{ GIPA(1), Sh4Ex_TMU2_TICPI2 },		//sh4_TMU2_TICPI2

	//RTC
	{ GIPA(0), Sh4Ex_RTC_ATI },			//sh4_RTC_ATI
	{ GIPA(0), Sh4Ex_RTC_PRI },			//sh4_RTC_PRI
	{ GIPA(0), Sh4Ex_RTC_CUI },			//sh4_RTC_CUI

	//SCI
	{ GIPB(1), Sh4Ex_SCI_ERI },			//sh4_SCI1_ERI
	{ GIPB(1), Sh4Ex_SCI_RXI },			//sh4_SCI1_RXI
	{ GIPB(1), Sh4Ex_SCI_TXI },			//sh4_SCI1_TXI
	{ GIPB(1), Sh4Ex_SCI_TEI },			//sh4_SCI1_TEI

	//SCIF
	{ GIPC(1), Sh4Ex_SCIF_ERI },		//sh4_SCIF_ERI
	{ GIPC(1), Sh4Ex_SCIF_RXI },		//sh4_SCIF_RXI
	{ GIPC(1), Sh4Ex_SCIF_BRI },		//sh4_SCIF_BRI
	{ GIPC(1), Sh4Ex_SCIF_TXI },		//sh4_SCIF_TXI

	//WDT
	{ GIPB(3), Sh4Ex_WDT },				//sh4_WDT_ITI

	//REF
	{ GIPB(2), Sh4Ex_REF_RCMI },		//sh4_REF_RCMI
	{ GIPA(2), Sh4Ex_REF_ROVI },		//sh4_REF_ROVI
};

//Maps siid -> EventID
alignas(64) static Sh4ExceptionCode InterruptEnvId[32];
//Maps piid -> 1<<siid
alignas(64) static u32 InterruptBit[32];
//Maps sh4 interrupt level to inclusive bitfield
alignas(64) static u32 InterruptLevelBit[16];

static void Do_Interrupt(Sh4ExceptionCode intEvn);

static u32 interrupt_vpend; // Vector of pending interrupts
static u32 interrupt_vmask; // Vector of masked interrupts             (-1 inhibits all interrupts)
static u32 decoded_srimask; // Vector of interrupts allowed by SR.IMSK (-1 inhibits all interrupts)

//bit 0 ~ 27 : interrupt source 27:0. 0 = lowest level, 27 = highest level.
static void recalc_pending_itrs()
{
	Sh4cntx.interrupt_pend = interrupt_vpend & interrupt_vmask & decoded_srimask;
}

//Rebuild sorted interrupt id table (priorities were updated)
void SIIDRebuild()
{
	int cnt = 0;
	u32 vpend = interrupt_vpend;
	u32 vmask = interrupt_vmask;
	interrupt_vpend = 0;
	interrupt_vmask = 0;
	//rebuild interrupt table
	for (int ilevel = 0; ilevel < 16; ilevel++)
	{
		for (int isrc = 0; isrc < sh4_INT_ID_COUNT; isrc++)
		{
			if (InterruptSourceList[isrc].GetPrLvl() == ilevel)
			{
				InterruptEnvId[cnt] = InterruptSourceList[isrc].IntEvnCode;
				u32 p = InterruptBit[isrc] & vpend;
				u32 m = InterruptBit[isrc] & vmask;
				InterruptBit[isrc] = 1 << cnt;
				if (p)
					interrupt_vpend |= InterruptBit[isrc];
				if (m)
					interrupt_vmask |= InterruptBit[isrc];
				cnt++;
			}
		}
		InterruptLevelBit[ilevel] = (1 << cnt) - 1;
	}
	SRdecode();
}

//Decode SR.IMSK into a interrupt mask, update and return the interrupt state
bool SRdecode()
{
	if (Sh4cntx.sr.BL)
		decoded_srimask=~0xFFFFFFFF;
	else
		decoded_srimask=~InterruptLevelBit[Sh4cntx.sr.IMASK];

	recalc_pending_itrs();
	return Sh4cntx.interrupt_pend;
}

int UpdateINTC()
{
	if (!Sh4cntx.interrupt_pend)
		return 0;

	Do_Interrupt(InterruptEnvId[bitscanrev(Sh4cntx.interrupt_pend)]);
	return 1;
}

void SetInterruptPend(InterruptID intr)
{
	interrupt_vpend |= InterruptBit[intr];
	recalc_pending_itrs();
}
void ResetInterruptPend(InterruptID intr)
{
	interrupt_vpend &= ~InterruptBit[intr];
	recalc_pending_itrs();
}

void SetInterruptMask(InterruptID intr)
{
	interrupt_vmask |= InterruptBit[intr];
	recalc_pending_itrs();
}
void ResetInterruptMask(InterruptID intr)
{
	interrupt_vmask &= ~InterruptBit[intr];
	recalc_pending_itrs();
}

static void Do_Interrupt(Sh4ExceptionCode intEvn)
{
	CCN_INTEVT = intEvn;

	Sh4cntx.ssr = Sh4cntx.sr.getFull();
	Sh4cntx.spc = Sh4cntx.pc;
	Sh4cntx.sgr = Sh4cntx.r[15];
	Sh4cntx.sr.BL = 1;
	Sh4cntx.sr.MD = 1;
	Sh4cntx.sr.RB = 1;
	UpdateSR();
	Sh4cntx.pc = Sh4cntx.vbr + 0x600;
	debugger::subroutineCall();
}

void Do_Exception(u32 epc, Sh4ExceptionCode expEvn)
{
    DEBUG_LOG(SH4, "Do_Exception: Called with expEvn=0x%03X, epc=0x%08X, Sh4cntx.vbr=0x%08X, CCN_TEA=0x%08X. SR.BL=%d", (u32)expEvn, epc, Sh4cntx.vbr, CCN_TEA, Sh4cntx.sr.BL);

	assert((expEvn >= Sh4Ex_TlbMissRead && expEvn <= Sh4Ex_SlotIllegalInstr)
			|| expEvn == Sh4Ex_FpuDisabled || expEvn == Sh4Ex_SlotFpuDisabled || expEvn == Sh4Ex_UserBreak);
	if (Sh4cntx.sr.BL != 0) {
		DEBUG_LOG(SH4, "Do_Exception: Double fault detected. SR.BL=1 at entry. Current event=0x%03X, EPC=0x%08X", (u32)expEvn, epc);
		if (g_itlb_miss_during_handler_fetch && (expEvn == Sh4Ex_TlbMissRead || expEvn == Sh4Ex_TlbMissWrite)) {
             DEBUG_LOG(SH4, "Do_Exception: Double fault cause: MMU error during handler fetch. Current (second) exception event: 0x%03X, EPC: 0x%08X", (u32)expEvn, epc);
        }
        WARN_LOG(SH4, "Do_Exception: DOUBLE FAULT DETECTED. Initiating CPU Reset. Event: 0x%03X, EPC: 0x%08X", (u32)expEvn, epc);
        emu.getSh4Executor()->Reset(false); // false for programmatic reset
        return; // CPU reset will take over
    }

    // Clear the flag that we just had a TLB miss write exception, as we are now processing an exception
    // (either the original one, or a double fault that will lead to reset)
    // This reset should happen *before* a potential double fault causes a CPU reset and return.
    // So, if we reach here, it's not a double fault, or it's the first fault.
    if (g_just_had_tlb_miss_write_exception && expEvn != Sh4Ex_TlbMissWrite) {
        g_just_had_tlb_miss_write_exception = false;
    }

	CCN_EXPEVT = expEvn;

	Sh4cntx.ssr = Sh4cntx.sr.status;
	Sh4cntx.spc = epc;
	Sh4cntx.sgr = Sh4cntx.r[15];
	Sh4cntx.sr.BL = 1;
	Sh4cntx.sr.MD = 1;
	Sh4cntx.sr.RB = 1;
	g_itlb_miss_during_handler_fetch = true; // Now in an exception state, flag that handler fetch is next critical step
	UpdateSR();

	DEBUG_LOG(SH4, "Do_Exception: Pre-handler jump. SR.BL=%d, g_itlb_miss_during_handler_fetch=%s. Current expEvn=0x%03X, epc=0x%08X", Sh4cntx.sr.BL, g_itlb_miss_during_handler_fetch ? "true" : "false", (u32)expEvn, epc);

	// Set flag based on the current exception type before jumping to handler
	if (expEvn == Sh4Ex_TlbMissWrite) {
	    g_just_had_tlb_miss_write_exception = true;
	    DEBUG_LOG(SH4, "Do_Exception: Setting g_just_had_tlb_miss_write_exception=true for Sh4Ex_TlbMissWrite (expEvn=0x%X)", expEvn);
	} else {
	    g_just_had_tlb_miss_write_exception = false;
	}

	u32 vector_offset = (expEvn == Sh4Ex_TlbMissRead || expEvn == Sh4Ex_TlbMissWrite ? 0x400 : 0x100);
	u32 new_pc = Sh4cntx.vbr + vector_offset;
	DEBUG_LOG(SH4, "Do_Exception: Calculated Handler. VectorOffset=0x%X, Target Handler PC=0x%08X. SR.BL is now %d.", vector_offset, new_pc, Sh4cntx.sr.BL);
	Sh4cntx.pc = new_pc;
	// debugger::subroutineCall(); // Temporarily commented out for double fault diagnosis

	// Diagnostic: log MMU/TLB exceptions with offending address to aid IR bring-up
	if (expEvn == Sh4Ex_TlbMissRead || expEvn == Sh4Ex_TlbMissWrite ||
		expEvn == Sh4Ex_TlbProtViolRead || expEvn == Sh4Ex_TlbProtViolWrite ||
		expEvn == Sh4Ex_AddressErrorRead || expEvn == Sh4Ex_AddressErrorWrite)
	{
		// TEA register contains the virtual address that caused the fault (on SH-4)
		INFO_LOG(SH4, "MMU exception %03X at EPC=%08X TEA=%08X", (u32)expEvn, epc, CCN_TEA);
	}
	//printf("RaiseException: from pc %08x to %08x, event %x\n", epc, next_pc, expEvn);
}


//Init/Res/Term
void interrupts_init()
{
}

void interrupts_reset()
{
	//reset interrupts cache
	interrupt_vpend = 0;
	interrupt_vmask = 0xFFFFFFFF;
	decoded_srimask = 0;

	for (u32 i = 0; i < sh4_INT_ID_COUNT; i++)
		InterruptBit[i] = 1 << i;

	//rebuild the interrupts table
	SIIDRebuild();
}

void interrupts_term()
{
}

void interrupts_serialize(Serializer& ser)
{
	ser << InterruptEnvId;
	ser << InterruptBit;
	ser << InterruptLevelBit;
	ser << interrupt_vpend;
	ser << interrupt_vmask;
	ser << decoded_srimask;

}

void interrupts_deserialize(Deserializer& deser)
{
	deser >> InterruptEnvId;
	deser >> InterruptBit;
	deser >> InterruptLevelBit;
	deser >> interrupt_vpend;
	deser >> interrupt_vmask;
	deser >> decoded_srimask;
}
