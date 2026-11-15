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
//   xmm0-xmm7        -> v16-v23      caller-saved on both sides
//   esp              -> unsupported
//
// Registers the backend keeps for itself:
//   x16              addresses
//   x17              immediates, and the second half of an address too far for one instruction
//   w13 w14 w15      operand and result temporaries within one primitive
//   w8               throwaway results of flag-setting instructions
//   w12              the x86 carry flag, when it lives in a register (see FlagState)
//
// Flags. x86 instructions set flags implicitly and the handlers read them with
// SETcc, Jcc, CMOVcc, ADC, SBB and RCR. AArch64 sets flags only when asked, and
// its carry after a subtraction is the inverse of x86's. So the backend tracks,
// while emitting, what the last flag-producing primitive left behind (FlagState)
// and picks the AArch64 condition to match. A consumer that needs a flag the
// tracker cannot vouch for fails the block instead of guessing.
#ifdef USE_A64_JIT

#include "basetypes.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mpe.h"
#include "NativeCodeCache.h"
#include <asmjit/arm/armutils.h>

using namespace asmjit;
using asmjit::a64::CondCode;

namespace {

constexpr uint32_t kNoReg = 0xFF;

const a64::Gp kAddr = a64::x16;          // address scratch
const a64::Gp kImm = a64::x17;           // immediate scratch
const a64::Gp kT0 = a64::w13;
const a64::Gp kT1 = a64::w14;
const a64::Gp kT2 = a64::w15;
const a64::Gp kDiscard = a64::w8;
const a64::Gp kCarry = a64::w12;

// Host register number for an x86 register number (eax=0 .. edi=7).
inline uint32_t HostRegId(const uint32_t x86Index)
{
  static const uint8_t ids[8] = { 9, 10, 11, 19, kNoReg /*esp*/, 20, 21, 22 };
  return ids[x86Index & 7];
}

inline a64::Gp W(const uint32_t id) { return a64::Gp::make_r32(id); }
inline a64::Gp X(const uint32_t id) { return a64::Gp::make_r64(id); }

// An x86 general register operand: which host register holds it, how wide it
// is, and where in the host register it sits (ah/ch/dh/bh are bits 8-15).
struct GpOperand
{
  uint32_t id;
  uint32_t bits;
  uint32_t shift;
  bool IsValid() const { return id != kNoReg; }
  bool Is32() const { return bits == 32; }
};

inline GpOperand DecodeReg(const x86Reg reg)
{
  const uint32_t v = (uint32_t)reg;
  if(v < 4)  return { HostRegId(v), 8, 0 };
  if(v < 8)  return { HostRegId(v - 4), 8, 8 };
  if(v < 16) return { HostRegId(v - 8), 16, 0 };
  if(v < 24) return { HostRegId(v - 16), 32, 0 };
  return { kNoReg, 0, 0 };
}

inline uint32_t XmmId(const x86Reg reg)
{
  const int index = (int)reg - (int)x86Reg::x86Reg_xmm0;
  return (index >= 0 && index < 8) ? (uint32_t)(16 + index) : kNoReg;
}

inline a64::Vec Q(const uint32_t id) { return a64::Vec::make_v128(id); }

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

// What the flags hold at the point being emitted. One block is compiled at a
// time, so this is file state, reset at the start of each block.
enum class Carry
{
  Unknown,
  Direct,     // AArch64 C == x86 CF: after an addition, or a logic op (both clear it)
  Inverted,   // AArch64 C == !CF: after a subtraction or compare
  InRegister  // CF is 0 or 1 in kCarry; AArch64 C says nothing
};

struct FlagState
{
  bool n, z, v;
  Carry carry;
};

FlagState g_flags;

inline void FlagsUnknown()
{
  g_flags = { false, false, false, Carry::Unknown };
}

inline void FlagsArith(const Carry carry)
{
  g_flags = { true, true, true, carry };
}

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
  if((value >> 16) == 0xFFFFu && (value & 0xFFFFu) != 0xFFFFu)
    a.movn(w, (~value) & 0xFFFFu);     // small negative numbers in one instruction
  else
    LoadImm64(a, w.x(), value);
}

enum class Op { Add, Adc, Sub, Sbb, And, Or, Xor, Cmp, Test };

inline bool OpWritesResult(const Op op) { return op != Op::Cmp && op != Op::Test; }

// The emitter proper: one of these wraps the cache for the duration of a primitive.
struct Emitter
{
  NativeCodeCache &c;
  a64::Assembler &a;

  explicit Emitter(NativeCodeCache &cache) : c(cache), a(*cache.a64As) {}

  bool Check(const GpOperand &r, const char * const what)
  {
    if(r.IsValid())
      return true;
    c.A64_Unsupported(what);
    return false;
  }

  // The operand's value, zero-extended, in a W register: the host register
  // itself for a 32-bit operand, 'tmp' otherwise.
  a64::Gp Read(const GpOperand &r, const a64::Gp &tmp)
  {
    if(r.Is32())
      return W(r.id);
    a.ubfx(tmp, W(r.id), r.shift, r.bits);
    return tmp;
  }

  a64::Gp ReadSigned(const GpOperand &r, const a64::Gp &tmp)
  {
    if(r.Is32())
      return W(r.id);
    a.sbfx(tmp, W(r.id), r.shift, r.bits);
    return tmp;
  }

  // Where to compute a result bound for 'r': in place for 32 bits, in 'tmp' otherwise.
  a64::Gp Target(const GpOperand &r, const a64::Gp &tmp) { return r.Is32() ? W(r.id) : tmp; }

  // Stores 'value' into the operand, leaving the rest of the host register alone.
  void Write(const GpOperand &r, const a64::Gp &value)
  {
    if(r.Is32())
    {
      if(value.id() != r.id)
        a.mov(W(r.id), value);
    }
    else
      a.bfi(W(r.id), value, r.shift, r.bits);
  }

  // An x86 address as a base register and what is left of the displacement.
  // 'base' is a register number (0-7), X86_NO_BASE, or an absolute host address.
  bool Address(const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp, a64::Gp &outBase, int32 &outDisp)
  {
    const bool bHasIndex = (index != x86IndexReg::x86IndexReg_none);
    const uint32_t indexId = bHasIndex ? HostRegId((uint32_t)index) : kNoReg;
    const uint32_t shift = (uint32_t)scale;

    if(bHasIndex && indexId == kNoReg)
    {
      c.A64_Unsupported("index register");
      return false;
    }

    if(base == X86_NO_BASE || base > 7)
    {
      // [index*scale + disp] or [absolute + index*scale + disp]
      const uint64_t address = (base == X86_NO_BASE) ? (uint64_t)(int64_t)disp : (uint64_t)base + (int64_t)disp;
      LoadImm64(a, kAddr, address);
      if(bHasIndex)
        a.add(kAddr, kAddr, X(indexId), a64::lsl(shift));
      outBase = kAddr;
      outDisp = 0;
      return true;
    }

    const uint32_t baseId = HostRegId((uint32_t)base);
    if(baseId == kNoReg)
    {
      c.A64_Unsupported("base register");
      return false;
    }

    outBase = X(baseId);
    outDisp = disp;
    if(bHasIndex)
    {
      a.add(kAddr, outBase, X(indexId), a64::lsl(shift));
      outBase = kAddr;
    }
    return true;
  }

  // A memory operand for an access of 'size' bytes. May use kAddr and kImm, so
  // callers load immediates only after building the operand.
  a64::Mem Mem(const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp, const uint32 size)
  {
    a64::Gp b = kAddr;
    int32 d = 0;
    if(!Address(base, index, scale, disp, b, d))
      return a64::ptr(kAddr);

    // Scaled unsigned 12-bit, or unscaled signed 9-bit: asmjit picks the encoding.
    if((d >= 0 && (d % (int32)size) == 0 && (d / (int32)size) < 4096) || (d >= -256 && d <= 255))
      return a64::ptr(b, d);

    LoadImm64(a, kImm, (uint64_t)(int64_t)d);
    a.add(kAddr, b, kImm);
    return a64::ptr(kAddr);
  }

  void Load(const a64::Gp &w, const a64::Mem &m, const uint32 size)
  {
    switch(size)
    {
      case 1: a.ldrb(w, m); break;
      case 2: a.ldrh(w, m); break;
      default: a.ldr(w, m); break;
    }
  }

  void Store(const a64::Gp &w, const a64::Mem &m, const uint32 size)
  {
    switch(size)
    {
      case 1: a.strb(w, m); break;
      case 2: a.strh(w, m); break;
      default: a.str(w, m); break;
    }
  }

  // Puts the x86 carry flag where an add-with-carry (bInverted false) or a
  // subtract-with-borrow (true) expects it. Clobbers N, Z and V, which the
  // instruction that follows sets again anyway.
  bool PrepareCarry(const bool bInverted)
  {
    switch(g_flags.carry)
    {
      case Carry::Direct:
        if(!bInverted)
          return true;
        a.cset(kCarry, CondCode::kCS);
        break;
      case Carry::Inverted:
        if(bInverted)
          return true;
        a.cset(kCarry, CondCode::kCC);
        break;
      case Carry::InRegister:
        break;
      default:
        c.A64_Unsupported("carry-in unknown");
        return false;
    }

    if(bInverted)
      a.negs(kDiscard, kCarry);   // C = (carry == 0)
    else
      a.cmp(kCarry, 1);           // C = (carry >= 1)
    return true;
  }

