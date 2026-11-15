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

#endif // USE_A64_JIT
#endif // A64_EMIT_H
