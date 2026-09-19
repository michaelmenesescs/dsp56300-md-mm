#include "interpreterunittests.h"

#include "agu.h"
#include "dsp.h"
#include "dspBootCode.h"
#include "memory.h"

namespace dsp56k
{
	InterpreterUnitTests::InterpreterUnitTests()
	{
		testOpcodeCacheAllocation();
		testBootOverwriteInvalidation();
		testBootOutOfRangeInvalidation();
		testCCCC();
		testSubr();
		testCycleAccounting();
		testCooperativeDoLoops();
		testLongInterruptPeripherals();
		
		runAllTests();
		dsp.setCooperativeDoLoops(true);
		runAllTests();
		dsp.setCooperativeDoLoops(false);
	}

	void InterpreterUnitTests::testLongInterruptPeripherals()
	{
		if constexpr(g_useJIT)
			return;
		for(const bool secondWord : {false, true})
		{
			dsp.resetHW();
			dsp.setCooperativeDoLoops(true);
			emitToMemory("do #$2,>$104", 0x100);
			emitToMemory("nop", 0x102);
			emitToMemory("nop", 0x103);
			emitToMemory(secondWord ? "nop" : "jsr $200", 0x60);
			emitToMemory(secondWord ? "jsr $200" : "nop", 0x61);
			emitToMemory("nop", 0x200);
			emitToMemory("rti", 0x201);
			dsp.setPC(0x100);
			dsp.execInterpreter();
			const auto sr = dsp.getSR();
			dsp.execInterrupt(0x60);
			verify(dsp.getProcessingMode() == DSP::LongInterrupt);
			verify(dsp.getPC() == 0x200);
			verify(!dsp.sr_test_noCache(SR_LF));
			verify(dsp.reg.sc.var == 3);

			// A due peripheral must run during the handler, even while interrupt
			// dispatch itself is suppressed. Previously its callback was a no-op.
			peripheralsX.resetDelayCycles(0, 0);
			dsp.execInterpreter();
			verify(peripheralsX.getTargetClock() > 0);
			verify(dsp.getPC() == 0x201);
			dsp.execInterpreter();
			verify(dsp.getPC() == 0x102);
			verify(dsp.getSR() == sr);
			verify(dsp.reg.sc.var == 2);
			execUntil(0x104);
			verify(dsp.reg.sc.var == 0);
		}
		dsp.setCooperativeDoLoops(false);
		dsp.resetHW();
	}