  // dest = left <op> right, with x86's flags when the operands are 32 bits wide.
  // Narrower operands produce the value only and leave the flags unknown.
  void Binary(const Op op, const a64::Gp &dest, const a64::Gp &left, const a64::Gp &right, const bool bFlags)
  {
    if(!bFlags)
    {
      switch(op)
      {
        case Op::Add: a.add(dest, left, right); break;
        case Op::Sub: a.sub(dest, left, right); break;
        case Op::And: a.and_(dest, left, right); break;
        case Op::Or:  a.orr(dest, left, right); break;
        case Op::Xor: a.eor(dest, left, right); break;
        default: c.A64_Unsupported("flag-dependent op on a sub-register"); break;
      }
      FlagsUnknown();
      return;
    }

    switch(op)
    {
      case Op::Add: a.adds(dest, left, right); FlagsArith(Carry::Direct); break;
      case Op::Sub: a.subs(dest, left, right); FlagsArith(Carry::Inverted); break;
      case Op::Cmp: a.cmp(left, right); FlagsArith(Carry::Inverted); break;
      case Op::And: a.ands(dest, left, right); FlagsArith(Carry::Direct); break;
      case Op::Test: a.tst(left, right); FlagsArith(Carry::Direct); break;
      case Op::Or:  a.orr(dest, left, right); a.tst(dest, dest); FlagsArith(Carry::Direct); break;
      case Op::Xor: a.eor(dest, left, right); a.tst(dest, dest); FlagsArith(Carry::Direct); break;
      case Op::Adc:
        if(PrepareCarry(false)) { a.adcs(dest, left, right); FlagsArith(Carry::Direct); }
        break;
      case Op::Sbb:
        if(PrepareCarry(true)) { a.sbcs(dest, left, right); FlagsArith(Carry::Inverted); }
        break;
    }
  }

  // As Binary, with a constant on the right. Uses the immediate form of the
  // instruction where AArch64 has one for this value, kImm otherwise.
  void BinaryImm(const Op op, const a64::Gp &dest, const a64::Gp &left, const uint32 imm, const bool bFlags)
  {
    if(bFlags)
    {
      const bool bAddSub = (op == Op::Add || op == Op::Sub || op == Op::Cmp) && imm < 4096;
      const bool bLogic = (op == Op::And || op == Op::Test) && arm::Utils::is_logical_imm(imm, 32);
      if(bAddSub)
      {
        if(op == Op::Add) { a.adds(dest, left, imm); FlagsArith(Carry::Direct); }
        else if(op == Op::Sub) { a.subs(dest, left, imm); FlagsArith(Carry::Inverted); }
        else { a.cmp(left, imm); FlagsArith(Carry::Inverted); }
        return;
      }
      if(bLogic)
      {
        if(op == Op::And) a.ands(dest, left, imm); else a.tst(left, imm);
        FlagsArith(Carry::Direct);
        return;
      }
    }

    LoadImm32(a, kImm.w(), imm);
    Binary(op, dest, left, kImm.w(), bFlags);
  }

  // reg <op>= reg
  void BinaryRR(const Op op, const x86Reg regDest, const x86Reg regSrc, const char * const what)
  {
    const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
    if(!Check(d, what) || !Check(s, what))
      return;
    const a64::Gp left = Read(d, kT0);
    const a64::Gp right = Read(s, kT1);
    const a64::Gp target = Target(d, kT0);
    Binary(op, target, left, right, d.Is32());
    if(OpWritesResult(op))
      Write(d, target);
  }

  // reg <op>= imm
  void BinaryIR(const Op op, const intptr_t imm, const x86Reg regDest, const char * const what)
  {
    const GpOperand d = DecodeReg(regDest);
    if(!Check(d, what))
      return;

    // A constant that does not fit 31 bits may be a host pointer, or be added to
    // one. The x86-64 emitter does these in 64 bits, and so does this: the low
    // half of the result is the same either way and a pointer survives.
    const uint64_t wide = (uint64_t)imm;
    if(d.Is32() && (wide >> 32) != 0 && op != Op::Adc && op != Op::Sbb && op != Op::Test)
    {
      const bool bPointer = (wide >> 32) != 0xFFFFFFFFu;
      LoadImm64(a, kImm, wide);
      if(op == Op::Cmp)
      {
        if(bPointer)
          a.cmp(X(d.id), kImm);
        else
          a.cmp(W(d.id), kImm.w());
        FlagsArith(Carry::Inverted);
        return;
      }

      // The flags as the 32-bit operation would leave them, for a constant that
      // is just a negative number; then the value in 64 bits.
      if(!bPointer)
        Binary(op, kDiscard, W(d.id), kImm.w(), true);
      switch(op)
      {
        case Op::Add: a.add(X(d.id), X(d.id), kImm); break;
        case Op::Sub: a.sub(X(d.id), X(d.id), kImm); break;
        case Op::And: a.and_(X(d.id), X(d.id), kImm); break;
        case Op::Or:  a.orr(X(d.id), X(d.id), kImm); break;
        default:      a.eor(X(d.id), X(d.id), kImm); break;
      }
      if(bPointer)
        FlagsUnknown();
      return;
    }

    const a64::Gp left = Read(d, kT0);
    const a64::Gp target = Target(d, kT0);
    BinaryImm(op, target, left, (uint32)imm, d.Is32());
    if(OpWritesResult(op))
      Write(d, target);
  }

  // reg <op>= [mem]
  void BinaryMR(const Op op, const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp, const char * const what)
  {
    const GpOperand d = DecodeReg(regDest);
    if(!Check(d, what))
      return;
    const uint32 size = d.bits / 8;
    const a64::Mem m = Mem(base, index, scale, disp, size);
    Load(kT1, m, size);
    const a64::Gp left = Read(d, kT0);
    const a64::Gp target = Target(d, kT0);
    Binary(op, target, left, kT1, d.Is32());
    if(OpWritesResult(op))
      Write(d, target);
  }

  // [mem] <op>= reg
  void BinaryRM(const Op op, const x86Reg regSrc, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp, const char * const what)
  {
    const GpOperand s = DecodeReg(regSrc);
    if(!Check(s, what))
      return;
    const uint32 size = s.bits / 8;
    const a64::Mem m = Mem(base, index, scale, disp, size);
    Load(kT0, m, size);
    const a64::Gp right = Read(s, kT1);
    Binary(op, kT0, kT0, right, s.Is32());
    if(OpWritesResult(op))
      Store(kT0, m, size);
  }

  // [mem] <op>= imm
  void BinaryIM(const Op op, const int32 imm, const x86MemPtr ptrType, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp, const char * const what)
  {
    if(ptrType == x86MemPtr::x86MemPtr_qword)
    {
      c.A64_Unsupported(what);
      return;
    }
    const uint32 size = (ptrType == x86MemPtr::x86MemPtr_byte) ? 1 : (ptrType == x86MemPtr::x86MemPtr_word) ? 2 : 4;
    const a64::Mem m = Mem(base, index, scale, disp, size);
    Load(kT0, m, size);
    BinaryImm(op, kT0, kT0, (uint32)imm, size == 4);
    if(OpWritesResult(op))
      Store(kT0, m, size);
  }

  // The AArch64 condition that is true exactly when the x86 condition would be,
  // given what the flags currently hold.
  bool Condition(const int x86cc, CondCode &out)
  {
    const FlagState &f = g_flags;
    const bool bDirect = (f.carry == Carry::Direct), bInverted = (f.carry == Carry::Inverted);
    bool ok = false;
    switch(x86cc & 0xF)
    {
      case X86_CC_O:   ok = f.v; out = CondCode::kVS; break;
      case X86_CC_NO:  ok = f.v; out = CondCode::kVC; break;
      case X86_CC_B:   ok = bDirect || bInverted; out = bDirect ? CondCode::kCS : CondCode::kCC; break;
      case X86_CC_NB:  ok = bDirect || bInverted; out = bDirect ? CondCode::kCC : CondCode::kCS; break;
      case X86_CC_Z:   ok = f.z; out = CondCode::kEQ; break;
      case X86_CC_NZ:  ok = f.z; out = CondCode::kNE; break;
      case X86_CC_BE:  ok = bInverted && f.z; out = CondCode::kLS; break;
      case X86_CC_NBE: ok = bInverted && f.z; out = CondCode::kHI; break;
      case X86_CC_S:   ok = f.n; out = CondCode::kMI; break;
      case X86_CC_NS:  ok = f.n; out = CondCode::kPL; break;
      case X86_CC_L:   ok = f.n && f.v; out = CondCode::kLT; break;
      case X86_CC_NL:  ok = f.n && f.v; out = CondCode::kGE; break;
      case X86_CC_LE:  ok = f.n && f.v && f.z; out = CondCode::kLE; break;
      case X86_CC_NLE: ok = f.n && f.v && f.z; out = CondCode::kGT; break;
      default: break; // parity: AArch64 has no such flag
    }
    if(!ok)
      c.A64_Unsupported("condition on flags the last operation did not produce");
    return ok;
  }

