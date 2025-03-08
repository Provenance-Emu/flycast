/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "../sh4_cache.h"
#include "debug/gdb_server.h"
#include "../sh4_cycles.h"

float sh4_cpu_timescale = 1.0f;

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

// Use the optimized memory functions from sh4_mem.cpp
extern u32 DYNACALL optimized_read32(u32 addr);
extern void DYNACALL optimized_write32(u32 addr, u32 data);

// Optimize floating point operations with NEON
static inline float optimized_float_add(float a, float b)
{
	float32x2_t va = vdup_n_f32(a);
	float32x2_t vb = vdup_n_f32(b);
	float32x2_t result = vadd_f32(va, vb);
	return vget_lane_f32(result, 0);
}

static inline float optimized_float_mul(float a, float b)
{
	float32x2_t va = vdup_n_f32(a);
	float32x2_t vb = vdup_n_f32(b);
	float32x2_t result = vmul_f32(va, vb);
	return vget_lane_f32(result, 0);
}

// Optimize vector operations with NEON
static inline void optimized_vector_mul(float *dst, const float *src, float factor, int count)
{
	float32x4_t vfactor = vdupq_n_f32(factor);

	for (int i = 0; i < count; i += 4)
	{
		float32x4_t vsrc = vld1q_f32(src + i);
		float32x4_t vresult = vmulq_f32(vsrc, vfactor);
		vst1q_f32(dst + i, vresult);
	}
}

// Optimize memory copy with NEON
static inline void optimized_memcpy(void *dst, const void *src, size_t size)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	// Handle small copies directly
	if (size < 16)
	{
		for (size_t i = 0; i < size; i++)
			d[i] = s[i];
		return;
	}

	// Align to 16-byte boundary
	size_t pre = (16 - (size_t)d) & 15;
	if (pre > 0)
	{
		for (size_t i = 0; i < pre; i++)
			d[i] = s[i];
		d += pre;
		s += pre;
		size -= pre;
	}

	// Copy 16 bytes at a time
	size_t main = size & ~15;
	for (size_t i = 0; i < main; i += 16)
	{
		uint8x16_t v = vld1q_u8(s + i);
		vst1q_u8(d + i, v);
	}

	// Copy remaining bytes
	for (size_t i = main; i < size; i++)
		d[i] = s[i];
}
#endif

Sh4ICache icache;
Sh4OCache ocache;
Sh4Interpreter *Sh4Interpreter::Instance;

// Add this declaration at the top of the file with other global variables
static bool sh4_int_bCpuRun = false;

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
	// Use optimized execution for common opcodes
	switch (op)
	{
		// Optimize memory access opcodes
		case 0x2000: // MOV.B Rm,@Rn
		case 0x2001: // MOV.W Rm,@Rn
		case 0x2002: // MOV.L Rm,@Rn
		{
			u32 n = ((op >> 8) & 0xf);
			u32 m = ((op >> 4) & 0xf);
			u32 addr = ctx->r[n];
			u32 data = ctx->r[m];

			if ((op & 3) == 2) // MOV.L
				optimized_write32(addr, data);
			else
				OpPtr[op](ctx, op); // Fall back for other sizes

			sh4cycles.executeCycles(op);
			return;
		}

		// Optimize floating point opcodes
		case 0xF000: // FADD Rm,Rn
		{
			u32 n = ((op >> 8) & 0xf);
			u32 m = ((op >> 4) & 0xf);
			ctx->fr[n] = optimized_float_add(ctx->fr[n], ctx->fr[m]);
			sh4cycles.executeCycles(op);
			return;
		}

		case 0xF002: // FMUL Rm,Rn
		{
			u32 n = ((op >> 8) & 0xf);
			u32 m = ((op >> 4) & 0xf);
			ctx->fr[n] = optimized_float_mul(ctx->fr[n], ctx->fr[m]);
			sh4cycles.executeCycles(op);
			return;
		}

		default:
			// Use standard execution for other opcodes
			OpPtr[op](ctx, op);
			break;
	}
#else
	// Standard execution
	OpPtr[op](ctx, op);
#endif

	sh4cycles.executeCycles(op);
}

u16 Sh4Interpreter::ReadNexOp()
{
	u32 addr = ctx->pc;
	if (!mmu_enabled() && (addr & 1))
		// address error
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);

	ctx->pc = addr + 2;

	return IReadMem16(addr);
}

void Sh4Interpreter::Run()
{
	Instance = this;
	ctx->restoreHostRoundingMode();

	try {
		do
		{
			try {
				do
				{
					u32 op = ReadNexOp();

					ExecuteOpcode(op);
				} while (ctx->cycle_counter > 0);
				ctx->cycle_counter += SH4_TIMESLICE;
				UpdateSystem_INTC();
			} catch (const SH4ThrownException& ex) {
				Do_Exception(ex.epc, ex.expEvn);
				// an exception requires the instruction pipeline to drain, so approx 5 cycles
				sh4cycles.addCycles(5 * CPU_RATIO);
			}
		} while (ctx->CpuRunning);
	} catch (const debugger::Stop&) {
	}

	ctx->CpuRunning = false;
	Instance = nullptr;
}

