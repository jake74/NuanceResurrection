// Arm64 JIT support for NuanceResurrection.
//
// The Emit_* handlers describe every NUON instruction as a sequence of
// x86-shaped primitives (NativeCodeCache::X86Emit_*). On Arm64 those primitives
// are implemented in nativecodecache_a64.cpp, where each one emits AArch64 code
// that behaves like the x86 instruction it is named after. The handlers do not
// change. See docs/arm64-jit-plan.md.
#ifndef A64_EMIT_H
#define A64_EMIT_H

#ifdef USE_A64_JIT

// The Stage 0 counters and the finer time split (docs/speed-stage0-results.md)
// are built in only with -DNUANCE_PERF_COUNTERS=ON. Even switched off at run
// time they cost about 1% of the frame rate at full speed (the tests on every
// turn and the larger dispatcher), so an ordinary build leaves them out:
// A64_COUNTING is then a constant false and the compiler removes the lot.
#ifndef NUANCE_PERF_COUNTERS
#define NUANCE_PERF_COUNTERS 0
#endif
#define A64_COUNTING (NUANCE_PERF_COUNTERS && a64PerfSampleInterpreted)


// X11/Xlib.h defines "Bool" as a macro which conflicts with asmjit::Type::Bool
#ifdef Bool
#undef Bool
#endif

#include <asmjit/a64.h>
#include "X86EmitTypes.h"

#define MAX_A64_LABELS 64

// Why the last native block could not be emitted (SuperBlock::a64EmitFailure).
#define A64_EMIT_OK          0
#define A64_EMIT_UNSUPPORTED 1 // a primitive the backend does not implement yet: interpret the packet instead
#define A64_EMIT_NO_ROOM     2 // code buffer full: flush the cache and compile again

// Block statistics, for seeing what the backend still lacks: how many blocks
// were emitted natively and, for those that were not, which primitive or
// condition stopped each one. A64_FormatStats writes a one-line summary with
// the most frequent reasons first.
void A64_NoteBlock(unsigned failure, const char *reason);
void A64_FormatStats(char *buffer, size_t bufferSize);

// Runs a few generated blocks through the backend - an empty block, a call into
// a C++ helper, a forward jump - and reports whether they behaved. 'message'
// receives one line saying what happened either way.
bool A64_JitSelfTest(char *message, size_t messageSize);

// The block being executed, for diagnosing a hang inside generated code.
extern volatile unsigned a64DebugBlockPc;
extern volatile unsigned a64DebugBlockMpe;
extern volatile unsigned a64DebugBlockRunning; // 1 while inside the block

// How many packets an MPE may run in one scheduler step by going straight from
// one compiled block to the next (see MPE::FetchDecodeExecute). 0 turns that off.
extern unsigned a64ChainPackets;

// Per-MPE dispatch counts for the NUANCE_PERF log: native blocks entered,
// packets interpreted, and how many of those were marked never to be compiled.
extern unsigned long long a64PerfNativeBlocks[4], a64PerfInterpreted[4], a64PerfNeverCompile[4];
// Why a compilable packet was interpreted: [mpe][0] in a delay slot or handed
// back by a block, [1] newly decoded, [2] marked compiled but no block for
// this overlay, [3] not yet run often enough to compile.
extern unsigned long long a64PerfWhy[4][4];