  void SetCC(const int x86cc, const x86Reg reg)
  {
    GpOperand r = DecodeReg(reg);
    if(!Check(r, "SETcc register"))
      return;
    // SETcc writes one byte. Named with a wider register, it is the low byte.
    if(r.bits != 8) { r.bits = 8; r.shift = 0; }

    if((x86cc == X86_CC_B || x86cc == X86_CC_NB) && g_flags.carry == Carry::InRegister)
    {
      if(x86cc == X86_CC_B)
        a.bfi(W(r.id), kCarry, r.shift, 8);
      else
      {
        a.eor(kT2, kCarry, 1);
        a.bfi(W(r.id), kT2, r.shift, 8);
      }
      return;
    }

    CondCode cond;
    if(!Condition(x86cc, cond))
      return;
    a.cset(kT2, cond);
    a.bfi(W(r.id), kT2, r.shift, 8);
  }

  void CMov(const int x86cc, const x86Reg regDest, const x86Reg regSrc)
  {
    const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
    if(!Check(d, "CMOVcc register") || !Check(s, "CMOVcc register"))
      return;
    if(!d.Is32() || !s.Is32())
    {
      c.A64_Unsupported("CMOVcc sub-register");
      return;
    }
    CondCode cond;
    if(!Condition(x86cc, cond))
      return;
    a.csel(W(d.id), W(s.id), W(d.id), cond);
  }

  enum class Shift { Shl, Shr, Sar, Rol, Ror };

  // reg <shift>= count, 1..31. x86 leaves the operand and the flags alone for a
  // count of zero; nothing reads the flags of a shift, so they become unknown.
  void ShiftImm(const Shift kind, const x86Reg regDest, const uint32 rawCount, const char * const what)
  {
    const GpOperand d = DecodeReg(regDest);
    if(!Check(d, what))
      return;
    const uint32 count = rawCount & 31;
    if(!count)
      return;

    if(d.Is32())
    {
      const a64::Gp w = W(d.id);
      switch(kind)
      {
        case Shift::Shl: a.lsl(w, w, count); break;
        case Shift::Shr: a.lsr(w, w, count); break;
        case Shift::Sar: a.asr(w, w, count); break;
        case Shift::Rol: a.ror(w, w, 32 - count); break;
        case Shift::Ror: a.ror(w, w, count); break;
      }
    }
    else
    {
      if(kind == Shift::Rol || kind == Shift::Ror)
      {
        c.A64_Unsupported("rotate of a sub-register");
        return;
      }
      const a64::Gp v = (kind == Shift::Sar) ? ReadSigned(d, kT0) : Read(d, kT0);
      switch(kind)
      {
        case Shift::Shl: a.lsl(kT0, v, count); break;
        case Shift::Shr: a.lsr(kT0, v, count); break;
        default:         a.asr(kT0, v, count); break;
      }
      Write(d, kT0);
    }
    FlagsUnknown();
  }

  // reg <shift>= cl. Both architectures take the count modulo 32.
  void ShiftCL(const Shift kind, const x86Reg regDest, const char * const what)
  {
    const GpOperand d = DecodeReg(regDest);
    if(!Check(d, what))
      return;
    const a64::Gp count = W(HostRegId(1)); // ecx

    if(!d.Is32() && (kind == Shift::Rol || kind == Shift::Ror))
    {
      c.A64_Unsupported("rotate of a sub-register");
      return;
    }

    const a64::Gp v = (kind == Shift::Sar) ? ReadSigned(d, kT0) : Read(d, kT0);
    const a64::Gp target = Target(d, kT0);
    switch(kind)
    {
      case Shift::Shl: a.lslv(target, v, count); break;
      case Shift::Shr: a.lsrv(target, v, count); break;
      case Shift::Sar: a.asrv(target, v, count); break;
      case Shift::Ror: a.rorv(target, v, count); break;
      case Shift::Rol: a.neg(kT1, count); a.rorv(target, v, kT1); break;
    }
    Write(d, target);
    FlagsUnknown();
  }

  // SHLD/SHRD: shift 'dest' by 'count' (already 0..31, in a W register or as a
  // constant), filling from 'src'. Done on the 64-bit concatenation of the two.
  void DoubleShift(const bool bLeft, const x86Reg regDest, const x86Reg regSrc, const a64::Gp *countReg, const uint32 countImm)
  {
    const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
    if(!Check(d, "double shift register") || !Check(s, "double shift register"))
      return;
    if(!d.Is32() || !s.Is32())
    {
      c.A64_Unsupported("double shift of a sub-register");
      return;
    }
    const a64::Gp pair = kT0.x();
    if(bLeft)
    {
      a.mov(kT0, W(s.id));                 // low half: src
      a.bfi(pair, X(d.id), 32, 32);        // high half: dest
      if(countReg) a.lslv(pair, pair, countReg->x()); else a.lsl(pair, pair, countImm);
      a.lsr(pair, pair, 32);
    }
    else
    {
      a.mov(kT0, W(d.id));                 // low half: dest
      a.bfi(pair, X(s.id), 32, 32);        // high half: src
      if(countReg) a.lsrv(pair, pair, countReg->x()); else a.lsr(pair, pair, countImm);
    }
    a.mov(W(d.id), kT0);
    FlagsUnknown();
  }
};

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
  FlagsUnknown();
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

  // A jump to a label that was never bound would be emitted as a jump to
  // itself: generated code that spins forever.
  if(!a64Failure && a64Code->has_unresolved_fixups())
  {
    a64Failure = A64_EMIT_UNSUPPORTED;
    a64FailReason = "jump to a label that was never bound";
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

  // An index that is already bound belongs to an earlier handler; this one is
  // binding a label nothing has jumped to, which still needs to be a fresh one.
  if(!a64Labels[labelIndex].is_valid() || a64LabelBound[labelIndex])
    a64Labels[labelIndex] = a64As->new_label();
  a64As->bind(a64Labels[labelIndex]);
  a64LabelBound[labelIndex] = true;

  // Control arrives here from more than one place, each with its own flags.
  FlagsUnknown();
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

// The 64-bit table load the bank-pointer lookup in EmitMEM.cpp needs: ebx holds
// the bank index times four on entry and the bank's host pointer on exit.
void NativeCodeCache::A64_LoadBankPtr(const uintptr_t table)
{
  a64::Assembler &a = *a64As;
  const a64::Gp rbx = X(HostRegId(3));
  LoadImm64(a, kAddr, (uint64_t)table);
  a.add(kAddr, kAddr, rbx, a64::lsl(1));
  a.ldr(rbx, a64::ptr(kAddr));
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
// The callee may use every caller-saved register, the flags and v16-v23.
void NativeCodeCache::X86Emit_CALLI(uintptr_t offset, uint16)
{
  a64::Assembler &a = *a64As;
  a.mov(a64::x0, X(HostRegId(1)));
  a.mov(a64::x1, X(HostRegId(2)));
  LoadImm64(a, kAddr, (uint64_t)offset);
  a.blr(kAddr);
  a.mov(X(HostRegId(0)), a64::x0);
  FlagsUnknown();
}

void NativeCodeCache::X86Emit_JMPI_Label(const uint32 labelIndex)
{
  a64As->b(A64_GetLabel(labelIndex));
}

void NativeCodeCache::X86Emit_JCC_Label(const int8 conditionCode, const uint32 labelIndex)
{
  Emitter e(*this);
  CondCode cond;
  if(e.Condition(conditionCode, cond))
    e.a.b(cond, A64_GetLabel(labelIndex));
}

// --- Moves ---

void NativeCodeCache::X86Emit_MOVRR(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
  if(!e.Check(d, "MOVRR register") || !e.Check(s, "MOVRR register"))
    return;
  if(d.bits != s.bits) { A64_Unsupported("MOVRR between sizes"); return; }
  e.Write(d, e.Read(s, kT0));
}

void NativeCodeCache::X86Emit_MOVRR64(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
  if(!e.Check(d, "MOVRR64 register") || !e.Check(s, "MOVRR64 register"))
    return;
  e.a.mov(X(d.id), X(s.id));
}

void NativeCodeCache::X86Emit_MOVZXRR(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
  if(!e.Check(d, "MOVZXRR register") || !e.Check(s, "MOVZXRR register"))
    return;
  if(!d.Is32() || s.Is32()) { A64_Unsupported("MOVZXRR sizes"); return; }
  e.a.ubfx(W(d.id), W(s.id), s.shift, s.bits);
}

// Like the x86-64 emitter, this loads the full sign-extended value when it
// does not fit 31 bits: handlers use it for host pointers as well as constants.
void NativeCodeCache::X86Emit_MOVIR(const intptr_t imm, const x86Reg regDest)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "MOVIR register"))
    return;
  if(!d.Is32())
  {
    LoadImm32(e.a, kT0, (uint32)imm);
    e.Write(d, kT0);
  }
  else if((uintptr_t)imm > (uintptr_t)INT32_MAX)
    LoadImm64(e.a, X(d.id), (uint64_t)imm);
  else
    LoadImm32(e.a, W(d.id), (uint32)imm);
}

