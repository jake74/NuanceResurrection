// Arm64 implementations of the NativeCodeCache::X86Emit_* primitives.
//
// The Emit_* handlers spell out every NUON instruction as a sequence of
// x86-shaped operations. This file gives each of those operations an AArch64
// body with the same observable behaviour, so the handlers run unchanged on
// Arm64. nativecodecache.cpp holds the x86 bodies and compiles them out when
// USE_A64_JIT is defined. See docs/arm64-jit-plan.md.
//
// A primitive that has no Arm64 body yet calls A64_Unsupported(). That fails
// the block being compiled, and the packet it starts with goes back to the
// interpreter - so the backend can be filled in one primitive at a time and
// every block that does compile is made only of primitives that exist.
//
// Register mapping:
//   eax ecx edx      -> w9 w10 w11   caller-saved on both sides
//   ebx ebp          -> w19 w20      callee-saved, as the handlers expect of x86-64
//   esi edi          -> x21 x22      register-file base pointers, survive calls
//   scratch          -> x16 x17      addresses and immediates, never live across primitives
//   esp              -> unsupported
#ifdef USE_A64_JIT

#include "basetypes.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mpe.h"
#include "NativeCodeCache.h"

using namespace asmjit;

namespace {

constexpr uint32_t kNoReg = 0xFF;

const a64::Gp kScratch0 = a64::x16;
const a64::Gp kScratch1 = a64::x17;

// Host register number for an x86 register number (eax=0 .. edi=7).
inline uint32_t HostRegId(const uint32_t x86Index)
{
  static const uint8_t ids[8] = { 9, 10, 11, 19, kNoReg /*esp*/, 20, 21, 22 };
  return ids[x86Index & 7];
}

inline bool IsReg32(const x86Reg reg)
{
  return reg >= x86Reg::x86Reg_eax && reg <= x86Reg::x86Reg_edi;
}

// Only the 32-bit general registers for now; the 8- and 16-bit ones need
// bit-field lowering that arrives with the integer primitives.
inline uint32_t HostRegId(const x86Reg reg)
{
  return IsReg32(reg) ? HostRegId((uint32_t)reg - (uint32_t)x86Reg::x86Reg_eax) : kNoReg;
}

inline a64::Gp W(const uint32_t id) { return a64::Gp::make_r32(id); }
inline a64::Gp X(const uint32_t id) { return a64::Gp::make_r64(id); }

// asmjit reports an operand it cannot encode by returning an error and emitting
// nothing. Dropping an instruction silently is the one thing a JIT must not do,
// so every such error fails the block.
class BlockErrorHandler final : public ErrorHandler
{
public:
  void handle_error(Error, const char *message, BaseEmitter *) override
  {
    bFailed = true;
    reason = message;
  }
  bool bFailed = false;
  const char *reason = nullptr;
};

BlockErrorHandler g_errorHandler;

// Any 64-bit constant, in as few instructions as its 16-bit pieces need.
void LoadImm64(a64::Assembler &a, const a64::Gp &x, const uint64_t value)
{
  bool bFirst = true;
  for(uint32_t shift = 0; shift < 64; shift += 16)
  {
    const uint64_t piece = (value >> shift) & 0xFFFFu;
    if(!piece)
      continue;
    if(bFirst)
      a.movz(x, piece, shift);
    else
      a.movk(x, piece, shift);
    bFirst = false;
  }
  if(bFirst)
    a.movz(x, 0);
}

inline void LoadImm32(a64::Assembler &a, const a64::Gp &w, const uint32_t value)
{
  LoadImm64(a, w.x(), value);
}

} // namespace

// --- Block management ---

void NativeCodeCache::A64_BeginBlock()
{
  delete a64As;
  delete a64Code;

  a64Code = new CodeHolder();
  a64Code->init(Environment::host());
  g_errorHandler.bFailed = false;
  g_errorHandler.reason = nullptr;
  a64Code->set_error_handler(&g_errorHandler);
  a64As = new a64::Assembler(a64Code);

  for(int i = 0; i < MAX_A64_LABELS; i++)
  {
    a64Labels[i] = Label();
    a64LabelBound[i] = false;
  }

  a64Failure = A64_EMIT_OK;
  a64FailReason = nullptr;
}

void NativeCodeCache::A64_AbortBlock()
{
  delete a64As;
  a64As = nullptr;
  delete a64Code;
  a64Code = nullptr;
}

uint32 NativeCodeCache::A64_EndBlock()
{
  if(!a64As || !a64Code)
  {
    a64Failure = A64_EMIT_UNSUPPORTED;
    return 0;
  }

  if(g_errorHandler.bFailed && !a64Failure)
  {
    a64Failure = A64_EMIT_UNSUPPORTED;
    a64FailReason = g_errorHandler.reason;
  }

  size_t codeSize = 0;
  if(!a64Failure)
  {
    a64Code->flatten();
    a64Code->resolve_cross_section_fixups();
    codeSize = a64Code->code_size();

    if(g_errorHandler.bFailed || !codeSize)
    {
      a64Failure = A64_EMIT_UNSUPPORTED;
      a64FailReason = g_errorHandler.bFailed ? g_errorHandler.reason : "empty block";
    }
    else if(codeSize > GetAvailableCodeBufferSize())
    {
      a64Failure = A64_EMIT_NO_ROOM;
      a64FailReason = "code buffer full";
    }
  }

  if(!a64Failure)
  {
    a64Code->relocate_to_base((uint64_t)(uintptr_t)pEmitLoc);
    a64Code->copy_flattened_data(pEmitLoc, codeSize);
  }

  A64_AbortBlock();
  return a64Failure ? 0 : (uint32)codeSize;
}

void NativeCodeCache::A64_Unsupported(const char * const what)
{
  if(!a64Failure)
  {
    a64Failure = A64_EMIT_UNSUPPORTED;
    a64FailReason = what;
  }
}

void NativeCodeCache::A64_BindLabel(const uint32 labelIndex)
{
  if(!a64As || labelIndex >= MAX_A64_LABELS)
  {
    A64_Unsupported("label index");
    return;
  }

  if(!a64Labels[labelIndex].is_valid())
    a64Labels[labelIndex] = a64As->new_label();
  a64As->bind(a64Labels[labelIndex]);
  a64LabelBound[labelIndex] = true;
}

Label NativeCodeCache::A64_GetLabel(const uint32 labelIndex)
{
  if(!a64As || labelIndex >= MAX_A64_LABELS)
  {
    A64_Unsupported("label index");
    return a64As ? a64As->new_label() : Label();
  }

  // Handlers reuse the small label indices and assume each is fresh at the top
  // of the handler. A reference to an index that is already bound therefore
  // means a new label, not a backward jump to the old bind site.
  if(a64LabelBound[labelIndex] || !a64Labels[labelIndex].is_valid())
  {
    a64Labels[labelIndex] = a64As->new_label();
    a64LabelBound[labelIndex] = false;
  }
  return a64Labels[labelIndex];
}