// Stage 0 counters (docs/speed-stage0-results.md). Every one of them is
// incremented only when a64PerfSampleInterpreted is set (NUANCE_PERF does),
// and none is per packet or per chain step. The per-turn ones are counted
// by one test at the end of a turn (MPE::FetchDecodeExecute), from a "why
// did it end" value that the turn's own paths leave in a local variable with
// a plain assignment. All are per MPE and read by the NUANCE_PERF report in
// libretro.cpp, which derives the rest (see A64PerfReport there).
//
// Why a turn (one call of MPE::FetchDecodeExecute from the round) ended.
// Every call adds to exactly one of these, so they sum to the turns.
enum A64TurnEnd
{
  A64END_NOT_RUNNING,       // the MPE is not running (mpectl GO clear): the call did nothing
  A64END_INTERPRETED,       // one packet was interpreted
  A64END_BIOS,              // a BIOS handler ran as C++
  A64END_PE,                // a Presentation Engine handler ran as C++
  A64END_IL,                // a non-native (IL) block ran: not expected on Arm64
  A64END_BUDGET,            // native turn: the packet budget (a64ChainPackets) was reached
  A64END_NO_CHAIN,          // native turn: chaining is off (limit 0, or MPE3 under the packet hack)
  A64END_BRANCH_PENDING,    // native turn: ecuSkipCounter above 1 (delay slots still to run)
  A64END_HALTED,            // native turn: MPE stopped or halted by an exception
  A64END_ICACHE_INVALIDATE, // native turn: instruction cache invalidation pending
  A64END_BAIL,              // native turn: a block bailed out mid-block (interpretNextPacket)
  A64END_INTERRUPT,         // native turn: an interrupt would be taken
  A64END_BREAKPOINT,        // native turn: the next address is the breakpoint
  A64END_IRAM_DIRTY,        // native turn: the next address is in a local memory region marked for invalidation
  A64END_NO_BLOCK,          // native turn: no native block for the next address
  A64END_COUNT
};
// What the next packet was when a native turn ended for lack of a block.
enum A64NoBlock { A64NOBLOCK_BIOS, A64NOBLOCK_NEVER_DECODED, A64NOBLOCK_NEVER_COMPILE, A64NOBLOCK_STALE_MARK, A64NOBLOCK_BELOW_THRESHOLD, A64NOBLOCK_NOT_NATIVE, A64NOBLOCK_COUNT };
// Why a packet was interpreted (numbering of docs/speed-plan.md section 4.1 in brackets).
enum A64IntReason
{
  A64INT_DELAY_SLOT,        // in the delay slots of an interpreted branch (R6)
  A64INT_BAIL,              // handed back by a block that bailed mid-block (R11)
  A64INT_NEW,               // newly decoded (R7)
  A64INT_STALE_MARK,        // marked compiled but no block under this overlay (R9)
  A64INT_BELOW_THRESHOLD,   // not run often enough yet (R8)
  A64INT_NEVER_BIOS,        // never compiled: BIOS jump table or breakpoint (R2)
  A64INT_NEVER_BRANCH,      // never compiled: a branch packet (R4, inferred from the packet's flags)
  A64INT_NEVER_CONTROL,     // never compiled: control register access with side effects (R1, inferred)
  A64INT_NEVER_OTHER,       // never compiled: anything else (halt and other ECU packets, R5)
  A64INT_COUNT
};
extern unsigned long long a64PerfTurnEnd[4][A64END_COUNT];
extern unsigned long long a64PerfPackets[4];     // packets run (what the round credits each MPE with)
extern unsigned long long a64PerfPolling[4];     // running turns that began at the pcexec the same MPE's previous running turn began at
extern unsigned long long a64PerfNoBlock[4][A64NOBLOCK_COUNT];
extern unsigned long long a64PerfIntReason[4][A64INT_COUNT];
// Turns by packets run: 0, 1, 2-3, 4-7, 8-15, 16-31, 32 and more.
extern unsigned long long a64PerfPacketsPerTurn[4][7];
// Native cache flush events: [0] overlay region, [1] main and system bus ranges
// (instruction cache invalidation), [2] whole cache, buffer past its threshold,
// [3] whole cache, a compile found no room. And FlushRegion calls with the
// number of addresses their loops walk.
extern unsigned long long a64PerfFlushEvents[4][4], a64PerfFlushCalls[4], a64PerfFlushAddresses[4];
extern unsigned long long a64PerfRounds, a64PerfEmptyRounds, a64PerfIdleRounds; // RunEmulation rounds; with no packet run; slept
extern unsigned a64PerfLastTurnPc[4];

// What a64DebugBlockRunning says the emulation thread is doing, for the
// sampler of the NUANCE_PERF report. The first four are the original ones.
// Without NUANCE_PERF only 0, 1 and 6 are ever stored by the dispatcher
// (and 2 and 3 as before); the watchdog reads state 1 as "inside a block".
enum A64State
{
  A64STATE_DISPATCH,    // FetchDecodeExecute proper: start and end of a turn, lookups
  A64STATE_GENERATED,   // inside a native block, prologue and epilogue included
  A64STATE_INTERPRETER, // ExecuteNuances for one packet
  A64STATE_ASLEEP,      // the 50 microsecond sleep of an idle round
  A64STATE_HANDLER,     // a BIOS or Presentation Engine handler, as C++
  A64STATE_COMPILE,     // CompileBlock
  A64STATE_CHAIN,       // the chain loop between two blocks of one turn (the call itself and the block are state 1)
  A64STATE_ROUND,       // RunEmulation outside the MPEs: comm bus, clock, timers, audio, scheduled interrupts
  A64STATE_FLUSH,       // cache flush and invalidation loops
  A64STATE_OVERLAY,     // overlay hashing and lookup
  A64STATE_DECODE,      // decoding a packet seen for the first time
  A64STATE_MEDIA,       // TickMediaCallbacks, the rest of the turn and the round's loop between turns (credits, packet accounting) up to the next turn
  A64STATE_COUNTING,    // NUANCE_PERF's own counting at the end of a turn: what the measurement costs
  A64STATE_COUNT
};
extern unsigned a64CompileSlotBranches; // compile a ",nop" branch in a conditional branch's second delay slot
extern unsigned a64CompileSlotMemory; // compile register-indirect memory access in a delay slot (S1.1)
extern unsigned a64CompileNested; // compile branches in both delay slots, and delayed branches in the slots (S1.3); needs a64CompileSlotBranches
extern unsigned a64IramThreshold;  // runs before a packet in MPE local memory is compiled
extern unsigned a64RecompileMarked; // compile a packet marked COMPILED that has no block


// With a64PerfSampleInterpreted set (NUANCE_PERF does), the dispatcher reports
// every packet it interprets and A64_TopInterpretedPackets lists the addresses
// seen most since the last call: what to teach the block compiler next.
extern bool a64PerfSampleInterpreted;
void A64_NoteInterpretedPacket(unsigned mpeIndex, unsigned address, unsigned packetInfo);
struct A64InterpretedPacket { unsigned mpeIndex, address, packetInfo; unsigned long long count; };
unsigned A64_TopInterpretedPackets(A64InterpretedPacket *out, unsigned maxEntries);

// Differential test against the interpreter (a64_difftest.cpp).
void A64_DiffTest(unsigned trials, unsigned seed);

#endif // USE_A64_JIT
#endif // A64_EMIT_H