void NativeCodeCache::X86Emit_MOVIR_Ptr(const uintptr_t addr, const x86Reg regDest)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "MOVIR_Ptr register"))
    return;
  LoadImm64(e.a, X(d.id), (uint64_t)addr);
}

void NativeCodeCache::X86Emit_MOVMR(const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "MOVMR register"))
    return;
  const uint32 size = d.bits / 8;
  const a64::Mem m = e.Mem(base, index, scale, disp, size);
  const a64::Gp target = e.Target(d, kT0);
  e.Load(target, m, size);
  e.Write(d, target);
}

void NativeCodeCache::X86Emit_MOVRM(const x86Reg regSrc, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const GpOperand s = DecodeReg(regSrc);
  if(!e.Check(s, "MOVRM register"))
    return;
  const uint32 size = s.bits / 8;
  const a64::Mem m = e.Mem(base, index, scale, disp, size);
  // A store of the low 8 or 16 bits needs no extraction; only ah/ch/dh/bh do.
  const a64::Gp value = s.shift ? e.Read(s, kT0) : W(s.id);
  e.Store(value, m, size);
}

void NativeCodeCache::X86Emit_MOVZXMR(const x86Reg regDest, const x86MemPtr ptrType, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "MOVZXMR register"))
    return;
  if(!d.Is32() || (ptrType != x86MemPtr::x86MemPtr_byte && ptrType != x86MemPtr::x86MemPtr_word)) { A64_Unsupported("MOVZXMR sizes"); return; }
  const uint32 size = (ptrType == x86MemPtr::x86MemPtr_byte) ? 1 : 2;
  const a64::Mem m = e.Mem(base, index, scale, disp, size);
  e.Load(W(d.id), m, size);
}

void NativeCodeCache::X86Emit_MOVIM(const int32 imm, const x86MemPtr ptrType, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  if(ptrType == x86MemPtr::x86MemPtr_qword) { A64_Unsupported("MOVIM qword"); return; }
  const uint32 size = (ptrType == x86MemPtr::x86MemPtr_byte) ? 1 : (ptrType == x86MemPtr::x86MemPtr_word) ? 2 : 4;
  const a64::Mem m = e.Mem(base, index, scale, disp, size);
  if(imm == 0)
  {
    e.Store(a64::wzr, m, size);
    return;
  }
  LoadImm32(e.a, kImm.w(), (uint32)imm);
  e.Store(kImm.w(), m, size);
}

// LEA computes an address and keeps the low 32 bits. Handlers use it as a
// three-operand add and shift that leaves the flags alone.
void NativeCodeCache::X86Emit_LEA(const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "LEA register"))
    return;
  if(!d.Is32()) { A64_Unsupported("LEA sub-register"); return; }
  a64::Gp b = kAddr;
  int32 rest = 0;
  if(!e.Address(base, index, scale, disp, b, rest))
    return;
  if(rest)
  {
    LoadImm64(e.a, kImm, (uint64_t)(int64_t)rest);
    e.a.add(kAddr, b, kImm);
    b = kAddr;
  }
  e.a.mov(W(d.id), b.w());
}

void NativeCodeCache::X86Emit_BSWAP(const x86Reg reg)
{
  Emitter e(*this);
  const GpOperand r = DecodeReg(reg);
  if(!e.Check(r, "BSWAP register"))
    return;
  if(!r.Is32()) { A64_Unsupported("BSWAP sub-register"); return; }
  e.a.rev(W(r.id), W(r.id));
}

// --- Arithmetic and logic ---

void NativeCodeCache::X86Emit_ADDRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::Add, d, s, "ADDRR register"); }
void NativeCodeCache::X86Emit_ADCRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::Adc, d, s, "ADCRR register"); }
void NativeCodeCache::X86Emit_SUBRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::Sub, d, s, "SUBRR register"); }
void NativeCodeCache::X86Emit_SBBRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::Sbb, d, s, "SBBRR register"); }
void NativeCodeCache::X86Emit_ANDRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::And, d, s, "ANDRR register"); }
void NativeCodeCache::X86Emit_ORRR(const x86Reg d, const x86Reg s)  { Emitter(*this).BinaryRR(Op::Or, d, s, "ORRR register"); }
void NativeCodeCache::X86Emit_CMPRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::Cmp, d, s, "CMPRR register"); }
void NativeCodeCache::X86Emit_TESTRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::Test, d, s, "TESTRR register"); }

// xor reg,reg is how the handlers clear a register; it still sets the flags.
void NativeCodeCache::X86Emit_XORRR(const x86Reg d, const x86Reg s) { Emitter(*this).BinaryRR(Op::Xor, d, s, "XORRR register"); }

void NativeCodeCache::X86Emit_ADDIR(const intptr_t imm, const x86Reg d) { Emitter(*this).BinaryIR(Op::Add, imm, d, "ADDIR register"); }
void NativeCodeCache::X86Emit_ADCIR(const intptr_t imm, const x86Reg d) { Emitter(*this).BinaryIR(Op::Adc, imm, d, "ADCIR register"); }
void NativeCodeCache::X86Emit_SUBIR(const intptr_t imm, const x86Reg d) { Emitter(*this).BinaryIR(Op::Sub, imm, d, "SUBIR register"); }
void NativeCodeCache::X86Emit_SBBIR(const intptr_t imm, const x86Reg d) { Emitter(*this).BinaryIR(Op::Sbb, imm, d, "SBBIR register"); }
void NativeCodeCache::X86Emit_ANDIR(const intptr_t imm, const x86Reg d) { Emitter(*this).BinaryIR(Op::And, imm, d, "ANDIR register"); }
void NativeCodeCache::X86Emit_ORIR(const intptr_t imm, const x86Reg d)  { Emitter(*this).BinaryIR(Op::Or, imm, d, "ORIR register"); }
void NativeCodeCache::X86Emit_XORIR(const intptr_t imm, const x86Reg d) { Emitter(*this).BinaryIR(Op::Xor, imm, d, "XORIR register"); }
void NativeCodeCache::X86Emit_CMPIR(const intptr_t imm, const x86Reg d) { Emitter(*this).BinaryIR(Op::Cmp, imm, d, "CMPIR register"); }
void NativeCodeCache::X86Emit_TESTIR(const uint32 imm, const x86Reg s)  { Emitter(*this).BinaryIR(Op::Test, (intptr_t)imm, s, "TESTIR register"); }

void NativeCodeCache::X86Emit_ADDMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryMR(Op::Add, d, b, i, s, disp, "ADDMR register"); }
void NativeCodeCache::X86Emit_ADCMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryMR(Op::Adc, d, b, i, s, disp, "ADCMR register"); }
void NativeCodeCache::X86Emit_SUBMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryMR(Op::Sub, d, b, i, s, disp, "SUBMR register"); }
void NativeCodeCache::X86Emit_SBBMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryMR(Op::Sbb, d, b, i, s, disp, "SBBMR register"); }
void NativeCodeCache::X86Emit_ANDMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryMR(Op::And, d, b, i, s, disp, "ANDMR register"); }
void NativeCodeCache::X86Emit_ORMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp)  { Emitter(*this).BinaryMR(Op::Or, d, b, i, s, disp, "ORMR register"); }
void NativeCodeCache::X86Emit_XORMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryMR(Op::Xor, d, b, i, s, disp, "XORMR register"); }
void NativeCodeCache::X86Emit_CMPMR(const x86Reg d, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryMR(Op::Cmp, d, b, i, s, disp, "CMPMR register"); }

void NativeCodeCache::X86Emit_ADDRM(const x86Reg r, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryRM(Op::Add, r, b, i, s, disp, "ADDRM register"); }
void NativeCodeCache::X86Emit_SUBRM(const x86Reg r, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryRM(Op::Sub, r, b, i, s, disp, "SUBRM register"); }
void NativeCodeCache::X86Emit_ANDRM(const x86Reg r, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryRM(Op::And, r, b, i, s, disp, "ANDRM register"); }
void NativeCodeCache::X86Emit_ORRM(const x86Reg r, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp)  { Emitter(*this).BinaryRM(Op::Or, r, b, i, s, disp, "ORRM register"); }
void NativeCodeCache::X86Emit_XORRM(const x86Reg r, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryRM(Op::Xor, r, b, i, s, disp, "XORRM register"); }

