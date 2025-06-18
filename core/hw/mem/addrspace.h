#pragma once
#include "types.h"
#include <cassert>

namespace addrspace
{

//Typedef's
//ReadMem
typedef u8 DYNACALL ReadMem8FP(u32 Address);
typedef u16 DYNACALL ReadMem16FP(u32 Address);
typedef u32 DYNACALL ReadMem32FP(u32 Address);
//WriteMem
typedef void DYNACALL WriteMem8FP(u32 Address,u8 data);
typedef void DYNACALL WriteMem16FP(u32 Address,u16 data);
typedef void DYNACALL WriteMem32FP(u32 Address,u32 data);

//our own handle type
typedef u32 handler;

//Functions

//init/reset/term
void init();
void term();
void initMappings();

//functions to register and map handlers/memory
handler registerHandler(ReadMem8FP *read8, ReadMem16FP *read16, ReadMem32FP *read32, WriteMem8FP *write8, WriteMem16FP *write16, WriteMem32FP *write32);

#define addrspaceRegisterHandlerTemplate(read, write) addrspace::registerHandler \
									(read<u8>, read<u16>, read<u32>,	\
									write<u8>, write<u16>, write<u32>)

#define HANDLER_MAX 0x1F

extern void* memInfo_ptr[0x100];

static inline u32 FindMask(u32 mask)
{
    // Count contiguous low-order 1-bits; this equals log2(block size).
    u32 width = 0;
    while (mask & 1) {
        ++width;
        mask >>= 1;
    }
    return width;
}

void mapHandler(handler Handler, u32 start, u32 end);
void mapBlock(void* base, u32 start, u32 end, u32 mask);
void mirrorMapping(u32 new_region, u32 start, u32 size);

static inline void mapBlockMirror(void *base, u32 start, u32 end, u32 mask)
{
    u32 page_shift = FindMask(mask);
    assert(page_shift <= HANDLER_MAX);

    for (u32 i = start; i <= end; i++)
    {
        memInfo_ptr[i] = (u8*)((uintptr_t)base | page_shift);
    }
}

u8 DYNACALL read8(u32 address);
u16 DYNACALL read16(u32 address);
u32 DYNACALL read32(u32 address);
u64 DYNACALL read64(u32 address);
template<typename T> T DYNACALL readt(u32 addr);


static inline int32_t DYNACALL read8SX32(u32 address) {
	return (int32_t)(int8_t)readt<u8>(address);
}
static inline int32_t DYNACALL read16SX32(u32 address) {
	return (int32_t)(int16_t)readt<u16>(address);
}

void DYNACALL write8(u32 address, u8 data);
void DYNACALL write16(u32 address, u16 data);
void DYNACALL write32(u32 address, u32 data);
void DYNACALL write64(u32 address, u64 data);
template<typename T> void DYNACALL writet(u32 addr, T data);

//should be called at start up to ensure it will succeed
bool reserve();
void release();

//dynarec helpers
void *readConst(u32 addr, bool& ismem, u32 sz);
void *writeConst(u32 addr, bool& ismem, u32 sz);

extern u8* ram_base;

static inline bool virtmemEnabled() {
	return ram_base != nullptr;
}
void bm_reset(); // FIXME rename? move?
bool bm_lockedWrite(u8* address); // FIXME rename?

void protectVram(u32 addr, u32 size);
void unprotectVram(u32 addr, u32 size);
u32 getVramOffset(void *addr);

} // namespace addrspace
