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
#include "deps/xxHash/xxhash.h"
#include "hw/sh4/interpr/sh4_opcodes.h" // Include for CHECK_FPU macros

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

static OpCallFP* sh4_opcode_handlers[0x10000];

// Cache for instruction fetches to avoid repeated memory lookups
// Size: 64K entries * (2 bytes opcode + 4 bytes addr + 1 byte valid) = ~448 KB
static u16 op_cache[1 << 16]; // 64K entries for 16-bit index
static u32 op_cache_addr[1 << 16];
static bool op_cache_valid[1 << 16];

Sh4ICache icache;
Sh4OCache ocache;
Sh4Interpreter *Sh4Interpreter::Instance;

Sh4Interpreter::Sh4Interpreter()
{
    Instance = this;
}

Sh4Interpreter::~Sh4Interpreter()
{
    if (Instance == this)
    {
        Instance = nullptr;
    }
}

// Add this declaration at the top of the file with other global variables
static bool sh4_int_bCpuRun = false;

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

	// Treat 0x0000 (and 0x0009) as NOP to mirror IR behaviour and avoid illegal opcode exceptions
	if (op == 0x0000 || op == 0x0009)
	{
		ctx->pc += 2; // advance to next instruction
		sh4cycles.executeCycles(op);
		return;
	}

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
			u32 n = GetN(op);
			u32 m = GetM(op);
			if (ctx->fpscr.PR == 0) // Single precision
			{
				ctx->fr[n] += ctx->fr[m];
				CHECK_FPU_32(ctx->fr[n]);
			}
			else // Double precision
			{
				u64* d = (u64*)&ctx->fr[0];
				d[n >> 1] += d[m >> 1];
				CHECK_FPU_64(d[n >> 1]);
			}
			sh4cycles.executeCycles(op);
			return;
		}

		case 0xF001: // FSUB FRm, FRn
		{
			u32 n = GetN(op);
			u32 m = GetM(op);
			if (ctx->fpscr.PR == 0) // Single precision
			{
				ctx->fr[n] -= ctx->fr[m];
				CHECK_FPU_32(ctx->fr[n]);
			}
			else // Double precision
			{
				u64* d = (u64*)&ctx->fr[0];
				d[n >> 1] -= d[m >> 1];
				CHECK_FPU_64(d[n >> 1]);
			}
			sh4cycles.executeCycles(op);
			return;
		}

		case 0xF002: // FMUL FRm, FRn
		{
			u32 n = GetN(op);
			u32 m = GetM(op);
			if (ctx->fpscr.PR == 0) // Single precision
			{
				ctx->fr[n] *= ctx->fr[m];
				CHECK_FPU_32(ctx->fr[n]);
			}
			else // Double precision
			{
				u64* d = (u64*)&ctx->fr[0];
				d[n >> 1] *= d[m >> 1];
				CHECK_FPU_64(d[n >> 1]);
			}
			sh4cycles.executeCycles(op);
			return;
		}

		case 0xF003: // FDIV FRm, FRn
		{
			u32 n = GetN(op);
			u32 m = GetM(op);
			if (ctx->fpscr.PR == 0) // Single precision
			{
				ctx->fr[n] /= ctx->fr[m];
				CHECK_FPU_32(ctx->fr[n]);
			}
			else // Double precision
			{
				u64* d = (u64*)&ctx->fr[0];
				d[n >> 1] /= d[m >> 1];
				CHECK_FPU_64(d[n >> 1]);
			}
			sh4cycles.executeCycles(op);
			return;
		}

		case 0xF00C: // FMOV FRm, FRn (or DRm, DRn etc.)
		{
			if (ctx->fpscr.SZ == 0) // Single precision FRm -> FRn
			{
				u32 n = GetN(op);
				u32 m = GetM(op);
				ctx->fr[n] = ctx->fr[m];
			}
			else // Double precision (DR/XD)
			{
				u32 n_idx = GetN(op) >> 1;
				u32 m_idx = GetM(op) >> 1;
				u64* d = (u64*)&ctx->fr[0];
				switch ((op >> 4) & 0x11)
				{
					case 0x00: d[n_idx] = d[m_idx]; break;         // DRm -> DRn
					case 0x01: d[n_idx] = d[m_idx + 16]; break; // XDm -> DRn
					case 0x10: d[n_idx + 16] = d[m_idx]; break; // DRm -> XDn
					case 0x11: d[n_idx + 16] = d[m_idx + 16]; break;// XDm -> XDn
				}
			}
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
	const u32 pc_before = ctx->pc;
	const u16 index = pc_before & 0xFFFF;

	// Check cache first
	if (op_cache_valid[index] && op_cache_addr[index] == pc_before)
	{
		ctx->pc += 2;
		return op_cache[index];
	}

	// Cache miss: read from memory
	u16 op = IReadMem16(pc_before);
	ctx->pc += 2;

	// Store in cache
	op_cache[index] = op;
	op_cache_addr[index] = pc_before;
	op_cache_valid[index] = true;

	return op;
}

void Sh4Interpreter::Run()
{
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

	g_itlb_miss_during_handler_fetch = false;

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
		u16 op = ReadNexOp();
 
		// --- NOP Optimization ---
		// If the delay slot instruction is not NOP (0x0009), execute it.
		if (op != 0x0009)
		{
			ExecuteOpcode(op);
		}
		// --- End NOP Optimization ---
 
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

#ifndef ENABLE_SH4_IR
Sh4Executor *Get_Sh4Interpreter()
{
    return new Sh4Interpreter();
}
#endif

// Then modify the Sh4_int_Run function
void Sh4_int_Run()
{
	sh4_int_bCpuRun = true;

	// Use a larger cycle batch size for better performance
	static const int CYCLE_BATCH_SIZE = 10000;

	while (sh4_int_bCpuRun)
	{
		// Apply CPU frequency scaling to the batch size, but with a more efficient approach
		int scaled_batch_size;

		// Use a lookup table approach for common scaling factors to avoid expensive calculations
		if (sh4_cpu_timescale >= 1.4f)
			scaled_batch_size = 25000;  // Very fast
		else if (sh4_cpu_timescale >= 1.1f)
			scaled_batch_size = 15000;  // Fast
		else if (sh4_cpu_timescale >= 0.9f)
			scaled_batch_size = 10000;  // Normal
		else if (sh4_cpu_timescale >= 0.7f)
			scaled_batch_size = 8000;   // Slow
		else
			scaled_batch_size = 5000;   // Very slow

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

		// Allow other threads to run, but only if we're not running at full speed
		if (sh4_cpu_timescale < 0.9f)
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