void NativeCodeCache::X86Emit_ADDIM(const int32 imm, const x86MemPtr p, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryIM(Op::Add, imm, p, b, i, s, disp, "ADDIM qword"); }
void NativeCodeCache::X86Emit_SUBIM(const int32 imm, const x86MemPtr p, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryIM(Op::Sub, imm, p, b, i, s, disp, "SUBIM qword"); }
void NativeCodeCache::X86Emit_ANDIM(const int32 imm, const x86MemPtr p, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryIM(Op::And, imm, p, b, i, s, disp, "ANDIM qword"); }
void NativeCodeCache::X86Emit_ORIM(const int32 imm, const x86MemPtr p, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp)  { Emitter(*this).BinaryIM(Op::Or, imm, p, b, i, s, disp, "ORIM qword"); }
void NativeCodeCache::X86Emit_XORIM(const int32 imm, const x86MemPtr p, const uintptr_t b, const x86IndexReg i, const x86ScaleVal s, const int32 disp) { Emitter(*this).BinaryIM(Op::Xor, imm, p, b, i, s, disp, "XORIM qword"); }

// 64-bit pointer arithmetic. Nothing reads the flags of these.
void NativeCodeCache::X86Emit_ADDRR64(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
  if(!e.Check(d, "ADDRR64 register") || !e.Check(s, "ADDRR64 register"))
    return;
  e.a.add(X(d.id), X(d.id), X(s.id));
  FlagsUnknown();
}

void NativeCodeCache::X86Emit_NEGR(const x86Reg regDest)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "NEGR register"))
    return;
  if(!d.Is32()) { A64_Unsupported("NEGR sub-register"); return; }
  e.a.negs(W(d.id), W(d.id));
  FlagsArith(Carry::Inverted); // x86 CF = (operand != 0), AArch64 C = (operand == 0)
}

void NativeCodeCache::X86Emit_NOTR(const x86Reg regDest)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "NOTR register"))
    return;
  if(!d.Is32()) { A64_Unsupported("NOTR sub-register"); return; }
  e.a.mvn(W(d.id), W(d.id));
}

// INC and DEC leave the x86 carry flag alone. The AArch64 add and subtract
// here do not, so afterwards the carry is simply not known.
void NativeCodeCache::X86Emit_INCR(const x86Reg reg)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(reg);
  if(!e.Check(d, "INCR register"))
    return;
  if(!d.Is32()) { A64_Unsupported("INCR sub-register"); return; }
  e.a.adds(W(d.id), W(d.id), 1);
  g_flags = { true, true, true, Carry::Unknown };
}

void NativeCodeCache::X86Emit_DECR(const x86Reg reg)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(reg);
  if(!e.Check(d, "DECR register"))
    return;
  if(!d.Is32()) { A64_Unsupported("DECR sub-register"); return; }
  e.a.subs(W(d.id), W(d.id), 1);
  g_flags = { true, true, true, Carry::Unknown };
}

// --- Multiplies ---

void NativeCodeCache::X86Emit_IMULRRR(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
  if(!e.Check(d, "IMULRRR register") || !e.Check(s, "IMULRRR register"))
    return;
  if(!d.Is32() || !s.Is32()) { A64_Unsupported("IMULRRR sub-register"); return; }
  e.a.mul(W(d.id), W(d.id), W(s.id));
  FlagsUnknown();
}

void NativeCodeCache::X86Emit_IMULIRR(const x86Reg regDest, const int32 imm, const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
  if(!e.Check(d, "IMULIRR register") || !e.Check(s, "IMULIRR register"))
    return;
  if(!d.Is32() || !s.Is32()) { A64_Unsupported("IMULIRR sub-register"); return; }
  LoadImm32(e.a, kImm.w(), (uint32)imm);
  e.a.mul(W(d.id), W(s.id), kImm.w());
  FlagsUnknown();
}

// One-operand IMUL: edx:eax = eax * operand, signed.
void NativeCodeCache::X86Emit_IMULRR(const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand s = DecodeReg(regSrc);
  if(!e.Check(s, "IMULRR register"))
    return;
  if(!s.Is32()) { A64_Unsupported("IMULRR sub-register"); return; }
  const a64::Gp eax = W(HostRegId(0)), edx = W(HostRegId(2));
  e.a.smull(kT0.x(), eax, W(s.id));
  e.a.mov(eax, kT0);
  e.a.lsr(kT0.x(), kT0.x(), 32);
  e.a.mov(edx, kT0);
  FlagsUnknown();
}

void NativeCodeCache::X86Emit_IMULMR(const x86MemPtr ptrType, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  if(ptrType != x86MemPtr::x86MemPtr_dword) { A64_Unsupported("IMULMR size"); return; }
  const a64::Mem m = e.Mem(base, index, scale, disp, 4);
  const a64::Gp eax = W(HostRegId(0)), edx = W(HostRegId(2));
  e.a.ldr(kT1, m);
  e.a.smull(kT0.x(), eax, kT1);
  e.a.mov(eax, kT0);
  e.a.lsr(kT0.x(), kT0.x(), 32);
  e.a.mov(edx, kT0);
  FlagsUnknown();
}

// --- Shifts and rotates ---

void NativeCodeCache::X86Emit_SHLIR(const x86Reg d, const uint8 n) { Emitter(*this).ShiftImm(Emitter::Shift::Shl, d, n, "SHLIR register"); }
void NativeCodeCache::X86Emit_SHRIR(const x86Reg d, const uint8 n) { Emitter(*this).ShiftImm(Emitter::Shift::Shr, d, n, "SHRIR register"); }
void NativeCodeCache::X86Emit_SARIR(const x86Reg d, const uint8 n) { Emitter(*this).ShiftImm(Emitter::Shift::Sar, d, n, "SARIR register"); }
void NativeCodeCache::X86Emit_ROLIR(const x86Reg d, const uint8 n) { Emitter(*this).ShiftImm(Emitter::Shift::Rol, d, n, "ROLIR register"); }
void NativeCodeCache::X86Emit_RORIR(const x86Reg d, const uint8 n) { Emitter(*this).ShiftImm(Emitter::Shift::Ror, d, n, "RORIR register"); }

void NativeCodeCache::X86Emit_SHLRR(const x86Reg d) { Emitter(*this).ShiftCL(Emitter::Shift::Shl, d, "SHLRR register"); }
void NativeCodeCache::X86Emit_SHRRR(const x86Reg d) { Emitter(*this).ShiftCL(Emitter::Shift::Shr, d, "SHRRR register"); }
void NativeCodeCache::X86Emit_SARRR(const x86Reg d) { Emitter(*this).ShiftCL(Emitter::Shift::Sar, d, "SARRR register"); }
void NativeCodeCache::X86Emit_ROLRR(const x86Reg d) { Emitter(*this).ShiftCL(Emitter::Shift::Rol, d, "ROLRR register"); }
void NativeCodeCache::X86Emit_RORRR(const x86Reg d) { Emitter(*this).ShiftCL(Emitter::Shift::Ror, d, "RORRR register"); }

// [mem] <<= cl
void NativeCodeCache::X86Emit_SHLRM(const x86MemPtr ptrType, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  if(ptrType == x86MemPtr::x86MemPtr_qword) { A64_Unsupported("SHLRM qword"); return; }
  const uint32 size = (ptrType == x86MemPtr::x86MemPtr_byte) ? 1 : (ptrType == x86MemPtr::x86MemPtr_word) ? 2 : 4;
  const a64::Mem m = e.Mem(base, index, scale, disp, size);
  e.Load(kT0, m, size);
  e.a.lslv(kT0, kT0, W(HostRegId(1)));
  e.Store(kT0, m, size);
  FlagsUnknown();
}

// Rotate right through carry: the 33-bit value CF:reg turns right by 'count'.
// The handlers use it to move a NUON flag out of a register into the x86 carry
// for the ADC or SBB that follows, so the carry it leaves goes into kCarry.
void NativeCodeCache::X86Emit_RCRIR(const x86Reg regDest, const uint8 shiftCount)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest);
  if(!e.Check(d, "RCRIR register"))
    return;
  if(!d.Is32()) { A64_Unsupported("RCRIR sub-register"); return; }
  const uint32 count = shiftCount & 31;
  if(!count)
    return;

  // The carry going in. When nothing has defined it, x86 rotates in whatever
  // the previous instruction left; no handler can depend on that, so use zero.
  switch(g_flags.carry)
  {
    case Carry::Direct:     e.a.cset(kCarry, CondCode::kCS); break;
    case Carry::Inverted:   e.a.cset(kCarry, CondCode::kCC); break;
    case Carry::InRegister: break;
    default:                e.a.mov(kCarry, 0); break;
  }

  const a64::Gp w = W(d.id);
  e.a.mov(kT0, w);
  e.a.lsr(w, kT0, count);
  e.a.orr(w, w, kCarry, a64::lsl(32 - count));
  if(count > 1)
    e.a.orr(w, w, kT0, a64::lsl(33 - count));
  e.a.ubfx(kCarry, kT0, count - 1, 1);

  // x86 leaves SF and ZF as they were; the instructions above did not touch
  // NZCV either, but the carry now lives in the register.
  g_flags.carry = Carry::InRegister;
}

void NativeCodeCache::X86Emit_SHLDRRR(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  e.a.and_(kT1, W(HostRegId(1)), 31);
  e.DoubleShift(true, regDest, regSrc, &kT1, 0);
}

