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