// Turns an x86 memory operand into an AArch64 one for an access of 'size'
// bytes, emitting whatever address arithmetic it takes into the scratch
// registers. 'base' is a register number (0-7), X86_NO_BASE, or an absolute
// host address.
a64::Mem NativeCodeCache::A64_Mem(const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp, const uint32 size)
{
  a64::Assembler &a = *a64As;
  const bool bHasIndex = (index != x86IndexReg::x86IndexReg_none);
  const uint32_t indexId = bHasIndex ? HostRegId((uint32_t)index) : kNoReg;
  const uint32_t shift = (uint32_t)scale;

  if(bHasIndex && indexId == kNoReg)
  {
    A64_Unsupported("index register");
    return a64::ptr(kScratch0);
  }

  if(base == X86_NO_BASE || base > 7)
  {
    // [index*scale + disp] or [absolute + index*scale + disp]
    const uint64_t address = (base == X86_NO_BASE) ? (uint64_t)(int64_t)disp : (uint64_t)base + (int64_t)disp;
    LoadImm64(a, kScratch0, address);
    if(bHasIndex)
      a.add(kScratch0, kScratch0, X(indexId), a64::lsl(shift));
    return a64::ptr(kScratch0);
  }

  const uint32_t baseId = HostRegId((uint32_t)base);
  if(baseId == kNoReg)
  {
    A64_Unsupported("base register");
    return a64::ptr(kScratch0);
  }

  a64::Gp cur = X(baseId);
  if(bHasIndex)
  {
    a.add(kScratch0, cur, X(indexId), a64::lsl(shift));
    cur = kScratch0;
  }

  if(disp == 0)
    return a64::ptr(cur);

  // The scaled unsigned 12-bit form covers every register-file offset.
  if(disp > 0 && (disp % (int32)size) == 0 && (disp / (int32)size) < 4096)
    return a64::ptr(cur, disp);

  LoadImm64(a, kScratch1, (uint64_t)(int64_t)disp);
  a.add(kScratch0, cur, kScratch1);
  return a64::ptr(kScratch0);
}

// --- Block entry and exit ---

// PUSHAD/POPAD bracket every block. On Arm64 they are the prologue and epilogue:
// only the callee-saved registers the mapping uses need saving, plus the frame
// and link registers so helper calls can be made from inside the block.
void NativeCodeCache::X86Emit_PUSHAD()
{
  a64::Assembler &a = *a64As;
  a.stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -48));
  a.mov(a64::x29, a64::sp);
  a.stp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
  a.stp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
}

void NativeCodeCache::X86Emit_POPAD()
{
  a64::Assembler &a = *a64As;
  a.ldp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
  a.ldp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
  a.ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 48));
}

void NativeCodeCache::X86Emit_RETN(uint16)
{
  a64As->ret(a64::x30);
}

// Helpers take their two arguments in ecx and edx (fastcall) and return in eax.
// Whole registers are moved, not halves: some helpers take or return pointers.
void NativeCodeCache::X86Emit_CALLI(uintptr_t offset, uint16)
{
  a64::Assembler &a = *a64As;
  a.mov(a64::x0, X(HostRegId(x86Reg::x86Reg_ecx)));
  a.mov(a64::x1, X(HostRegId(x86Reg::x86Reg_edx)));
  LoadImm64(a, kScratch0, (uint64_t)offset);
  a.blr(kScratch0);
  a.mov(X(HostRegId(x86Reg::x86Reg_eax)), a64::x0);
}

void NativeCodeCache::X86Emit_JMPI_Label(const uint32 labelIndex)
{
  a64As->b(A64_GetLabel(labelIndex));
}

// --- Moves ---

void NativeCodeCache::X86Emit_MOVRR(const x86Reg regDest, const x86Reg regSrc)
{
  const uint32_t d = HostRegId(regDest), s = HostRegId(regSrc);
  if(d == kNoReg || s == kNoReg) { A64_Unsupported("MOVRR sub-register"); return; }
  a64As->mov(W(d), W(s));
}

void NativeCodeCache::X86Emit_MOVRR64(const x86Reg regDest, const x86Reg regSrc)
{
  const uint32_t d = HostRegId(regDest), s = HostRegId(regSrc);
  if(d == kNoReg || s == kNoReg) { A64_Unsupported("MOVRR64 register"); return; }
  a64As->mov(X(d), X(s));
}

// Like the x86-64 emitter, this loads the full sign-extended value: handlers
// use it for host pointers as well as for 32-bit constants.
void NativeCodeCache::X86Emit_MOVIR(const intptr_t imm, const x86Reg regDest)
{
  const uint32_t d = HostRegId(regDest);
  if(d == kNoReg) { A64_Unsupported("MOVIR sub-register"); return; }
  if((uintptr_t)imm > (uintptr_t)INT32_MAX)
    LoadImm64(*a64As, X(d), (uint64_t)imm);
  else
    LoadImm32(*a64As, W(d), (uint32_t)imm);
}

void NativeCodeCache::X86Emit_MOVIR_Ptr(const uintptr_t addr, const x86Reg regDest)
{
  const uint32_t d = HostRegId(regDest);
  if(d == kNoReg) { A64_Unsupported("MOVIR_Ptr register"); return; }
  LoadImm64(*a64As, X(d), (uint64_t)addr);
}

void NativeCodeCache::X86Emit_MOVMR(const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  const uint32_t d = HostRegId(regDest);
  if(d == kNoReg) { A64_Unsupported("MOVMR sub-register"); return; }
  const a64::Mem m = A64_Mem(base, index, scale, disp, 4);
  a64As->ldr(W(d), m);
}

void NativeCodeCache::X86Emit_MOVRM(const x86Reg regSrc, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  const uint32_t s = HostRegId(regSrc);
  if(s == kNoReg) { A64_Unsupported("MOVRM sub-register"); return; }
  const a64::Mem m = A64_Mem(base, index, scale, disp, 4);
  a64As->str(W(s), m);
}

void NativeCodeCache::X86Emit_MOVIM(const int32 imm, const x86MemPtr ptrType, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  a64::Assembler &a = *a64As;
  // The address may use the first scratch register, so the value goes in the second.
  const uint32 size = (ptrType == x86MemPtr::x86MemPtr_byte) ? 1 : (ptrType == x86MemPtr::x86MemPtr_word) ? 2 : 4;
  if(ptrType == x86MemPtr::x86MemPtr_qword) { A64_Unsupported("MOVIM qword"); return; }

  // A64_Mem only needs the second scratch register for a displacement it
  // cannot encode; build that address first, then load the value.
  a64::Mem m = A64_Mem(base, index, scale, disp, size);
  const a64::Gp value = kScratch1.w();
  if(imm == 0)
  {
    switch(size)
    {
      case 1: a.strb(a64::wzr, m); break;
      case 2: a.strh(a64::wzr, m); break;
      default: a.str(a64::wzr, m); break;
    }
    return;
  }

  LoadImm32(a, value, (uint32_t)imm);
  switch(size)
  {
    case 1: a.strb(value, m); break;
    case 2: a.strh(value, m); break;
    default: a.str(value, m); break;
  }
}

// --- Not implemented yet ---