void NativeCodeCache::X86Emit_SHRDRRR(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  e.a.and_(kT1, W(HostRegId(1)), 31);
  e.DoubleShift(false, regDest, regSrc, &kT1, 0);
}

void NativeCodeCache::X86Emit_SHLDIRR(const x86Reg regDest, const x86Reg regSrc, const uint8 shiftCount)
{
  if(shiftCount & 31)
    Emitter(*this).DoubleShift(true, regDest, regSrc, nullptr, shiftCount & 31);
}

void NativeCodeCache::X86Emit_SHRDIRR(const x86Reg regDest, const x86Reg regSrc, const uint8 shiftCount)
{
  if(shiftCount & 31)
    Emitter(*this).DoubleShift(false, regDest, regSrc, nullptr, shiftCount & 31);
}

// Index of the highest set bit. For a zero source x86 sets ZF and leaves the
// destination alone, which the handlers rely on (CMOVZ follows).
void NativeCodeCache::X86Emit_BSRRR(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const GpOperand d = DecodeReg(regDest), s = DecodeReg(regSrc);
  if(!e.Check(d, "BSRRR register") || !e.Check(s, "BSRRR register"))
    return;
  if(!d.Is32() || !s.Is32()) { A64_Unsupported("BSRRR sub-register"); return; }
  e.a.clz(kT0, W(s.id));
  e.a.mov(kT1, 31);
  e.a.sub(kT1, kT1, kT0);
  e.a.tst(W(s.id), W(s.id));
  e.a.csel(W(d.id), W(d.id), kT1, CondCode::kEQ);
  g_flags = { false, true, false, Carry::Unknown };
}

// --- Conditions ---

void NativeCodeCache::X86Emit_SETOR(const x86Reg r)   { Emitter(*this).SetCC(X86_CC_O, r); }
void NativeCodeCache::X86Emit_SETNOR(const x86Reg r)  { Emitter(*this).SetCC(X86_CC_NO, r); }
void NativeCodeCache::X86Emit_SETBR(const x86Reg r)   { Emitter(*this).SetCC(X86_CC_B, r); }
void NativeCodeCache::X86Emit_SETNBR(const x86Reg r)  { Emitter(*this).SetCC(X86_CC_NB, r); }
void NativeCodeCache::X86Emit_SETZR(const x86Reg r)   { Emitter(*this).SetCC(X86_CC_Z, r); }
void NativeCodeCache::X86Emit_SETNZR(const x86Reg r)  { Emitter(*this).SetCC(X86_CC_NZ, r); }
void NativeCodeCache::X86Emit_SETBER(const x86Reg r)  { Emitter(*this).SetCC(X86_CC_BE, r); }
void NativeCodeCache::X86Emit_SETNBER(const x86Reg r) { Emitter(*this).SetCC(X86_CC_NBE, r); }
void NativeCodeCache::X86Emit_SETSR(const x86Reg r)   { Emitter(*this).SetCC(X86_CC_S, r); }
void NativeCodeCache::X86Emit_SETNSR(const x86Reg r)  { Emitter(*this).SetCC(X86_CC_NS, r); }
void NativeCodeCache::X86Emit_SETLR(const x86Reg r)   { Emitter(*this).SetCC(X86_CC_L, r); }
void NativeCodeCache::X86Emit_SETNLR(const x86Reg r)  { Emitter(*this).SetCC(X86_CC_NL, r); }
void NativeCodeCache::X86Emit_SETLER(const x86Reg r)  { Emitter(*this).SetCC(X86_CC_LE, r); }
void NativeCodeCache::X86Emit_SETNLER(const x86Reg r) { Emitter(*this).SetCC(X86_CC_NLE, r); }

void NativeCodeCache::X86Emit_CMOVORR(const x86Reg d, const x86Reg s)   { Emitter(*this).CMov(X86_CC_O, d, s); }
void NativeCodeCache::X86Emit_CMOVNORR(const x86Reg d, const x86Reg s)  { Emitter(*this).CMov(X86_CC_NO, d, s); }
void NativeCodeCache::X86Emit_CMOVBRR(const x86Reg d, const x86Reg s)   { Emitter(*this).CMov(X86_CC_B, d, s); }
void NativeCodeCache::X86Emit_CMOVNBRR(const x86Reg d, const x86Reg s)  { Emitter(*this).CMov(X86_CC_NB, d, s); }
void NativeCodeCache::X86Emit_CMOVZRR(const x86Reg d, const x86Reg s)   { Emitter(*this).CMov(X86_CC_Z, d, s); }
void NativeCodeCache::X86Emit_CMOVNZRR(const x86Reg d, const x86Reg s)  { Emitter(*this).CMov(X86_CC_NZ, d, s); }
void NativeCodeCache::X86Emit_CMOVBERR(const x86Reg d, const x86Reg s)  { Emitter(*this).CMov(X86_CC_BE, d, s); }
void NativeCodeCache::X86Emit_CMOVNBERR(const x86Reg d, const x86Reg s) { Emitter(*this).CMov(X86_CC_NBE, d, s); }
void NativeCodeCache::X86Emit_CMOVSRR(const x86Reg d, const x86Reg s)   { Emitter(*this).CMov(X86_CC_S, d, s); }
void NativeCodeCache::X86Emit_CMOVNSRR(const x86Reg d, const x86Reg s)  { Emitter(*this).CMov(X86_CC_NS, d, s); }
void NativeCodeCache::X86Emit_CMOVLRR(const x86Reg d, const x86Reg s)   { Emitter(*this).CMov(X86_CC_L, d, s); }
void NativeCodeCache::X86Emit_CMOVNLRR(const x86Reg d, const x86Reg s)  { Emitter(*this).CMov(X86_CC_NL, d, s); }
void NativeCodeCache::X86Emit_CMOVLERR(const x86Reg d, const x86Reg s)  { Emitter(*this).CMov(X86_CC_LE, d, s); }
void NativeCodeCache::X86Emit_CMOVNLERR(const x86Reg d, const x86Reg s) { Emitter(*this).CMov(X86_CC_NLE, d, s); }

// --- Vector moves ---
// The packet-start register snapshot (Emit_SaveRegs) copies the register file
// sixteen bytes at a time, so these come with the integer primitives.

void NativeCodeCache::X86Emit_MOVDQUMR(const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regDest);
  if(v == kNoReg) { A64_Unsupported("MOVDQUMR register"); return; }
  const a64::Mem m = e.Mem(base, index, scale, disp, 16);
  e.a.ldr(Q(v), m);
}

void NativeCodeCache::X86Emit_MOVDQURM(const x86Reg regSrc, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regSrc);
  if(v == kNoReg) { A64_Unsupported("MOVDQURM register"); return; }
  const a64::Mem m = e.Mem(base, index, scale, disp, 16);
  e.a.str(Q(v), m);
}

// AArch64 has no alignment requirement to honour, so these are the same loads and stores.
void NativeCodeCache::X86Emit_MOVDQAMR(const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  X86Emit_MOVDQUMR(regDest, base, index, scale, disp);
}

void NativeCodeCache::X86Emit_MOVDQARM(const x86Reg regSrc, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  X86Emit_MOVDQURM(regSrc, base, index, scale, disp);
}

void NativeCodeCache::X86Emit_MOVQMR(const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regDest);
  if(v == kNoReg) { A64_Unsupported("MOVQMR register"); return; }
  const a64::Mem m = e.Mem(base, index, scale, disp, 8);
  e.a.ldr(Q(v).d(), m);   // clears the upper half, as MOVQ does
}

void NativeCodeCache::X86Emit_MOVQRM(const x86Reg regSrc, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regSrc);
  if(v == kNoReg) { A64_Unsupported("MOVQRM register"); return; }
  const a64::Mem m = e.Mem(base, index, scale, disp, 8);
  e.a.str(Q(v).d(), m);
}

void NativeCodeCache::X86Emit_MOVSSMR(const x86Reg regDest, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regDest);
  if(v == kNoReg) { A64_Unsupported("MOVSSMR register"); return; }
  const a64::Mem m = e.Mem(base, index, scale, disp, 4);
  e.a.ldr(Q(v).s(), m);   // clears the upper 96 bits, as MOVSS from memory does
}

void NativeCodeCache::X86Emit_MOVDRM(const x86Reg regSrc, const uintptr_t base, const x86IndexReg index, const x86ScaleVal scale, const int32 disp)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regSrc);
  if(v == kNoReg) { A64_Unsupported("MOVDRM register"); return; }
  const a64::Mem m = e.Mem(base, index, scale, disp, 4);
  e.a.str(Q(v).s(), m);
}

// general register -> vector, clearing the rest of the vector
void NativeCodeCache::X86Emit_MOVDRR(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regDest);
  const GpOperand s = DecodeReg(regSrc);
  if(v == kNoReg || !s.IsValid() || !s.Is32()) { A64_Unsupported("MOVDRR register"); return; }
  e.a.fmov(Q(v).s(), W(s.id));
}