	void InterpreterUnitTests::testCooperativeDoLoops()
	{
		// Compare nested loop results and clocks against the whole-loop interpreter.
		const auto setup = [&]
		{
			dsp.resetHW();
			dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0)));
			dsp.setALU(true, TReg56(static_cast<TReg56::MyType>(0x1000000)));
			emitToMemory("do #$3,>$107", 0x100);
			emitToMemory("do #$2,>$106", 0x102);
			emitToMemory("add b,a", 0x104);
			emitToMemory("nop", 0x105);
			emitToMemory("nop", 0x106);
			dsp.setPC(0x100);
		};
		setup();
		dsp.execInterpreter();
		const auto reference = dsp.readRegs();
		const auto cycles = dsp.getCycles();
		const auto instructions = dsp.getInstructionCounter();
		verify(dsp.getPC() == 0x107);
		verify(dsp.aluA().var == 0x6000000);

		setup();
		dsp.setCooperativeDoLoops(true);
		dsp.execInterpreter();
		verify(dsp.getPC() == 0x102);
		verify(dsp.getInstructionCounter() == 1);
		execUntil(0x107);
		verify(dsp.getCycles() == cycles);
		verify(dsp.getInstructionCounter() == instructions);
		verify(dsp.reg.a.var == reference.a.var);
		verify(dsp.reg.la == reference.la && dsp.reg.lc == reference.lc);
		verify(dsp.reg.sp == reference.sp && dsp.reg.sc == reference.sc);
		verify(dsp.getSR() == reference.sr);

		// A boot-loader-shaped loop must yield while HRDF is clear, allowing the
		// single-thread host to supply each word. No cycles or reads are skipped.
		dsp.resetHW();
		peripheralsX.getHDI08().setRXRateLimit(0);
		emitToMemory("do #$2,>$106", 0x100);
		const auto poll = assembler.assemble("brclr #$0,x:<<$ffffc3,>$0");
		verify(poll.success());
		dsp.memWriteP(0x102, poll.word[0]);
		dsp.memWriteP(0x103, poll.word[1]);
		emitToMemory("movep x:<<$ffffc6,x0", 0x104);
		emitToMemory("nop", 0x105);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		for(const TWord word : {0x123456u, 0x654321u})
		{
			const auto before = dsp.getInstructionCounter();
			for(unsigned i = 0; i < 16; ++i)
				dsp.execInterpreter();
			verify(dsp.getPC() == 0x102);
			verify(dsp.getInstructionCounter() == before + 16);
			peripheralsX.getHDI08().writeRX(&word, 1);
			dsp.execInterpreter();
			verify(dsp.getPC() == 0x104);
			dsp.execInterpreter();
			verify(dsp.x0() == word);
			dsp.execInterpreter();
		}
		verify(dsp.getPC() == 0x106);
		verify(!dsp.sr_test_noCache(SR_LF));
		verify(dsp.reg.sc.var == 0);
		dsp.setCooperativeDoLoops(false);
		dsp.resetHW();
	}

	void InterpreterUnitTests::testOpcodeCacheAllocation()
	{
		if constexpr(g_useJIT)
		{
			verify(dsp.m_opcodeCache.empty());

			// Explicit interpreter use in a JIT-capable diagnostic must lazily create
			// the cache before the first dispatch and execute normally.
			dsp.resetHW();
			emitToMemory("nop", 0x100);
			dsp.setPC(0x100);
			dsp.execInterpreter();
			verify(dsp.m_opcodeCache.size() == dsp.memory().sizeP());
			verify(dsp.getPC() == 0x101);
		}
		else
		{
			verify(dsp.m_opcodeCache.size() == dsp.memory().sizeP());
		}
	}

	void InterpreterUnitTests::testBootOverwriteInvalidation()
	{
		constexpr TWord pc = 0x100;

		// Resolve and cache NOP through the interpreter before the boot loader
		// replaces the same P-memory address with a different instruction.
		dsp.resetHW();
		emitToMemory("nop", pc);
		dsp.setPC(pc);
		dsp.execInterpreter();
		verify(dsp.getPC() == pc + 1);

		const auto replacement = assembler.assemble("move #$22,x0");
		verify(replacement.success() && replacement.wordCount == 1);

		DspBoot boot(dsp);
		verify(!boot.hdiWriteTX(replacement.wordCount));
		verify(!boot.hdiWriteTX(pc));
		verify(boot.hdiWriteTX(replacement.word[0]));
		verify(dsp.memRead(MemArea_P, pc) == replacement.word[0]);

		dsp.x0(0);
		dsp.execInterpreter();
		verify(dsp.x0() == 0x220000);
		verify(dsp.getPC() == pc + 1);
		dsp.resetHW();
	}

	void InterpreterUnitTests::testBootOutOfRangeInvalidation()
	{
		constexpr TWord pc = 0x100;
		dsp.resetHW();
		emitToMemory("nop", pc);
		dsp.setPC(pc);
		dsp.execInterpreter();
		const auto cachedOp = dsp.m_opcodeCache[pc].op;

		// The first address outside configured P memory is ignored by Memory.
		// In a forced-interpreter build it must not write one past the cycle
		// cache. Do not execute the invalid PC installed by this synthetic boot.
		for(const TWord address : {dsp.memory().sizeP(), dsp.memory().sizeP() + 1})
		{
			DspBoot boot(dsp);
			verify(!boot.hdiWriteTX(1));
			verify(!boot.hdiWriteTX(address));
			verify(boot.hdiWriteTX(0));
			verify(dsp.m_opcodeCache[pc].op == cachedOp);
		}
		dsp.setPC(pc);
		dsp.execInterpreter();
		verify(dsp.getPC() == pc + 1);
		dsp.resetHW();
	}

	void InterpreterUnitTests::testCycleAccounting()
	{
		if constexpr(g_useJIT)
		{
			// Normal JIT builds must not pay for the interpreter-only per-PC cache.
			verify(dsp.m_opcodeCycleCache.empty());
			return;
		}

		verify(dsp.m_opcodeCycleCache.size() == dsp.memory().sizeP());

		// A cached instruction cost is used on execution and invalidated by P writes.
		dsp.resetHW();
		execOpcode(assembler.assemble("nop").word[0], 0, false, 0x100);
		verify(dsp.getCycles() == 1);
		verify(dsp.m_opcodeCycleCache[0x100] == 1);

		const auto andi = assembler.assemble("andi #$33,mr");
		verify(andi.success());
		dsp.memWriteP(0x100, andi.word[0]);
		if(andi.wordCount > 1)
			dsp.memWriteP(0x101, andi.word[1]);
		verify(dsp.m_opcodeCycleCache[0x100] == 0);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getCycles() == 4);
		verify(dsp.m_opcodeCycleCache[0x100] == 3);

		// REP executes its own instruction plus the repeated body inside one interpreter step.
		dsp.resetHW();
		TWord pc = 0x100;
		pc = emitToMemory("rep #$4", pc);
		emitToMemory("nop", pc);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getCycles() == 9); // REP (5) + four NOPs (1 each)

		// DO likewise runs its loop body internally rather than returning through execOp per pass.
		dsp.resetHW();
		pc = 0x100;
		pc = emitToMemory("do #$5,>$104", pc);
		pc = emitToMemory("nop", pc);
		emitToMemory("nop", pc);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getCycles() == 15); // DO (5) + five two-NOP iterations
	}

	void InterpreterUnitTests::execOpcode(uint32_t _op0, uint32_t _op1, const bool _reset, TWord _pc)
	{
		if(_reset)
			dsp.resetHW();
		dsp.clearOpcodeCache();
		dsp.mem.set(MemArea_P, _pc, _op0);
		dsp.mem.set(MemArea_P, _pc + 1, _op1);
		dsp.setPC(_pc);

		// Execute only the instruction, bypassing interrupt handling which
		// is designed for a running DSP, not single-step unit tests.
		dsp.pcCurrentInstruction = _pc;
		const auto op = dsp.fetchPC();
		dsp.execOp(op);
	}

	void InterpreterUnitTests::testSubr()
	{
		dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00600000000000)));
		dsp.setALU(true , TReg56(static_cast<TReg56::MyType>(0x00020000000000)));

		emit("subr b,a");
		verify(dsp.aluA().var == 0x002e0000000000);
		verify(!dsp.sr_test(CCR_C));
		verify(!dsp.sr_test(CCR_V));
	}

	void InterpreterUnitTests::testCCCC()
	{
		constexpr auto T=true;
		constexpr auto F=false;

		//                            <  <= =  >= >  != 
		testCCCC(0xff000000000000, 0, T, T, F, F, F, T);
		testCCCC(0x00ff0000000000, 0, F, F, F, T, T, T);
		testCCCC(0x00000000000000, 0, F, T, T, T ,F ,F);
	}

	void InterpreterUnitTests::testCCCC(const int64_t _value, const int64_t _compareValue, const bool _lt, bool _le, bool _eq, bool _ge, bool _gt, bool _neq)
	{
		dsp.resetHW();
		dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(_value)));
		dsp.alu_cmp(false, TReg56(_compareValue), false);
		char sr[16]{};
		dsp.sr_debug(sr);
		verify(_lt == (dsp.decode_cccc(CCCC_LessThan) != 0));
		verify(_le == (dsp.decode_cccc(CCCC_LessEqual) != 0));
		verify(_eq == (dsp.decode_cccc(CCCC_Equal) != 0));
		verify(_ge == (dsp.decode_cccc(CCCC_GreaterEqual) != 0));
		verify(_gt == (dsp.decode_cccc(CCCC_GreaterThan) != 0));
		verify(_neq == (dsp.decode_cccc(CCCC_NotEqual) != 0));	
	}

	void InterpreterUnitTests::runTest(const std::function<void()>& _build, const std::function<void()>& _verify)
	{
		_build();
		_verify();
	}

	void InterpreterUnitTests::emit(TWord _opA, TWord _opB, TWord _pc)
	{
		execOpcode(_opA, _opB, false, _pc);
	}

}