void NativeCodeCache::X86Emit_ModRegRM(const x86ModType, const x86ModReg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ModRegRM"); }
void NativeCodeCache::X86Emit_Group1RR(const x86Reg, const x86Reg, const uint8) { A64_Unsupported("Group1RR"); }
void NativeCodeCache::X86Emit_Group1RM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32, const uint8) { A64_Unsupported("Group1RM"); }
void NativeCodeCache::X86Emit_Group1MR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32, const uint8) { A64_Unsupported("Group1MR"); }
void NativeCodeCache::X86Emit_Group1IR(const intptr_t, const x86Reg, const uint8) { A64_Unsupported("Group1IR"); }
void NativeCodeCache::X86Emit_Group1IM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32, const uint8) { A64_Unsupported("Group1IM"); }
void NativeCodeCache::X86Emit_Group2IR(const x86Reg, const uint8, const uint8) { A64_Unsupported("Group2IR"); }
void NativeCodeCache::X86Emit_Group2IM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32, const uint8) { A64_Unsupported("Group2IM"); }
void NativeCodeCache::X86Emit_Group2RR(const x86Reg, const uint8) { A64_Unsupported("Group2RR"); }
void NativeCodeCache::X86Emit_Group2RM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32, const uint8) { A64_Unsupported("Group2RM"); }
void NativeCodeCache::X86Emit_ADDRR(const x86Reg, const x86Reg) { A64_Unsupported("ADDRR"); }
void NativeCodeCache::X86Emit_ADDRR64(const x86Reg, const x86Reg) { A64_Unsupported("ADDRR64"); }
void NativeCodeCache::X86Emit_ADDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADDRM"); }
void NativeCodeCache::X86Emit_ADDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADDMR"); }
void NativeCodeCache::X86Emit_ADDIR(const intptr_t, const x86Reg) { A64_Unsupported("ADDIR"); }
void NativeCodeCache::X86Emit_ADDIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADDIM"); }
void NativeCodeCache::X86Emit_PUSHES() { A64_Unsupported("PUSHES"); }
void NativeCodeCache::X86Emit_POPES() { A64_Unsupported("POPES"); }
void NativeCodeCache::X86Emit_ORRR(const x86Reg, const x86Reg) { A64_Unsupported("ORRR"); }
void NativeCodeCache::X86Emit_ORRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ORRM"); }
void NativeCodeCache::X86Emit_ORMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ORMR"); }
void NativeCodeCache::X86Emit_ORIR(const intptr_t, const x86Reg) { A64_Unsupported("ORIR"); }
void NativeCodeCache::X86Emit_ORIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ORIM"); }
void NativeCodeCache::X86Emit_PUSHCS() { A64_Unsupported("PUSHCS"); }
void NativeCodeCache::X86Emit_ADCRR(const x86Reg, const x86Reg) { A64_Unsupported("ADCRR"); }
void NativeCodeCache::X86Emit_ADCRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADCRM"); }
void NativeCodeCache::X86Emit_ADCMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADCMR"); }
void NativeCodeCache::X86Emit_ADCIR(const intptr_t, const x86Reg) { A64_Unsupported("ADCIR"); }
void NativeCodeCache::X86Emit_ADCIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADCIM"); }
void NativeCodeCache::X86Emit_PUSHSS() { A64_Unsupported("PUSHSS"); }
void NativeCodeCache::X86Emit_POPSS() { A64_Unsupported("POPSS"); }
void NativeCodeCache::X86Emit_SBBRR(const x86Reg, const x86Reg) { A64_Unsupported("SBBRR"); }
void NativeCodeCache::X86Emit_SBBRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SBBRM"); }
void NativeCodeCache::X86Emit_SBBMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SBBMR"); }
void NativeCodeCache::X86Emit_SBBIR(const intptr_t, const x86Reg) { A64_Unsupported("SBBIR"); }
void NativeCodeCache::X86Emit_SBBIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SBBIM"); }
void NativeCodeCache::X86Emit_PUSHDS() { A64_Unsupported("PUSHDS"); }
void NativeCodeCache::X86Emit_POPDS() { A64_Unsupported("POPDS"); }
void NativeCodeCache::X86Emit_ANDRR(const x86Reg, const x86Reg) { A64_Unsupported("ANDRR"); }
void NativeCodeCache::X86Emit_ANDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ANDRM"); }
void NativeCodeCache::X86Emit_ANDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ANDMR"); }
void NativeCodeCache::X86Emit_ANDIR(const intptr_t, const x86Reg) { A64_Unsupported("ANDIR"); }
void NativeCodeCache::X86Emit_ANDIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ANDIM"); }
void NativeCodeCache::X86Emit_ES() { A64_Unsupported("ES"); }
void NativeCodeCache::X86Emit_DAA() { A64_Unsupported("DAA"); }
void NativeCodeCache::X86Emit_SUBRR(const x86Reg, const x86Reg) { A64_Unsupported("SUBRR"); }
void NativeCodeCache::X86Emit_SUBRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SUBRM"); }
void NativeCodeCache::X86Emit_SUBMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SUBMR"); }
void NativeCodeCache::X86Emit_SUBIR(const intptr_t, const x86Reg) { A64_Unsupported("SUBIR"); }
void NativeCodeCache::X86Emit_SUBIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SUBIM"); }
void NativeCodeCache::X86Emit_CS() { A64_Unsupported("CS"); }
void NativeCodeCache::X86Emit_DAS() { A64_Unsupported("DAS"); }
void NativeCodeCache::X86Emit_XORRR(const x86Reg, const x86Reg) { A64_Unsupported("XORRR"); }
void NativeCodeCache::X86Emit_XORRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("XORRM"); }
void NativeCodeCache::X86Emit_XORMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("XORMR"); }
void NativeCodeCache::X86Emit_XORIR(const intptr_t, const x86Reg) { A64_Unsupported("XORIR"); }
void NativeCodeCache::X86Emit_XORIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("XORIM"); }
void NativeCodeCache::X86Emit_SS() { A64_Unsupported("SS"); }
void NativeCodeCache::X86Emit_AAA() { A64_Unsupported("AAA"); }
void NativeCodeCache::X86Emit_CMPRR(const x86Reg, const x86Reg) { A64_Unsupported("CMPRR"); }
void NativeCodeCache::X86Emit_CMPRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMPRM"); }
void NativeCodeCache::X86Emit_CMPMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMPMR"); }
void NativeCodeCache::X86Emit_CMPIR(const intptr_t, const x86Reg) { A64_Unsupported("CMPIR"); }
void NativeCodeCache::X86Emit_CMPIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMPIM"); }
void NativeCodeCache::X86Emit_DS() { A64_Unsupported("DS"); }
void NativeCodeCache::X86Emit_AAS() { A64_Unsupported("AAS"); }
void NativeCodeCache::X86Emit_INCR(const x86Reg) { A64_Unsupported("INCR"); }
void NativeCodeCache::X86Emit_DECR(const x86Reg) { A64_Unsupported("DECR"); }
void NativeCodeCache::X86Emit_PUSHR(const x86Reg) { A64_Unsupported("PUSHR"); }
void NativeCodeCache::X86Emit_POPR(const x86Reg) { A64_Unsupported("POPR"); }
void NativeCodeCache::X86Emit_PUSHAW() { A64_Unsupported("PUSHAW"); }
void NativeCodeCache::X86Emit_POPAW() { A64_Unsupported("POPAW"); }
void NativeCodeCache::X86Emit_FS() { A64_Unsupported("FS"); }
void NativeCodeCache::X86Emit_GS() { A64_Unsupported("GS"); }
void NativeCodeCache::X86Emit_OPSIZE() { A64_Unsupported("OPSIZE"); }
void NativeCodeCache::X86Emit_ADSIZE() { A64_Unsupported("ADSIZE"); }
void NativeCodeCache::X86Emit_PUSHID(const int32) { A64_Unsupported("PUSHID"); }
void NativeCodeCache::X86Emit_PUSHIW(const int16) { A64_Unsupported("PUSHIW"); }
void NativeCodeCache::X86Emit_IMULIRR(const x86Reg, const int32, const x86Reg) { A64_Unsupported("IMULIRR"); }
void NativeCodeCache::X86Emit_IMULIMR(const x86Reg, const int32, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("IMULIMR"); }
void NativeCodeCache::X86Emit_IMULRR(const x86Reg) { A64_Unsupported("IMULRR"); }
void NativeCodeCache::X86Emit_IMULMR(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("IMULMR"); }
void NativeCodeCache::X86Emit_IMULRRR(const x86Reg, const x86Reg) { A64_Unsupported("IMULRRR"); }
void NativeCodeCache::X86Emit_IMULMRR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("IMULMRR"); }
void NativeCodeCache::X86Emit_BSRRR(const x86Reg, const x86Reg) { A64_Unsupported("BSRRR"); }
void NativeCodeCache::X86Emit_PUSHIB(const int8) { A64_Unsupported("PUSHIB"); }
void NativeCodeCache::X86Emit_INSB() { A64_Unsupported("INSB"); }
void NativeCodeCache::X86Emit_INSW() { A64_Unsupported("INSW"); }
void NativeCodeCache::X86Emit_INSD() { A64_Unsupported("INSD"); }
void NativeCodeCache::X86Emit_OUTSB() { A64_Unsupported("OUTSB"); }
void NativeCodeCache::X86Emit_OUTSW() { A64_Unsupported("OUTSW"); }
void NativeCodeCache::X86Emit_OUTSD() { A64_Unsupported("OUTSD"); }
void NativeCodeCache::X86Emit_JCC(uint8 *, const int8) { A64_Unsupported("JCC"); }
void NativeCodeCache::X86Emit_JCC_Label(const int8, const uint32) { A64_Unsupported("JCC_Label"); }
void NativeCodeCache::X86Emit_JO(uint8 *) { A64_Unsupported("JO"); }
void NativeCodeCache::X86Emit_JNO(uint8 *) { A64_Unsupported("JNO"); }
void NativeCodeCache::X86Emit_JB(uint8 *) { A64_Unsupported("JB"); }
void NativeCodeCache::X86Emit_JNB(uint8 *) { A64_Unsupported("JNB"); }
void NativeCodeCache::X86Emit_JZ(uint8 *) { A64_Unsupported("JZ"); }
void NativeCodeCache::X86Emit_JNZ(uint8 *) { A64_Unsupported("JNZ"); }
void NativeCodeCache::X86Emit_JBE(uint8 *) { A64_Unsupported("JBE"); }
void NativeCodeCache::X86Emit_JNBE(uint8 *) { A64_Unsupported("JNBE"); }
void NativeCodeCache::X86Emit_JS(uint8 *) { A64_Unsupported("JS"); }
void NativeCodeCache::X86Emit_JNS(uint8 *) { A64_Unsupported("JNS"); }
void NativeCodeCache::X86Emit_JP(uint8 *) { A64_Unsupported("JP"); }
void NativeCodeCache::X86Emit_JNP(uint8 *) { A64_Unsupported("JNP"); }
void NativeCodeCache::X86Emit_JL(uint8 *) { A64_Unsupported("JL"); }
void NativeCodeCache::X86Emit_JNL(uint8 *) { A64_Unsupported("JNL"); }
void NativeCodeCache::X86Emit_JLE(uint8 *) { A64_Unsupported("JLE"); }
void NativeCodeCache::X86Emit_JNLE(uint8 *) { A64_Unsupported("JNLE"); }
void NativeCodeCache::X86Emit_TESTRR(const x86Reg, const x86Reg) { A64_Unsupported("TESTRR"); }
void NativeCodeCache::X86Emit_TESTRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("TESTRM"); }
void NativeCodeCache::X86Emit_TESTIR(const uint32, const x86Reg) { A64_Unsupported("TESTIR"); }
void NativeCodeCache::X86Emit_TESTIM(const uint32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("TESTIM"); }
void NativeCodeCache::X86Emit_XCHGRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("XCHGRM"); }
void NativeCodeCache::X86Emit_LEA(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("LEA"); }
void NativeCodeCache::X86Emit_POPM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("POPM"); }
void NativeCodeCache::X86Emit_NOP() { A64_Unsupported("NOP"); }
void NativeCodeCache::X86Emit_XCHGRR(const x86Reg, const x86Reg) { A64_Unsupported("XCHGRR"); }
void NativeCodeCache::X86Emit_CBW() { A64_Unsupported("CBW"); }
void NativeCodeCache::X86Emit_CWDE() { A64_Unsupported("CWDE"); }
void NativeCodeCache::X86Emit_CWD() { A64_Unsupported("CWD"); }
void NativeCodeCache::X86Emit_CDQ() { A64_Unsupported("CDQ"); }
void NativeCodeCache::X86Emit_JMPI(uint8 *, uint16) { A64_Unsupported("JMPI"); }
void NativeCodeCache::X86Emit_WAIT() { A64_Unsupported("WAIT"); }
void NativeCodeCache::X86Emit_PUSHFW() { A64_Unsupported("PUSHFW"); }
void NativeCodeCache::X86Emit_PUSHFD() { A64_Unsupported("PUSHFD"); }
void NativeCodeCache::X86Emit_POPFW() { A64_Unsupported("POPFW"); }
void NativeCodeCache::X86Emit_POPFD() { A64_Unsupported("POPFD"); }
void NativeCodeCache::X86Emit_SAHF() { A64_Unsupported("SAHF"); }
void NativeCodeCache::X86Emit_LAHF() { A64_Unsupported("LAHF"); }
void NativeCodeCache::X86Emit_MOVSB() { A64_Unsupported("MOVSB"); }
void NativeCodeCache::X86Emit_MOVSW() { A64_Unsupported("MOVSW"); }
void NativeCodeCache::X86Emit_MOVSD() { A64_Unsupported("MOVSD"); }
void NativeCodeCache::X86Emit_CMPSB() { A64_Unsupported("CMPSB"); }
void NativeCodeCache::X86Emit_CMPSW() { A64_Unsupported("CMPSW"); }
void NativeCodeCache::X86Emit_CMPSD() { A64_Unsupported("CMPSD"); }
void NativeCodeCache::X86Emit_STOSB() { A64_Unsupported("STOSB"); }
void NativeCodeCache::X86Emit_STOSW() { A64_Unsupported("STOSW"); }
void NativeCodeCache::X86Emit_STOSD() { A64_Unsupported("STOSD"); }
void NativeCodeCache::X86Emit_LODSB() { A64_Unsupported("LODSB"); }
void NativeCodeCache::X86Emit_LODSW() { A64_Unsupported("LODSW"); }
void NativeCodeCache::X86Emit_LODSD() { A64_Unsupported("LODSD"); }
void NativeCodeCache::X86Emit_SCASB() { A64_Unsupported("SCASB"); }
void NativeCodeCache::X86Emit_SCASW() { A64_Unsupported("SCASW"); }
void NativeCodeCache::X86Emit_SCASD() { A64_Unsupported("SCASD"); }
void NativeCodeCache::X86Emit_ROLIR(const x86Reg, const uint8) { A64_Unsupported("ROLIR"); }
void NativeCodeCache::X86Emit_RORIR(const x86Reg, const uint8) { A64_Unsupported("RORIR"); }
void NativeCodeCache::X86Emit_RCLIR(const x86Reg, const uint8) { A64_Unsupported("RCLIR"); }
void NativeCodeCache::X86Emit_RCRIR(const x86Reg, const uint8) { A64_Unsupported("RCRIR"); }
void NativeCodeCache::X86Emit_SHLIR(const x86Reg, const uint8) { A64_Unsupported("SHLIR"); }
void NativeCodeCache::X86Emit_SHLDIRR(const x86Reg, const x86Reg, const uint8) { A64_Unsupported("SHLDIRR"); }
void NativeCodeCache::X86Emit_SHLDIMR(const x86Reg, const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHLDIMR"); }
void NativeCodeCache::X86Emit_SHLDRRR(const x86Reg, const x86Reg) { A64_Unsupported("SHLDRRR"); }
void NativeCodeCache::X86Emit_SHRDIRR(const x86Reg, const x86Reg, const uint8) { A64_Unsupported("SHRDIRR"); }
void NativeCodeCache::X86Emit_SHRDIMR(const x86Reg, const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHRDIMR"); }
void NativeCodeCache::X86Emit_SHRDRRR(const x86Reg, const x86Reg) { A64_Unsupported("SHRDRRR"); }
void NativeCodeCache::X86Emit_SHLDRMR(const x86Reg, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHLDRMR"); }
void NativeCodeCache::X86Emit_SHRDRMR(const x86Reg, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHRDRMR"); }
void NativeCodeCache::X86Emit_SHRIR(const x86Reg, const uint8) { A64_Unsupported("SHRIR"); }
void NativeCodeCache::X86Emit_SALIR(const x86Reg, const uint8) { A64_Unsupported("SALIR"); }
void NativeCodeCache::X86Emit_SARIR(const x86Reg, const uint8) { A64_Unsupported("SARIR"); }
void NativeCodeCache::X86Emit_ROLIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ROLIM"); }
void NativeCodeCache::X86Emit_RORIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("RORIM"); }
void NativeCodeCache::X86Emit_RCLIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("RCLIM"); }
void NativeCodeCache::X86Emit_RCRIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("RCRIM"); }
void NativeCodeCache::X86Emit_SHLIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHLIM"); }
void NativeCodeCache::X86Emit_SHRIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHRIM"); }
void NativeCodeCache::X86Emit_SALIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SALIM"); }
void NativeCodeCache::X86Emit_SARIM(const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SARIM"); }
void NativeCodeCache::X86Emit_ROLRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ROLRM"); }
void NativeCodeCache::X86Emit_RORRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("RORRM"); }
void NativeCodeCache::X86Emit_RCLRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("RCLRM"); }
void NativeCodeCache::X86Emit_RCRRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("RCRRM"); }
void NativeCodeCache::X86Emit_SHLRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHLRM"); }
void NativeCodeCache::X86Emit_SHRRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHRRM"); }
void NativeCodeCache::X86Emit_SALRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SALRM"); }
void NativeCodeCache::X86Emit_SARRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SARRM"); }
void NativeCodeCache::X86Emit_LEAVE() { A64_Unsupported("LEAVE"); }
void NativeCodeCache::X86Emit_RETF(uint16) { A64_Unsupported("RETF"); }
void NativeCodeCache::X86Emit_INT3() { A64_Unsupported("INT3"); }
void NativeCodeCache::X86Emit_INT(int8) { A64_Unsupported("INT"); }
void NativeCodeCache::X86Emit_INTO() { A64_Unsupported("INTO"); }
void NativeCodeCache::X86Emit_IRET() { A64_Unsupported("IRET"); }
void NativeCodeCache::X86Emit_ROLRR(const x86Reg) { A64_Unsupported("ROLRR"); }
void NativeCodeCache::X86Emit_RORRR(const x86Reg) { A64_Unsupported("RORRR"); }
void NativeCodeCache::X86Emit_RCLRR(const x86Reg) { A64_Unsupported("RCLRR"); }
void NativeCodeCache::X86Emit_RCRRR(const x86Reg) { A64_Unsupported("RCRRR"); }
void NativeCodeCache::X86Emit_SHLRR(const x86Reg) { A64_Unsupported("SHLRR"); }
void NativeCodeCache::X86Emit_SHRRR(const x86Reg) { A64_Unsupported("SHRRR"); }
void NativeCodeCache::X86Emit_SALRR(const x86Reg) { A64_Unsupported("SALRR"); }
void NativeCodeCache::X86Emit_SARRR(const x86Reg) { A64_Unsupported("SARRR"); }
void NativeCodeCache::X86Emit_AAM(uint8) { A64_Unsupported("AAM"); }
void NativeCodeCache::X86Emit_AAD(uint8) { A64_Unsupported("AAD"); }
void NativeCodeCache::X86Emit_XLAT() { A64_Unsupported("XLAT"); }
void NativeCodeCache::X86Emit_ESC0() { A64_Unsupported("ESC0"); }
void NativeCodeCache::X86Emit_ESC1() { A64_Unsupported("ESC1"); }
void NativeCodeCache::X86Emit_ESC2() { A64_Unsupported("ESC2"); }
void NativeCodeCache::X86Emit_ESC3() { A64_Unsupported("ESC3"); }
void NativeCodeCache::X86Emit_ESC4() { A64_Unsupported("ESC4"); }
void NativeCodeCache::X86Emit_ESC5() { A64_Unsupported("ESC5"); }
void NativeCodeCache::X86Emit_ESC6() { A64_Unsupported("ESC6"); }
void NativeCodeCache::X86Emit_ESC7() { A64_Unsupported("ESC7"); }
void NativeCodeCache::X86Emit_LOOPNE(uint8 *) { A64_Unsupported("LOOPNE"); }
void NativeCodeCache::X86Emit_LOOPNE_Label(const uint32) { A64_Unsupported("LOOPNE_Label"); }
void NativeCodeCache::X86Emit_LOOPE(uint8 *) { A64_Unsupported("LOOPE"); }
void NativeCodeCache::X86Emit_LOOPE_Label(const uint32) { A64_Unsupported("LOOPE_Label"); }
void NativeCodeCache::X86Emit_LOOP(uint8 *) { A64_Unsupported("LOOP"); }
void NativeCodeCache::X86Emit_LOOP_Label(const uint32) { A64_Unsupported("LOOP_Label"); }
void NativeCodeCache::X86Emit_JCXZ(uint8 *) { A64_Unsupported("JCXZ"); }
void NativeCodeCache::X86Emit_JCXZ_Label(const uint32) { A64_Unsupported("JCXZ_Label"); }
void NativeCodeCache::X86Emit_JECXZ(uint8 *) { A64_Unsupported("JECXZ"); }
void NativeCodeCache::X86Emit_JECXZ_Label(const uint32) { A64_Unsupported("JECXZ_Label"); }
void NativeCodeCache::X86Emit_INI(const x86Reg, uint8) { A64_Unsupported("INI"); }
void NativeCodeCache::X86Emit_OUTI(const x86Reg, uint8) { A64_Unsupported("OUTI"); }
void NativeCodeCache::X86Emit_INR(const x86Reg) { A64_Unsupported("INR"); }
void NativeCodeCache::X86Emit_OUTR(const x86Reg) { A64_Unsupported("OUTR"); }
void NativeCodeCache::X86Emit_LOCK() { A64_Unsupported("LOCK"); }
void NativeCodeCache::X86Emit_INT1() { A64_Unsupported("INT1"); }
void NativeCodeCache::X86Emit_REPNE() { A64_Unsupported("REPNE"); }
void NativeCodeCache::X86Emit_REPE() { A64_Unsupported("REPE"); }
void NativeCodeCache::X86Emit_HLT() { A64_Unsupported("HLT"); }
void NativeCodeCache::X86Emit_CMC() { A64_Unsupported("CMC"); }
void NativeCodeCache::X86Emit_NOTR(const x86Reg) { A64_Unsupported("NOTR"); }
void NativeCodeCache::X86Emit_NOTM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("NOTM"); }
void NativeCodeCache::X86Emit_NEGR(const x86Reg) { A64_Unsupported("NEGR"); }
void NativeCodeCache::X86Emit_NEGM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("NEGM"); }
void NativeCodeCache::X86Emit_CLC() { A64_Unsupported("CLC"); }
void NativeCodeCache::X86Emit_STC() { A64_Unsupported("STC"); }
void NativeCodeCache::X86Emit_CLI() { A64_Unsupported("CLI"); }
void NativeCodeCache::X86Emit_STI() { A64_Unsupported("STI"); }
void NativeCodeCache::X86Emit_CLD() { A64_Unsupported("CLD"); }
void NativeCodeCache::X86Emit_STD() { A64_Unsupported("STD"); }
void NativeCodeCache::X86Emit_INCM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("INCM"); }
void NativeCodeCache::X86Emit_DECM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("DECM"); }
void NativeCodeCache::X86Emit_CALLNM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CALLNM"); }
void NativeCodeCache::X86Emit_CALLR(const x86Reg) { A64_Unsupported("CALLR"); }
void NativeCodeCache::X86Emit_CALLFM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CALLFM"); }
void NativeCodeCache::X86Emit_JMPR(const x86Reg) { A64_Unsupported("JMPR"); }
void NativeCodeCache::X86Emit_JMPNM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("JMPNM"); }
void NativeCodeCache::X86Emit_JMPFM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("JMPFM"); }
void NativeCodeCache::X86Emit_PUSHM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PUSHM"); }
void NativeCodeCache::X86Emit_INVD(const x86Reg) { A64_Unsupported("INVD"); }
void NativeCodeCache::X86Emit_WBINVD(const x86Reg) { A64_Unsupported("WBINVD"); }
void NativeCodeCache::X86Emit_UD1(const x86Reg) { A64_Unsupported("UD1"); }
void NativeCodeCache::X86Emit_BSWAP(const x86Reg) { A64_Unsupported("BSWAP"); }
void NativeCodeCache::X86Emit_SETOR(const x86Reg) { A64_Unsupported("SETOR"); }
void NativeCodeCache::X86Emit_SETOM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETOM"); }
void NativeCodeCache::X86Emit_SETNOR(const x86Reg) { A64_Unsupported("SETNOR"); }
void NativeCodeCache::X86Emit_SETNOM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNOM"); }
void NativeCodeCache::X86Emit_SETBR(const x86Reg) { A64_Unsupported("SETBR"); }
void NativeCodeCache::X86Emit_SETBM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETBM"); }
void NativeCodeCache::X86Emit_SETNBR(const x86Reg) { A64_Unsupported("SETNBR"); }
void NativeCodeCache::X86Emit_SETNBM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNBM"); }
void NativeCodeCache::X86Emit_SETZR(const x86Reg) { A64_Unsupported("SETZR"); }
void NativeCodeCache::X86Emit_SETZM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETZM"); }
void NativeCodeCache::X86Emit_SETNZR(const x86Reg) { A64_Unsupported("SETNZR"); }
void NativeCodeCache::X86Emit_SETNZM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNZM"); }
void NativeCodeCache::X86Emit_SETBER(const x86Reg) { A64_Unsupported("SETBER"); }
void NativeCodeCache::X86Emit_SETBEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETBEM"); }
void NativeCodeCache::X86Emit_SETNBER(const x86Reg) { A64_Unsupported("SETNBER"); }
void NativeCodeCache::X86Emit_SETNBEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNBEM"); }
void NativeCodeCache::X86Emit_SETSR(const x86Reg) { A64_Unsupported("SETSR"); }
void NativeCodeCache::X86Emit_SETSM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETSM"); }
void NativeCodeCache::X86Emit_SETNSR(const x86Reg) { A64_Unsupported("SETNSR"); }
void NativeCodeCache::X86Emit_SETNSM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNSM"); }
void NativeCodeCache::X86Emit_SETPR(const x86Reg) { A64_Unsupported("SETPR"); }
void NativeCodeCache::X86Emit_SETPM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETPM"); }
void NativeCodeCache::X86Emit_SETNPR(const x86Reg) { A64_Unsupported("SETNPR"); }
void NativeCodeCache::X86Emit_SETNPM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNPM"); }
void NativeCodeCache::X86Emit_SETLR(const x86Reg) { A64_Unsupported("SETLR"); }
void NativeCodeCache::X86Emit_SETLM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETLM"); }
void NativeCodeCache::X86Emit_SETNLR(const x86Reg) { A64_Unsupported("SETNLR"); }
void NativeCodeCache::X86Emit_SETNLM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNLM"); }
void NativeCodeCache::X86Emit_SETLER(const x86Reg) { A64_Unsupported("SETLER"); }
void NativeCodeCache::X86Emit_SETLEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETLEM"); }
void NativeCodeCache::X86Emit_SETNLER(const x86Reg) { A64_Unsupported("SETNLER"); }
void NativeCodeCache::X86Emit_SETNLEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNLEM"); }
void NativeCodeCache::X86Emit_CMOVORR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVORR"); }
void NativeCodeCache::X86Emit_CMOVOMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVOMR"); }
void NativeCodeCache::X86Emit_CMOVNORR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNORR"); }
void NativeCodeCache::X86Emit_CMOVNOMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNOMR"); }
void NativeCodeCache::X86Emit_CMOVBRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVBRR"); }
void NativeCodeCache::X86Emit_CMOVBMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVBMR"); }
void NativeCodeCache::X86Emit_CMOVNBRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNBRR"); }
void NativeCodeCache::X86Emit_CMOVNBRR64(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNBRR64"); }
void NativeCodeCache::X86Emit_CMOVNBMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNBMR"); }
void NativeCodeCache::X86Emit_CMOVZRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVZRR"); }
void NativeCodeCache::X86Emit_CMOVZMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVZMR"); }
void NativeCodeCache::X86Emit_CMOVNZRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNZRR"); }
void NativeCodeCache::X86Emit_CMOVNZMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNZMR"); }
void NativeCodeCache::X86Emit_CMOVBERR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVBERR"); }
void NativeCodeCache::X86Emit_CMOVBEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVBEMR"); }
void NativeCodeCache::X86Emit_CMOVNBERR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNBERR"); }
void NativeCodeCache::X86Emit_CMOVNBEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNBEMR"); }
void NativeCodeCache::X86Emit_CMOVSRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVSRR"); }
void NativeCodeCache::X86Emit_CMOVSMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVSMR"); }
void NativeCodeCache::X86Emit_CMOVNSRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNSRR"); }
void NativeCodeCache::X86Emit_CMOVNSMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNSMR"); }
void NativeCodeCache::X86Emit_CMOVPRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVPRR"); }
void NativeCodeCache::X86Emit_CMOVPMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVPMR"); }
void NativeCodeCache::X86Emit_CMOVNPRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNPRR"); }
void NativeCodeCache::X86Emit_CMOVNPMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNPMR"); }
void NativeCodeCache::X86Emit_CMOVLRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVLRR"); }
void NativeCodeCache::X86Emit_CMOVLMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVLMR"); }
void NativeCodeCache::X86Emit_CMOVNLRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNLRR"); }
void NativeCodeCache::X86Emit_CMOVNLMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNLMR"); }
void NativeCodeCache::X86Emit_CMOVLERR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVLERR"); }
void NativeCodeCache::X86Emit_CMOVLEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVLEMR"); }
void NativeCodeCache::X86Emit_CMOVNLERR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNLERR"); }
void NativeCodeCache::X86Emit_CMOVNLEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNLEMR"); }
void NativeCodeCache::X86Emit_MOVSXRR(const x86Reg, const x86Reg) { A64_Unsupported("MOVSXRR"); }
void NativeCodeCache::X86Emit_MOVZXRR(const x86Reg, const x86Reg) { A64_Unsupported("MOVZXRR"); }
void NativeCodeCache::X86Emit_MOVSXMR(const x86Reg, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVSXMR"); }
void NativeCodeCache::X86Emit_MOVZXMR(const x86Reg, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVZXMR"); }
void NativeCodeCache::X86Emit_PSRADRR(const x86Reg, const x86Reg) { A64_Unsupported("PSRADRR"); }
void NativeCodeCache::X86Emit_PSRADIR(const x86Reg, const uint8) { A64_Unsupported("PSRADIR"); }
void NativeCodeCache::X86Emit_PSRLDRR(const x86Reg, const x86Reg) { A64_Unsupported("PSRLDRR"); }
void NativeCodeCache::X86Emit_PSRLDIR(const x86Reg, const uint8) { A64_Unsupported("PSRLDIR"); }
void NativeCodeCache::X86Emit_PSLDRR(const x86Reg, const x86Reg) { A64_Unsupported("PSLDRR"); }
void NativeCodeCache::X86Emit_PSLDIR(const x86Reg, const uint8) { A64_Unsupported("PSLDIR"); }
void NativeCodeCache::X86Emit_PMULLDRR(const x86Reg, const x86Reg) { A64_Unsupported("PMULLDRR"); }
void NativeCodeCache::X86Emit_PHADDDRR(const x86Reg, const x86Reg) { A64_Unsupported("PHADDDRR"); }
void NativeCodeCache::X86Emit_PSHUFBRR(const x86Reg, const x86Reg) { A64_Unsupported("PSHUFBRR"); }
void NativeCodeCache::X86Emit_DPPSRR(const x86Reg, const x86Reg, const uint8) { A64_Unsupported("DPPSRR"); }
void NativeCodeCache::X86Emit_MOVSSMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVSSMR"); }
void NativeCodeCache::X86Emit_UNPCKLRR(const x86Reg, const x86Reg) { A64_Unsupported("UNPCKLRR"); }
void NativeCodeCache::X86Emit_MOVLHRR(const x86Reg, const x86Reg) { A64_Unsupported("MOVLHRR"); }
void NativeCodeCache::X86Emit_SHUFIR(const x86Reg, const x86Reg, const uint8) { A64_Unsupported("SHUFIR"); }
void NativeCodeCache::X86Emit_MOVQRR(const x86Reg, const x86Reg) { A64_Unsupported("MOVQRR"); }
void NativeCodeCache::X86Emit_MOVQRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVQRM"); }
void NativeCodeCache::X86Emit_MOVDQURM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVDQURM"); }
void NativeCodeCache::X86Emit_MOVDQARM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVDQARM"); }
void NativeCodeCache::X86Emit_MOVQMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVQMR"); }
void NativeCodeCache::X86Emit_MOVDQUMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVDQUMR"); }
void NativeCodeCache::X86Emit_MOVDQAMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVDQAMR"); }
void NativeCodeCache::X86Emit_PANDRR(const x86Reg, const x86Reg) { A64_Unsupported("PANDRR"); }
void NativeCodeCache::X86Emit_PANDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDMR"); }
void NativeCodeCache::X86Emit_PANDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDRM"); }
void NativeCodeCache::X86Emit_PANDBRR(const x86Reg, const x86Reg) { A64_Unsupported("PANDBRR"); }
void NativeCodeCache::X86Emit_PANDNMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDNMR"); }
void NativeCodeCache::X86Emit_PANDNRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDNRM"); }
void NativeCodeCache::X86Emit_PSUBDRR(const x86Reg, const x86Reg) { A64_Unsupported("PSUBDRR"); }
void NativeCodeCache::X86Emit_PSUBDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PSUBDMR"); }
void NativeCodeCache::X86Emit_PSUBDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PSUBDRM"); }
void NativeCodeCache::X86Emit_PADDRR(const x86Reg, const x86Reg) { A64_Unsupported("PADDRR"); }
void NativeCodeCache::X86Emit_PADDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PADDMR"); }
void NativeCodeCache::X86Emit_PADDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PADDRM"); }
void NativeCodeCache::X86Emit_EMMS() { A64_Unsupported("EMMS"); }
void NativeCodeCache::X86Emit_FEMMS() { A64_Unsupported("FEMMS"); }
void NativeCodeCache::X86Emit_MOVDRR(const x86Reg, const x86Reg) { A64_Unsupported("MOVDRR"); }
void NativeCodeCache::X86Emit_MOVDRR2(const x86Reg, const x86Reg) { A64_Unsupported("MOVDRR2"); }
void NativeCodeCache::X86Emit_MOVDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVDRM"); }

// --- Statistics ---

namespace {

struct ReasonCount
{
  const char *reason;
  uint64 count;
};

constexpr int kMaxReasons = 128;
ReasonCount g_reasons[kMaxReasons];
int g_numReasons = 0;
uint64 g_blocksNative = 0;
uint64 g_blocksNotNative = 0;

} // namespace

void A64_NoteBlock(const unsigned failure, const char *reason)
{
  if(!failure)
  {
    g_blocksNative++;
    return;
  }

  g_blocksNotNative++;
  if(!reason)
    reason = "unknown";

  for(int i = 0; i < g_numReasons; i++)
  {
    if(!strcmp(g_reasons[i].reason, reason))
    {
      g_reasons[i].count++;
      return;
    }
  }

  if(g_numReasons < kMaxReasons)
  {
    // Reasons are string literals, or messages owned by asmjit for the life of
    // the process, so the pointer can be kept.
    g_reasons[g_numReasons].reason = reason;
    g_reasons[g_numReasons].count = 1;
    g_numReasons++;
  }
}

void A64_FormatStats(char * const buffer, const size_t bufferSize)
{
  int used = snprintf(buffer, bufferSize, "blocks: %llu native, %llu left to the interpreter",
    (unsigned long long)g_blocksNative, (unsigned long long)g_blocksNotNative);

  bool taken[kMaxReasons] = {};
  for(int n = 0; n < 12 && n < g_numReasons && used > 0 && (size_t)used < bufferSize; n++)
  {
    int best = -1;
    for(int i = 0; i < g_numReasons; i++)
    {
      if(!taken[i] && (best < 0 || g_reasons[i].count > g_reasons[best].count))
        best = i;
    }
    taken[best] = true;
    used += snprintf(buffer + used, bufferSize - used, "%s%s (%llu)", n ? ", " : "; stopped by: ",
      g_reasons[best].reason, (unsigned long long)g_reasons[best].count);
  }
}

// --- Self-test ---

namespace {

uint32 SelfTestHelper(const uint32 a, const uint32 b)
{
  return a + b;
}

// Ends the block under construction, makes it executable and runs it.
bool RunSelfTestBlock(NativeCodeCache &cache)
{
  uint8 * const entry = cache.GetEmitPointer();
  uint32 size;
  {
    const NativeCodeCache::WriteScope writeScope(cache);
    size = cache.A64_EndBlock();
  }
  if(!size)
    return false;
  FlushInstructionCache(GetCurrentProcess(), entry, size);
  ((void (*)())entry)();
  return true;
}

} // namespace

bool A64_JitSelfTest(char * const message, const size_t messageSize)
{
  NativeCodeCache cache(64U*1024U);
  if(!cache.GetBuffer() || !cache.IsExecutable())
  {
    snprintf(message, messageSize, "no executable memory available");
    return false;
  }

  static volatile uint32 cells[4];
  static volatile uint32 out;
  const uintptr_t cellsAddr = (uintptr_t)&cells[0];
  const uintptr_t outAddr = (uintptr_t)&out;

  // 1: what every block is made of - prologue, the two base pointers, a store
  // of the exit address to an absolute location, epilogue.
  cells[0] = cells[1] = cells[2] = cells[3] = 0;
  out = 0;
  cache.A64_BeginBlock();
  cache.X86Emit_PUSHAD();
  cache.X86Emit_MOVIR_Ptr(cellsAddr, x86Reg::x86Reg_esi);
  cache.X86Emit_MOVIR_Ptr(cellsAddr, x86Reg::x86Reg_edi);
  cache.X86Emit_MOVIM((int32)0x80001234, x86MemPtr::x86MemPtr_dword, outAddr);
  cache.X86Emit_POPAD();
  cache.X86Emit_RETN();
  if(!RunSelfTestBlock(cache) || out != 0x80001234U)
  {
    snprintf(message, messageSize, "empty block failed (%s, wrote %08X)", cache.A64_FailReason() ? cache.A64_FailReason() : "ran", (unsigned)out);
    return false;
  }

  // 2: a call into C++ with two arguments, with the base pointer and a
  // callee-saved register expected to survive it.
  cells[0] = cells[1] = cells[2] = cells[3] = 0;
  out = 0;
  cache.A64_BeginBlock();
  cache.X86Emit_PUSHAD();
  cache.X86Emit_MOVIR_Ptr(cellsAddr, x86Reg::x86Reg_esi);
  cache.X86Emit_MOVIR(1000, x86Reg::x86Reg_ebx);
  cache.X86Emit_MOVIR(7, x86Reg::x86Reg_ecx);
  cache.X86Emit_MOVIR(35, x86Reg::x86Reg_edx);
  cache.X86Emit_CALLI((uintptr_t)SelfTestHelper, 0);
  cache.X86Emit_MOVRM(x86Reg::x86Reg_eax, x86BaseReg::x86BaseReg_esi, x86IndexReg::x86IndexReg_none, x86ScaleVal::x86Scale_1, 8);
  cache.X86Emit_MOVRM(x86Reg::x86Reg_ebx, x86BaseReg::x86BaseReg_esi, x86IndexReg::x86IndexReg_none, x86ScaleVal::x86Scale_1, 4);
  cache.X86Emit_MOVMR(x86Reg::x86Reg_ebp, x86BaseReg::x86BaseReg_esi, x86IndexReg::x86IndexReg_none, x86ScaleVal::x86Scale_1, 8);
  cache.X86Emit_MOVRR(x86Reg::x86Reg_edx, x86Reg::x86Reg_ebp);
  cache.X86Emit_MOVRM(x86Reg::x86Reg_edx, outAddr);
  cache.X86Emit_POPAD();
  cache.X86Emit_RETN();
  if(!RunSelfTestBlock(cache) || cells[2] != 42 || cells[1] != 1000 || out != 42)
  {
    snprintf(message, messageSize, "helper-call block failed (%s, got %u %u %u)", cache.A64_FailReason() ? cache.A64_FailReason() : "ran",
      (unsigned)cells[2], (unsigned)cells[1], (unsigned)out);
    return false;
  }

  // 3: a forward jump over a store, and an indexed store that must land.
  cells[0] = cells[1] = cells[2] = cells[3] = 0;
  out = 0;
  cache.A64_BeginBlock();
  cache.X86Emit_PUSHAD();
  cache.X86Emit_MOVIR_Ptr(cellsAddr, x86Reg::x86Reg_esi);
  cache.X86Emit_MOVIM(1, x86MemPtr::x86MemPtr_dword, outAddr);
  cache.X86Emit_JMPI_Label(0);
  cache.X86Emit_MOVIM(2, x86MemPtr::x86MemPtr_dword, outAddr);
  cache.SetLabelPointer(0);
  cache.X86Emit_MOVIR(3, x86Reg::x86Reg_ecx);
  cache.X86Emit_MOVIM(77, x86MemPtr::x86MemPtr_dword, (uintptr_t)x86BaseReg::x86BaseReg_esi, x86IndexReg::x86IndexReg_ecx, x86ScaleVal::x86Scale_4, 0);
  cache.X86Emit_POPAD();
  cache.X86Emit_RETN();
  if(!RunSelfTestBlock(cache) || out != 1 || cells[3] != 77)
  {
    snprintf(message, messageSize, "jump block failed (%s, got %u %u)", cache.A64_FailReason() ? cache.A64_FailReason() : "ran", (unsigned)out, (unsigned)cells[3]);
    return false;
  }

  // 4: a primitive without an Arm64 body must fail the block, not emit nothing.
  cache.A64_BeginBlock();
  cache.X86Emit_PUSHAD();
  cache.X86Emit_ADDRR(x86Reg::x86Reg_eax, x86Reg::x86Reg_ebx);
  cache.X86Emit_POPAD();
  cache.X86Emit_RETN();
  uint32 size;
  {
    const NativeCodeCache::WriteScope writeScope(cache);
    size = cache.A64_EndBlock();
  }
  if(size || cache.A64_Failure() != A64_EMIT_UNSUPPORTED)
  {
    snprintf(message, messageSize, "an unimplemented primitive did not fail its block");
    return false;
  }

  snprintf(message, messageSize, "passed (empty block, helper call, forward jump, unsupported-primitive fallback)");
  return true;
}

#endif // USE_A64_JIT