// vector -> general register
void NativeCodeCache::X86Emit_MOVDRR2(const x86Reg regDest, const x86Reg regSrc)
{
  Emitter e(*this);
  const uint32_t v = XmmId(regSrc);
  const GpOperand d = DecodeReg(regDest);
  if(v == kNoReg || !d.IsValid() || !d.Is32()) { A64_Unsupported("MOVDRR2 register"); return; }
  e.a.fmov(W(d.id), Q(v).s());
}

// --- Vector arithmetic ---

namespace {

const a64::Vec kV0 = a64::Vec::make_v128(24);   // vector scratch
const a64::Vec kV1 = a64::Vec::make_v128(25);   // kV1 and kV2 are consecutive: a two-register TBL needs that
const a64::Vec kV2 = a64::Vec::make_v128(26);

bool VecPair(NativeCodeCache &c, const x86Reg regDest, const x86Reg regSrc, uint32_t &d, uint32_t &s, const char * const what)
{
  d = XmmId(regDest);
  s = XmmId(regSrc);
  if(d == kNoReg || s == kNoReg)
  {
    c.A64_Unsupported(what);
    return false;
  }
  return true;
}

} // namespace

void NativeCodeCache::X86Emit_PANDRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "PANDRR register"))
    a64As->and_(Q(d).b16(), Q(d).b16(), Q(s).b16());
}

void NativeCodeCache::X86Emit_PADDRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "PADDRR register"))
    a64As->add(Q(d).s4(), Q(d).s4(), Q(s).s4());
}

void NativeCodeCache::X86Emit_PSUBDRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "PSUBDRR register"))
    a64As->sub(Q(d).s4(), Q(d).s4(), Q(s).s4());
}

void NativeCodeCache::X86Emit_PMULLDRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "PMULLDRR register"))
    a64As->mul(Q(d).s4(), Q(d).s4(), Q(s).s4());
}

// Horizontal add: [d0+d1, d2+d3, s0+s1, s2+s3]
void NativeCodeCache::X86Emit_PHADDDRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "PHADDDRR register"))
    a64As->addp(Q(d).s4(), Q(d).s4(), Q(s).s4());
}

// unpcklps: [d0, s0, d1, s1]
void NativeCodeCache::X86Emit_UNPCKLRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "UNPCKLRR register"))
    a64As->zip1(Q(d).s4(), Q(d).s4(), Q(s).s4());
}

// movlhps: the low half of src becomes the high half of dest
void NativeCodeCache::X86Emit_MOVLHRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "MOVLHRR register"))
    a64As->ins(Q(d).d(1), Q(s).d(0));
}

// pshufb. TBL yields zero for any index of 16 or more, where x86 only zeroes
// on bit 7 and otherwise uses the low four bits; the emitters' shuffle tables
// hold nothing but 0-15 and 0x80, for which the two agree.
void NativeCodeCache::X86Emit_PSHUFBRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(VecPair(*this, regDest, regSrc, d, s, "PSHUFBRR register"))
    a64As->tbl(Q(d).b16(), Q(d).b16(), Q(s).b16());
}

void NativeCodeCache::X86Emit_PSRADIR(const x86Reg regDest, const uint8 shiftCount)
{
  const uint32_t d = XmmId(regDest);
  if(d == kNoReg) { A64_Unsupported("PSRADIR register"); return; }
  if(shiftCount)
    a64As->sshr(Q(d).s4(), Q(d).s4(), shiftCount > 31 ? 31 : shiftCount);
}

void NativeCodeCache::X86Emit_PSLDIR(const x86Reg regDest, const uint8 shiftCount)
{
  const uint32_t d = XmmId(regDest);
  if(d == kNoReg) { A64_Unsupported("PSLDIR register"); return; }
  if(shiftCount > 31)
    a64As->eor(Q(d).b16(), Q(d).b16(), Q(d).b16());
  else if(shiftCount)
    a64As->shl(Q(d).s4(), Q(d).s4(), shiftCount);
}

// pslld by a count held in a vector register: every lane shifts by the count
// in the source's low lane, and a count over 31 clears the lane - which is
// also what USHL does with a shift of that size.
void NativeCodeCache::X86Emit_PSLDRR(const x86Reg regDest, const x86Reg regSrc)
{
  uint32_t d, s;
  if(!VecPair(*this, regDest, regSrc, d, s, "PSLDRR register"))
    return;
  a64::Assembler &a = *a64As;
  a.fmov(kT0, Q(s).s());
  a.dup(kV0.s4(), kT0);
  a.ushl(Q(d).s4(), Q(d).s4(), kV0.s4());
}

