/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "sh4_ops.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"

class Sh4InterpreterTest : public Sh4OpTest {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		ctx = &p_sh4rcb->cntx;
		sh4 = Get_Sh4Interpreter();
		sh4->Init();
	}
	void PrepareOp(u16 op, u16 op2 = 0, u16 op3 = 0) override
	{
		ctx->pc = START_PC;
		addrspace::write16(ctx->pc, op);
		if (op2 != 0)
			addrspace::write16(ctx->pc + 2, op2);
		if (op3 != 0)
			addrspace::write16(ctx->pc + 4, op3);
	}
	void RunOp(int numOp = 1) override
	{
		ctx->pc = START_PC;
		for (int i = 0; i < numOp; i++)
			sh4->Step();
	}
};

TEST_F(Sh4InterpreterTest, MovRmRnTest)
{
	Sh4OpTest::MovRmRnTest();
}
TEST_F(Sh4InterpreterTest, MovImmRnTest)
{
	Sh4OpTest::MovImmRnTest();
}
TEST_F(Sh4InterpreterTest, MovMiscTest)
{
	Sh4OpTest::MovMiscTest();
}
TEST_F(Sh4InterpreterTest, LoadTest)
{
	Sh4OpTest::LoadTest();
}
TEST_F(Sh4InterpreterTest, LoadTest2)
{
	Sh4OpTest::LoadTest2();
}
TEST_F(Sh4InterpreterTest, StoreTest)
{
	Sh4OpTest::StoreTest();
}
TEST_F(Sh4InterpreterTest, StoreTest2)
{
	Sh4OpTest::StoreTest2();
}
TEST_F(Sh4InterpreterTest, ArithmeticTest)
{
	Sh4OpTest::ArithmeticTest();
}
TEST_F(Sh4InterpreterTest, MulDivTest)
{
	Sh4OpTest::MulDivTest();
}
TEST_F(Sh4InterpreterTest, CmpTest)
{
	Sh4OpTest::CmpTest();
}
TEST_F(Sh4InterpreterTest, StatusRegTest)
{
	Sh4OpTest::StatusRegTest();
}
TEST_F(Sh4InterpreterTest, FloatingPointTest)
{
	Sh4OpTest::FloatingPointTest();
}
TEST_F(Sh4InterpreterTest, DoubleFloatingPointTest)
{
	Sh4OpTest::DoubleFloatingPointTest();
}

TEST_F(Sh4InterpreterTest, DirectAddrSpaceP1WriteReadTest)
{
	// Target P1 address (cached RAM)
	const u32 test_addr = 0x8C010000;
	const u32 test_value = 0xDEADBEEF;
	const u32 initial_ram_value = addrspace::read32(test_addr);

	INFO_LOG(SH4, "DirectAddrSpaceP1WriteReadTest: Initial value at 0x%08X is 0x%08X", test_addr, initial_ram_value);

	addrspace::write32(test_addr, test_value);
	INFO_LOG(SH4, "DirectAddrSpaceP1WriteReadTest: Wrote 0x%08X to 0x%08X", test_value, test_addr);

	const u32 value_after_write = addrspace::read32(test_addr);
	INFO_LOG(SH4, "DirectAddrSpaceP1WriteReadTest: Read 0x%08X from 0x%08X after write", value_after_write, test_addr);

	EXPECT_EQ(value_after_write, test_value)
		<< "Value read from 0x" << std::hex << test_addr
		<< " after writing 0x" << test_value
		<< " did not match. Read: 0x" << value_after_write;

	// As a control, write something else and read again
	const u32 control_value = 0x12345678;
	addrspace::write32(test_addr, control_value);
	const u32 value_after_control_write = addrspace::read32(test_addr);
	EXPECT_EQ(value_after_control_write, control_value)
		<< "Control write failed. Value read from 0x" << std::hex << test_addr
		<< " after writing 0x" << control_value
		<< " did not match. Read: 0x" << value_after_control_write;
}

TEST_F(Sh4InterpreterTest, DirectAddrSpaceP0WriteReadTest)
{
	// Target P0 address (cached RAM)
	const u32 test_addr = 0x0C010000; // Same offset as P1 test, but in P0
	const u32 test_value = 0xCAFEBABE;
	const u32 initial_ram_value = addrspace::read32(test_addr);

	INFO_LOG(SH4, "DirectAddrSpaceP0WriteReadTest: Initial value at 0x%08X is 0x%08X", test_addr, initial_ram_value);

	addrspace::write32(test_addr, test_value);
	INFO_LOG(SH4, "DirectAddrSpaceP0WriteReadTest: Wrote 0x%08X to 0x%08X", test_value, test_addr);

	const u32 value_after_write = addrspace::read32(test_addr);
	INFO_LOG(SH4, "DirectAddrSpaceP0WriteReadTest: Read 0x%08X from 0x%08X after write", value_after_write, test_addr);

	EXPECT_EQ(value_after_write, test_value)
		<< "Value read from 0x" << std::hex << test_addr
		<< " after writing 0x" << test_value
		<< " did not match. Read: 0x" << value_after_write;
}