void Sh4Interpreter::Start()
{
	ctx->CpuRunning = true;
}

void Sh4Interpreter::Stop()
{
	ctx->CpuRunning = false;
}

void Sh4Interpreter::Step()
{
	verify(!ctx->CpuRunning);
	Instance = this;

	ctx->restoreHostRoundingMode();
	try {
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
	} catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
		// an exception requires the instruction pipeline to drain, so approx 5 cycles
		sh4cycles.addCycles(5 * CPU_RATIO);
	} catch (const debugger::Stop&) {
	}
	Instance = nullptr;
}

void Sh4Interpreter::Reset(bool hard)
{
	verify(!ctx->CpuRunning);

	if (hard)
	{
		int schedNext = ctx->sh4_sched_next;
		memset(ctx, 0, sizeof(*ctx));
		ctx->sh4_sched_next = schedNext;
	}
	ctx->pc = 0xA0000000;

	memset(ctx->r, 0, sizeof(ctx->r));
	memset(ctx->r_bank, 0, sizeof(ctx->r_bank));

	ctx->gbr = ctx->ssr = ctx->spc = ctx->sgr = ctx->dbr = ctx->vbr = 0;
	ctx->mac.full = ctx->pr = ctx->fpul = 0;

	ctx->sr.setFull(0x700000F0);
	ctx->old_sr.status = ctx->sr.status;
	UpdateSR();

	ctx->fpscr.full = 0x00040001;
	ctx->old_fpscr = ctx->fpscr;

	icache.Reset(hard);
	ocache.Reset(hard);
	sh4cycles.reset();
	ctx->cycle_counter = SH4_TIMESLICE;

	INFO_LOG(INTERPRETER, "Sh4 Reset");
}

bool Sh4Interpreter::IsCpuRunning()
{
	return ctx->CpuRunning;
}

//TODO : Check for valid delayslot instruction
void Sh4Interpreter::ExecuteDelayslot()
{
	try {
		u32 op = ReadNexOp();

		ExecuteOpcode(op);
	} catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

void Sh4Interpreter::ExecuteDelayslot_RTE()
{
	try {
		// In an RTE delay slot, status register (SR) bits are referenced as follows.
		// In instruction access, the MD bit is used before modification, and in data access,
		// the MD bit is accessed after modification.
		// The other bits—S, T, M, Q, FD, BL, and RB—after modification are used for delay slot
		// instruction execution. The STC and STC.L SR instructions access all SR bits after modification.
		u32 op = ReadNexOp();
		// Now restore all SR bits
		ctx->sr.setFull(ctx->ssr);
		// And execute
		ExecuteOpcode(op);
	} catch (const SH4ThrownException&) {
		throw FlycastException("Fatal: SH4 exception in RTE delay slot");
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

// every SH4_TIMESLICE cycles
int UpdateSystem_INTC()
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next < 0)
		sh4_sched_tick(SH4_TIMESLICE);
	if (Sh4cntx.interrupt_pend)
		return UpdateINTC();
	else
		return 0;
}

void Sh4Interpreter::Init()
{
	ctx = &p_sh4rcb->cntx;
	memset(ctx, 0, sizeof(*ctx));
	sh4cycles.init(ctx);
	icache.init(ctx);
	ocache.init(ctx);
}

void Sh4Interpreter::Term()
{
	Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}

Sh4Executor *Get_Sh4Interpreter()
{
	return new Sh4Interpreter();
}

// Then modify the Sh4_int_Run function
void Sh4_int_Run()
{
	sh4_int_bCpuRun = true;

	// Use a larger cycle batch size for better performance
	static const int CYCLE_BATCH_SIZE = 10000;

	while (sh4_int_bCpuRun)
	{
		// Apply CPU frequency scaling to the batch size
		int scaled_batch_size = (int)(CYCLE_BATCH_SIZE * sh4_cpu_timescale);
		scaled_batch_size = std::max(1000, std::min(30000, scaled_batch_size));

		// Process in larger batches for better efficiency
		for (int i = 0; i < scaled_batch_size && sh4_int_bCpuRun; i++)
		{
			try {
				// Use the existing ExecuteOpcode function
				u32 op = Sh4Interpreter::Instance->ReadNexOp();
				Sh4Interpreter::Instance->ExecuteOpcode(op);
			}
			catch (SH4ThrownException& ex) {
				// Call the Do_Exception function from the Sh4 core
				::Do_Exception(ex.epc, ex.expEvn);
			}

			// Check if we need to update the system
			if (Sh4Interpreter::Instance->UpdateSystem()) {
				// System updated, might need to break the batch
				break;
			}
		}

		// Allow other threads to run
		std::this_thread::yield();
	}
}

bool Sh4Interpreter::UpdateSystem()
{
	if (ctx->cycle_counter <= 0)
	{
		ctx->cycle_counter += SH4_TIMESLICE;
		UpdateSystem_INTC();
		return true;
	}
	return false;
}