// shufps: lanes 0 and 1 come from dest, lanes 2 and 3 from src, each chosen by
// two bits of the immediate. Done as a byte table lookup over the pair.
void NativeCodeCache::X86Emit_SHUFIR(const x86Reg regDest, const x86Reg regSrc, const uint8 shiftCount)
{
  uint32_t d, s;
  if(!VecPair(*this, regDest, regSrc, d, s, "SHUFIR register"))
    return;
  a64::Assembler &a = *a64As;

  uint64_t index[2] = { 0, 0 };
  for(uint32_t lane = 0; lane < 4; lane++)
  {
    const uint32_t from = (shiftCount >> (lane * 2)) & 3;
    const uint32_t firstByte = from * 4 + ((lane >= 2 && d != s) ? 16 : 0);
    for(uint32_t b = 0; b < 4; b++)
      index[lane / 2] |= (uint64_t)(firstByte + b) << (((lane & 1) * 4 + b) * 8);
  }

  LoadImm64(a, kAddr, index[0]);
  LoadImm64(a, kImm, index[1]);
  a.fmov(kV0.d(), kAddr);
  a.ins(kV0.d(1), kImm);

  if(d == s)
    a.tbl(Q(d).b16(), Q(d).b16(), kV0.b16());
  else
  {
    a.mov(kV1.b16(), Q(d).b16());
    a.mov(kV2.b16(), Q(s).b16());
    a.tbl(Q(d).b16(), kV1.b16(), kV2.b16(), kV0.b16());
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
void NativeCodeCache::X86Emit_PUSHES() { A64_Unsupported("PUSHES"); }
void NativeCodeCache::X86Emit_POPES() { A64_Unsupported("POPES"); }
void NativeCodeCache::X86Emit_PUSHCS() { A64_Unsupported("PUSHCS"); }
void NativeCodeCache::X86Emit_ADCRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADCRM"); }
void NativeCodeCache::X86Emit_ADCIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("ADCIM"); }
void NativeCodeCache::X86Emit_PUSHSS() { A64_Unsupported("PUSHSS"); }
void NativeCodeCache::X86Emit_POPSS() { A64_Unsupported("POPSS"); }
void NativeCodeCache::X86Emit_SBBRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SBBRM"); }
void NativeCodeCache::X86Emit_SBBIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SBBIM"); }
void NativeCodeCache::X86Emit_PUSHDS() { A64_Unsupported("PUSHDS"); }
void NativeCodeCache::X86Emit_POPDS() { A64_Unsupported("POPDS"); }
void NativeCodeCache::X86Emit_ES() { A64_Unsupported("ES"); }
void NativeCodeCache::X86Emit_DAA() { A64_Unsupported("DAA"); }
void NativeCodeCache::X86Emit_CS() { A64_Unsupported("CS"); }
void NativeCodeCache::X86Emit_DAS() { A64_Unsupported("DAS"); }
void NativeCodeCache::X86Emit_SS() { A64_Unsupported("SS"); }
void NativeCodeCache::X86Emit_AAA() { A64_Unsupported("AAA"); }
void NativeCodeCache::X86Emit_CMPRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMPRM"); }
void NativeCodeCache::X86Emit_CMPIM(const int32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMPIM"); }
void NativeCodeCache::X86Emit_DS() { A64_Unsupported("DS"); }
void NativeCodeCache::X86Emit_AAS() { A64_Unsupported("AAS"); }
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
void NativeCodeCache::X86Emit_IMULIMR(const x86Reg, const int32, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("IMULIMR"); }
void NativeCodeCache::X86Emit_IMULMRR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("IMULMRR"); }
void NativeCodeCache::X86Emit_PUSHIB(const int8) { A64_Unsupported("PUSHIB"); }
void NativeCodeCache::X86Emit_INSB() { A64_Unsupported("INSB"); }
void NativeCodeCache::X86Emit_INSW() { A64_Unsupported("INSW"); }
void NativeCodeCache::X86Emit_INSD() { A64_Unsupported("INSD"); }
void NativeCodeCache::X86Emit_OUTSB() { A64_Unsupported("OUTSB"); }
void NativeCodeCache::X86Emit_OUTSW() { A64_Unsupported("OUTSW"); }
void NativeCodeCache::X86Emit_OUTSD() { A64_Unsupported("OUTSD"); }
void NativeCodeCache::X86Emit_JCC(uint8 *, const int8) { A64_Unsupported("JCC"); }
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
void NativeCodeCache::X86Emit_TESTRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("TESTRM"); }
void NativeCodeCache::X86Emit_TESTIM(const uint32, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("TESTIM"); }
void NativeCodeCache::X86Emit_XCHGRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("XCHGRM"); }
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
void NativeCodeCache::X86Emit_RCLIR(const x86Reg, const uint8) { A64_Unsupported("RCLIR"); }
void NativeCodeCache::X86Emit_SHLDIMR(const x86Reg, const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHLDIMR"); }
void NativeCodeCache::X86Emit_SHRDIMR(const x86Reg, const x86MemPtr, const uint8, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHRDIMR"); }
void NativeCodeCache::X86Emit_SHLDRMR(const x86Reg, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHLDRMR"); }
void NativeCodeCache::X86Emit_SHRDRMR(const x86Reg, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHRDRMR"); }
void NativeCodeCache::X86Emit_SALIR(const x86Reg, const uint8) { A64_Unsupported("SALIR"); }
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
void NativeCodeCache::X86Emit_SHRRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SHRRM"); }
void NativeCodeCache::X86Emit_SALRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SALRM"); }
void NativeCodeCache::X86Emit_SARRM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SARRM"); }
void NativeCodeCache::X86Emit_LEAVE() { A64_Unsupported("LEAVE"); }
void NativeCodeCache::X86Emit_RETF(uint16) { A64_Unsupported("RETF"); }
void NativeCodeCache::X86Emit_INT3() { A64_Unsupported("INT3"); }
void NativeCodeCache::X86Emit_INT(int8) { A64_Unsupported("INT"); }
void NativeCodeCache::X86Emit_INTO() { A64_Unsupported("INTO"); }
void NativeCodeCache::X86Emit_IRET() { A64_Unsupported("IRET"); }
void NativeCodeCache::X86Emit_RCLRR(const x86Reg) { A64_Unsupported("RCLRR"); }
void NativeCodeCache::X86Emit_RCRRR(const x86Reg) { A64_Unsupported("RCRRR"); }
void NativeCodeCache::X86Emit_SALRR(const x86Reg) { A64_Unsupported("SALRR"); }
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
void NativeCodeCache::X86Emit_NOTM(const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("NOTM"); }
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
void NativeCodeCache::X86Emit_SETOM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETOM"); }
void NativeCodeCache::X86Emit_SETNOM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNOM"); }
void NativeCodeCache::X86Emit_SETBM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETBM"); }
void NativeCodeCache::X86Emit_SETNBM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNBM"); }
void NativeCodeCache::X86Emit_SETZM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETZM"); }
void NativeCodeCache::X86Emit_SETNZM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNZM"); }
void NativeCodeCache::X86Emit_SETBEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETBEM"); }
void NativeCodeCache::X86Emit_SETNBEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNBEM"); }
void NativeCodeCache::X86Emit_SETSM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETSM"); }
void NativeCodeCache::X86Emit_SETNSM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNSM"); }
void NativeCodeCache::X86Emit_SETPR(const x86Reg) { A64_Unsupported("SETPR"); }
void NativeCodeCache::X86Emit_SETPM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETPM"); }
void NativeCodeCache::X86Emit_SETNPR(const x86Reg) { A64_Unsupported("SETNPR"); }
void NativeCodeCache::X86Emit_SETNPM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNPM"); }
void NativeCodeCache::X86Emit_SETLM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETLM"); }
void NativeCodeCache::X86Emit_SETNLM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNLM"); }
void NativeCodeCache::X86Emit_SETLEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETLEM"); }
void NativeCodeCache::X86Emit_SETNLEM(const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("SETNLEM"); }
void NativeCodeCache::X86Emit_CMOVOMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVOMR"); }
void NativeCodeCache::X86Emit_CMOVNOMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNOMR"); }
void NativeCodeCache::X86Emit_CMOVBMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVBMR"); }
void NativeCodeCache::X86Emit_CMOVNBRR64(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNBRR64"); }
void NativeCodeCache::X86Emit_CMOVNBMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNBMR"); }
void NativeCodeCache::X86Emit_CMOVZMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVZMR"); }
void NativeCodeCache::X86Emit_CMOVNZMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNZMR"); }
void NativeCodeCache::X86Emit_CMOVBEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVBEMR"); }
void NativeCodeCache::X86Emit_CMOVNBEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNBEMR"); }
void NativeCodeCache::X86Emit_CMOVSMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVSMR"); }
void NativeCodeCache::X86Emit_CMOVNSMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNSMR"); }
void NativeCodeCache::X86Emit_CMOVPRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVPRR"); }
void NativeCodeCache::X86Emit_CMOVPMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVPMR"); }
void NativeCodeCache::X86Emit_CMOVNPRR(const x86Reg, const x86Reg) { A64_Unsupported("CMOVNPRR"); }
void NativeCodeCache::X86Emit_CMOVNPMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNPMR"); }
void NativeCodeCache::X86Emit_CMOVLMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVLMR"); }
void NativeCodeCache::X86Emit_CMOVNLMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNLMR"); }
void NativeCodeCache::X86Emit_CMOVLEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVLEMR"); }
void NativeCodeCache::X86Emit_CMOVNLEMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("CMOVNLEMR"); }
void NativeCodeCache::X86Emit_MOVSXRR(const x86Reg, const x86Reg) { A64_Unsupported("MOVSXRR"); }
void NativeCodeCache::X86Emit_MOVSXMR(const x86Reg, const x86MemPtr, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("MOVSXMR"); }
void NativeCodeCache::X86Emit_PSRADRR(const x86Reg, const x86Reg) { A64_Unsupported("PSRADRR"); }
void NativeCodeCache::X86Emit_PSRLDRR(const x86Reg, const x86Reg) { A64_Unsupported("PSRLDRR"); }
void NativeCodeCache::X86Emit_PSRLDIR(const x86Reg, const uint8) { A64_Unsupported("PSRLDIR"); }
void NativeCodeCache::X86Emit_DPPSRR(const x86Reg, const x86Reg, const uint8) { A64_Unsupported("DPPSRR"); }
void NativeCodeCache::X86Emit_MOVQRR(const x86Reg, const x86Reg) { A64_Unsupported("MOVQRR"); }
void NativeCodeCache::X86Emit_PANDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDMR"); }
void NativeCodeCache::X86Emit_PANDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDRM"); }
void NativeCodeCache::X86Emit_PANDBRR(const x86Reg, const x86Reg) { A64_Unsupported("PANDBRR"); }
void NativeCodeCache::X86Emit_PANDNMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDNMR"); }
void NativeCodeCache::X86Emit_PANDNRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PANDNRM"); }
void NativeCodeCache::X86Emit_PSUBDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PSUBDMR"); }
void NativeCodeCache::X86Emit_PSUBDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PSUBDRM"); }
void NativeCodeCache::X86Emit_PADDMR(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PADDMR"); }
void NativeCodeCache::X86Emit_PADDRM(const x86Reg, const uintptr_t, const x86IndexReg, const x86ScaleVal, const int32) { A64_Unsupported("PADDRM"); }
void NativeCodeCache::X86Emit_EMMS() { A64_Unsupported("EMMS"); }
void NativeCodeCache::X86Emit_FEMMS() { A64_Unsupported("FEMMS"); }

volatile unsigned a64DebugBlockPc = 0;
volatile unsigned a64DebugBlockMpe = 0;
volatile unsigned a64DebugBlockRunning = 0;
unsigned a64ChainPackets = 32;
unsigned long long a64PerfNativeBlocks[4], a64PerfInterpreted[4], a64PerfNeverCompile[4];

// --- Which packets the interpreter still runs ---

bool a64PerfSampleInterpreted = false;

namespace {

constexpr unsigned kInterpTableSize = 4096; // open addressing, power of two
A64InterpretedPacket g_interpTable[kInterpTableSize];

} // namespace

void A64_NoteInterpretedPacket(const unsigned mpeIndex, const unsigned address, const unsigned packetInfo)
{
  unsigned slot = ((address >> 1) * 2654435761u + mpeIndex) & (kInterpTableSize - 1);
  for(unsigned probe = 0; probe < 16; probe++, slot = (slot + 1) & (kInterpTableSize - 1))
  {
    A64InterpretedPacket &entry = g_interpTable[slot];
    if(entry.count && (entry.address != address || entry.mpeIndex != mpeIndex))
      continue;
    entry.mpeIndex = mpeIndex;
    entry.address = address;
    entry.packetInfo = packetInfo;
    entry.count++;
    return;
  }
  // Table crowded around here: the packet goes uncounted, which only matters
  // for ones too rare to make the list anyway.
}

unsigned A64_TopInterpretedPackets(A64InterpretedPacket * const out, const unsigned maxEntries)
{
  unsigned found = 0;
  for(; found < maxEntries; found++)
  {
    unsigned best = kInterpTableSize;
    for(unsigned i = 0; i < kInterpTableSize; i++)
    {
      if(g_interpTable[i].count && (best == kInterpTableSize || g_interpTable[i].count > g_interpTable[best].count))
        best = i;
    }
    if(best == kInterpTableSize)
      break;
    out[found] = g_interpTable[best];
    g_interpTable[best].count = 0;
  }
  memset(g_interpTable, 0, sizeof(g_interpTable));
  return found;
}

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
  cache.X86Emit_EMMS(); // MMX: nothing on AArch64 corresponds, so this one stays unimplemented
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
