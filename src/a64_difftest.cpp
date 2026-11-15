// Differential test for the Arm64 JIT backend.
//
// Writes short runs of random NUON packets into main-bus RAM and executes each
// run twice from the same random machine state: once through the interpreter
// and once as a natively compiled block, both by way of the real dispatcher
// (MPE::FetchDecodeExecute). Any difference afterwards - in the 48 registers,
// in where execution ended up, or in the memory the run touched - is a bug in
// the backend, or now and then a place where the emitters and the interpreter
// disagree by design, which shows up on x86 too and is for a person to judge.
//
// Covered: ALU, MUL and RCU instructions, memory instructions on main-bus RAM
// (loads, stores, pixel and bilinear addressing, push and pop), branches with
// their delay slots, and loads and stores of the control registers that are
// plain state. Not covered: the other control registers, which have side
// effects on the rest of the machine.
//
// Runs that cannot be compiled natively are counted, with the reason, so the
// output also says what the backend lacks.
//
// Started by NUANCE_A64_DIFFTEST=<trials>[,<seed>] in the environment, from
// retro_load_game. tools/a64_difftest_host.c loads the core without a
// frontend so this can run from a shell.
#ifdef USE_A64_JIT

#include "basetypes.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include "ExecuteMEM.h"
#include "Handlers.h"
#include "InstructionCache.h"
#include "mpe.h"
#include "NativeCodeCache.h"
#include "NuonEnvironment.h"
#include "NuonMemoryMap.h"
#include "SuperBlock.h"

extern NuonEnvironment nuonEnv;
extern NuancePrintHandler printHandlers[];
extern NativeEmitHandler emitHandlers[];
extern void (*a64MemoryTraceHook)(uint32 address, void *pointer);
void libretro_log_info(const char* text);

namespace {

// Test code lives at kCodeBase. Data the code may touch lives in a window
// well away from it; a run that reaches anywhere else is not used.
constexpr uint32 kCodeBase = MAIN_BUS_BASE + 0x00400000U;
constexpr uint32 kCodeBytes = 512;
constexpr uint32 kDataBase = MAIN_BUS_BASE + 0x00500000U;
constexpr uint32 kDataBytes = 0x00020000U;
constexpr uint32 kDataMargin = 0x1000U;       // accesses near an address spill this far at most
constexpr uint32 kMaxPacketsPerRun = 4;
constexpr uint32 kTraceBytes = 256;           // saved around each traced pointer
constexpr uint32 kTraceBefore = 64;

// Packets left out: anything the decoders mark as never compilable. Control
// register access is let through only for registers that are plain state
// (see IsSafeControlRegisterAccess).
constexpr uint32 kRejectMask =
  PACKETINFO_NEVERCOMPILE | PACKETINFO_EXCEPTION | PACKETINFO_BREAKPOINT;

constexpr uint32 kBranchMask = PACKETINFO_BRANCH_ALWAYS | PACKETINFO_BRANCH_CONDITIONAL | PACKETINFO_BRANCH_NOP;

struct Rng
{
  uint64 s;
  uint32 Next()
  {
    // xorshift64*
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return (uint32)((s * 0x2545F4914F6CDD1DULL) >> 32);
  }
  uint32 Below(const uint32 n) { return Next() % n; }
  // Somewhere in the data window, word-aligned more often than not.
  uint32 DataAddress()
  {
    const uint32 offset = kDataMargin + Below(kDataBytes - 2 * kDataMargin);
    return kDataBase + (Below(4) ? (offset & ~3U) : offset);
  }
  // Register values: random, with edge cases and usable addresses over-represented.
  uint32 Value()
  {
    static const uint32 edges[] = { 0, 1, 2, 0xFFFFFFFFU, 0x80000000U, 0x7FFFFFFFU, 0x0000FFFFU, 0xFFFF0000U, 0x00008000U, 0x00010000U, 31, 32, 33 };
    const uint32 pick = Below(10);
    if(pick == 0) return edges[Below(sizeof(edges)/sizeof(edges[0]))];
    if(pick == 1) return Below(64);
    if(pick == 2) return (uint32)(-(int32)Below(64));
    if(pick < 6)  return DataAddress();
    // Not the control registers: a load or store through a register that
    // happens to point there would reconfigure the machine under the test.
    const uint32 value = Next();
    return (value >= MPE_DTAGS_BASE && value < MAIN_BUS_BASE) ? DataAddress() : value;
  }
};

void Log(const char * const fmt, ...)
{
  char line[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  libretro_log_info(line);
}

// Everything about the MPE a run can change and the test compares or restores.
struct MachineState
{
  uint32 regs[48];
  uint32 pcexec;
  uint32 sp;
  uint32 xybase, uvbase, linpixctl, clutbase;
};

void RandomState(Rng &rng, MachineState &state)
{
  for(int i = 0; i < 32; i++)
  {
    state.regs[i] = rng.Value();
    // Any register can be a multiply's shift count, and a count field of 0x40
    // is the undefined left-shift-by-64 (see IsOutOfSpec).
    if((state.regs[i] & 0x7FU) == 0x40U)
      state.regs[i] ^= 1;
  }
  state.regs[32] = rng.Next() & CC_ALLFLAGS;     // cc
  state.regs[33] = rng.Value() & 0xFFFFU;        // rc0
  state.regs[34] = rng.Value() & 0xFFFFU;        // rc1
  // rx ry ru rv: 16.16 pixel coordinates, small so bilinear addresses stay in the window
  for(int i = 35; i < 39; i++)
    state.regs[i] = (rng.Below(32) << 16) | (rng.Next() & 0xFFFFU);
  state.regs[39] = rng.Value();                  // rz
  state.regs[40] = rng.Value();                  // rzi1
  state.regs[41] = rng.Value();                  // rzi2
  state.regs[42] = rng.Next();                   // xyctl
  state.regs[43] = rng.Next();                   // uvctl
  state.regs[44] = rng.Value();                  // xyrange
  state.regs[45] = rng.Value();                  // uvrange
  state.regs[46] = rng.Next() & 0x7FU;           // acshift
  if(state.regs[46] == 0x40U)
    state.regs[46] = 0x41U;
  state.regs[47] = rng.Next() & 0x3U;            // svshift

  state.pcexec = kCodeBase;
  // The stack is always in the MPE's own data RAM, whatever sp's upper bits say.
  state.sp = MPE_DTRAM_BASE + 0x00010000U + ((rng.Next() & 0xFFFFU) & ~15U);
  state.xybase = rng.DataAddress() & ~3U;
  state.uvbase = rng.DataAddress() & ~3U;
  state.clutbase = rng.DataAddress() & ~0x3FFU;
  state.linpixctl = rng.Next();
}

void ApplyState(MPE &mpe, const MachineState &state)
{
  memcpy(mpe.reg_union, state.regs, sizeof(state.regs));
  memcpy(mpe.tempreg_union, state.regs, sizeof(state.regs));
  mpe.pcexec = state.pcexec;
  mpe.sp = state.sp;
  mpe.xybase = state.xybase;
  mpe.uvbase = state.uvbase;
  mpe.clutbase = state.clutbase;
  mpe.linpixctl = state.linpixctl;
  // Left behind by the last bilinear address calculation and read by the
  // pixel loads and stores; an absolute-address pixel access sees whatever
  // was there, so make that the same thing for both executions.
  mpe.ba_reg_offset = 0;
  mpe.ba_control = state.linpixctl;
  mpe.ba_mipped_xoffset = 0;
  mpe.mpectl |= MPECTRL_MPEGO;
  mpe.intsrc = 0;
  mpe.excepsrc = 0;
  mpe.ecuSkipCounter = 0;
  mpe.interpretNextPacket = 0;
  mpe.pcfetchnext = 0;
  mpe.bInvalidateInstructionCaches = false;
  mpe.bInvalidateInterpreterCache = false;
  mpe.invalidateRegionStart = 0xFFFFFFFFU;
  mpe.invalidateRegionEnd = 0;
  mpe.interpreterInvalidateRegionStart = 0xFFFFFFFFU;
  mpe.interpreterInvalidateRegionEnd = 0;
}

void CaptureState(const MPE &mpe, MachineState &state)
{
  memcpy(state.regs, mpe.reg_union, sizeof(state.regs));
  state.pcexec = mpe.pcexec;
  state.sp = mpe.sp;
  state.xybase = mpe.xybase;
  state.uvbase = mpe.uvbase;
  state.clutbase = mpe.clutbase;
  state.linpixctl = mpe.linpixctl;
}

// Memory the interpreter reached, recorded as it goes: where, and what was
// there before. Lets the test put memory back between the two executions and
// compare what each left behind.
struct TraceEntry
{
  uint8 *start;
  uint8 before[kTraceBytes];
  uint8 afterInterpreter[kTraceBytes];
};

std::vector<TraceEntry> g_trace;
bool g_traceOutsideWindow = false;

// Remembers what is around 'pointer' before the run changes it. Overlapping
// entries are fine: they are restored in reverse order, so the oldest image
// of any byte is the one that ends up back in memory.
void TracePointer(uint8 * const pointer)
{
  uint8 * const start = pointer - kTraceBefore;
  for(const TraceEntry &entry : g_trace)
  {
    if(entry.start == start)
      return;
  }
  g_trace.emplace_back();
  g_trace.back().start = start;
  memcpy(g_trace.back().before, start, kTraceBytes);
}

// Memory a run may touch: the data window, and the MPE's own data and
// instruction RAM, which is where the short absolute addressing forms point.
bool IsTestMemory(const uint32 address)
{
  if(address >= kDataBase + kDataMargin && address < kDataBase + kDataBytes - kDataMargin)
    return true;
  return address >= MPE_DTRAM_BASE + kDataMargin && address < MPE_DTAGS_BASE - kDataMargin;
}

void TraceHook(const uint32 address, void * const pointer)
{
  if(!pointer || !IsTestMemory(address))
  {
    g_traceOutsideWindow = true;
    return;
  }
  TracePointer((uint8 *)pointer);
}

// The absolute-address forms carry a host pointer worked out by the decoder,
// so the interpreter never asks where their memory is and the trace would
// miss them. These are the handlers that use that pointer.
bool UsesDecodedPointer(const uint32 handler)
{
  switch(handler)
  {
    case Handler_LoadByteAbsolute: case Handler_LoadWordAbsolute: case Handler_LoadScalarAbsolute:
    case Handler_LoadShortVectorAbsolute: case Handler_LoadVectorAbsolute:
    case Handler_LoadPixelAbsolute: case Handler_LoadPixelZAbsolute:
    case Handler_StoreScalarAbsolute: case Handler_StoreShortVectorAbsolute: case Handler_StoreVectorAbsolute:
    case Handler_StorePixelAbsolute: case Handler_StorePixelZAbsolute:
      return true;
    default:
      return false;
  }
}

// Pointers decoded into the packets of the run being built.
std::vector<uint8 *> g_runPointers;

void RestoreTracedMemory()
{
  for(size_t i = g_trace.size(); i > 0; i--)
    memcpy(g_trace[i-1].start, g_trace[i-1].before, kTraceBytes);
}

// One random packet: up to three instructions that do not end the packet,
// then one that does. Lengths and the end-of-packet bit follow the encoding
// (see MPE::GetPacketDelta): a first byte up to 0x3F is a 16-bit ALU
// instruction and always ends the packet; 0x88-0x8F is a 32-bit extension to
// the instruction after it; 0x90-0x9F starts a 32-bit instruction, which ends
// the packet if bit 4 of its third byte is set; anything else is 16 bits and
// ends the packet if its top bit is set. 0x80-0x83 are PAD, NOP and BREAKPOINT.
uint32 RandomPacketBytes(Rng &rng, uint8 * const packet, uint32 &numInstructions)
{
  const uint32 numLeading = (rng.Below(3) == 0) ? rng.Below(4) : 0;
  uint32 length = 0;
  numInstructions = numLeading + 1;

  for(uint32 n = 0; n <= numLeading; n++)
  {
    const bool bLast = (n == numLeading);
    uint8 * const p = packet + length;
    for(int i = 0; i < 8; i++)
      p[i] = (uint8)rng.Next();

    const uint32 kind = rng.Below(10);
    if(kind < 4)
    {
      // 16 bits
      if(bLast && rng.Below(2))
        p[0] &= 0x3F;                               // ALU
      else
      {
        p[0] = 0x40 | (p[0] & 0x3F);                // another unit
        if(bLast)
          p[0] |= 0x80;
        if(p[0] >= 0x80 && p[0] <= 0x9F)
          p[0] |= 0x20;                             // clear of the control codes and longer forms
      }
      length += 2;
    }
    else if(kind < 8)
    {
      p[0] = 0x90 | (p[0] & 0x0F);                  // 32 bits
      p[2] = bLast ? (p[2] | 0x10) : (p[2] & ~0x10);
      length += 4;
    }
    else if(kind < 9)
    {
      p[0] = 0x88 | (p[0] & 0x07);                  // 32-bit extension, then a 32-bit instruction
      p[4] = 0x90 | (p[4] & 0x0F);
      p[6] = bLast ? (p[6] | 0x10) : (p[6] & ~0x10);
      length += 8;
    }
    else
    {
      p[0] = 0x88 | (p[0] & 0x07);                  // 32-bit extension, then a 16-bit instruction
      if(bLast && rng.Below(2))
        p[4] &= 0x3F;
      else
      {
        p[4] = 0x40 | (p[4] & 0x3F);
        if(bLast)
          p[4] |= 0x80;
        if(p[4] >= 0x80 && p[4] <= 0x9F)
          p[4] |= 0x20;
      }
      length += 6;
    }
  }
  return length;
}

// Encodings the hardware documentation does not define, where the emitters
// and the interpreter each do something different and neither is wrong:
//  - a multiply whose shift field asks for a left shift by 64, which C++
//    leaves undefined in the interpreter and the emitters compute as zero;
//  - butt (butterfly) with r31 as destination, whose second result would land
//    in "r32", the condition codes.
bool IsOutOfSpec(const InstructionCacheEntry &entry, const uint32 slot)
{
  char text[512];
  (printHandlers[entry.handlers[slot]])(text, sizeof(text), (Nuance &)(entry.nuances[FIXED_FIELD(slot,0)]), false);
  if(strstr(text, ">>#-64"))
    return true;
  if(!strncmp(text, "butt ", 5))
  {
    const char * const lastComma = strrchr(text, ',');
    if(lastComma && !strncmp(lastComma, ", r31", 5))
      return true;
  }
  return false;
}

// The byte, word, scalar and vector forms of (xy)/(uv) addressing are only
// defined for an address aligned to what they access, under a pixel type with
// a whole number of bytes per pixel. Outside that the interpreter and the
// emitters each do something of their own (the interpreter shifts by a
// negative count, which C++ leaves undefined). Returns whether instruction
// 'slot' of the packet is such a case in the machine's current state.
bool IsUndefinedBilinearAccess(MPE &mpe, const InstructionCacheEntry &entry, const uint32 slot)
{
  uint32 size;
  bool bUV;
  switch(entry.handlers[slot])
  {
    case Handler_LoadByteBilinearUV:         size = 1;  bUV = true;  break;
    case Handler_LoadByteBilinearXY:         size = 1;  bUV = false; break;
    case Handler_LoadWordBilinearUV:         size = 2;  bUV = true;  break;
    case Handler_LoadWordBilinearXY:         size = 2;  bUV = false; break;
    case Handler_LoadScalarBilinearUV:
    case Handler_StoreScalarBilinearUV:      size = 4;  bUV = true;  break;
    case Handler_LoadScalarBilinearXY:
    case Handler_StoreScalarBilinearXY:      size = 4;  bUV = false; break;
    case Handler_LoadShortVectorBilinearUV:
    case Handler_StoreShortVectorBilinearUV: size = 8;  bUV = true;  break;
    case Handler_LoadShortVectorBilinearXY:
    case Handler_StoreShortVectorBilinearXY: size = 8;  bUV = false; break;
    case Handler_LoadVectorBilinearUV:
    case Handler_StoreVectorBilinearUV:      size = 16; bUV = true;  break;
    case Handler_LoadVectorBilinearXY:
    case Handler_StoreVectorBilinearXY:      size = 16; bUV = false; break;
    default: return false;
  }

  const uint32 control = bUV ? mpe.uvctl : mpe.xyctl;
  if(BilinearInfo_XYType(control) < 2)
    return true; // MPEG and 4-bit pixels: less than a byte each
  const uint32 x = bUV ? mpe.ru : mpe.rx, y = bUV ? mpe.rv : mpe.ry;
  const uint32 address = ((bUV ? mpe.uvbase : mpe.xybase) & 0xFFFFFFFCU) + GetBilinearAddress((x & 0xFFFF0000U) | (y >> 16), control);
  return (address & (size - 1)) != 0;
}

// Control registers that are nothing but state the test already sets up and
// compares: the index, counter and shift registers of the register file, and
// the texture and palette base addresses. The rest start DMA, talk to the
// other processors, or change how the machine runs.
bool IsSafeControlRegister(const uint32 address)
{
  switch((address & 0x1FF0) >> 4)
  {
    case 0x04: // cc
    case 0x08: case 0x09: case 0x0A: // rz rzi1 rzi2
    case 0x1E: case 0x1F: // rc0 rc1
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: // rx ry xyrange xybase xyctl
    case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: // ru rv uvrange uvbase uvctl
    case 0x2B: case 0x2C: case 0x2D: // clutbase svshift acshift
      return (address & 0xFFFFE000U) == MPE_CTRL_BASE;
    default:
      return false;
  }
}

// Whether every control-register access in the packet is to such a register.
bool IsSafeControlRegisterAccess(const InstructionCacheEntry &entry)
{
  if(!(entry.packetInfo & PACKETINFO_MEMORY_IO))
    return true;
  bool bFound = false;
  for(uint32 i = 0; i < entry.nuanceCount; i++)
  {
    const size_t * const fields = &entry.nuances[FIXED_FIELD(i,0)];
    switch(entry.handlers[i])
    {
      case Handler_LoadScalarControlRegisterAbsolute:
        if(!IsSafeControlRegister((uint32)fields[FIELD_MEM_FROM])) return false;
        bFound = true;
        break;
      case Handler_StoreScalarControlRegisterImmediate:
        // A constant written to cc with either coprocessor flag set: constant
        // propagation deliberately drops those two bits and the interpreter
        // keeps them. Not something a program does.
        if(((((uint32)fields[FIELD_MEM_TO]) & 0x1FF0) >> 4) == 0x04 && (fields[FIELD_MEM_FROM] & (CC_COPROCESSOR0 | CC_COPROCESSOR1)))
          return false;
        // fall through
      case Handler_StoreScalarControlRegisterAbsolute:
        if(!IsSafeControlRegister((uint32)fields[FIELD_MEM_TO])) return false;
        bFound = true;
        break;
      case Handler_LoadVectorControlRegisterAbsolute:
      case Handler_StoreVectorControlRegisterAbsolute:
        return false;
      default:
        break;
    }
  }
  // Flagged as control-register access but by none of the handlers above:
  // something this function does not know about, so leave it out.
  return bFound;
}

bool DecodeAt(MPE &mpe, uint8 * const mem, const uint32 address, InstructionCacheEntry &entry, const uint32 options)
{
  entry.pcexec = address;
  mpe.DecompressPacket(mem + (address - kCodeBase), entry, options);
  return entry.pcroute > address;
}

std::string Disassemble(MPE &mpe, uint8 * const mem, const uint32 start, const uint32 end)
{
  std::string text;
  uint32 pc = start;
  while(pc < end)
  {
    InstructionCacheEntry entry;
    if(!DecodeAt(mpe, mem, pc, entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST))
      break;
    text += "{";
    for(uint32 i = 0; i < entry.nuanceCount; i++)
    {
      char buf[512];
      (printHandlers[entry.handlers[i]])(buf, sizeof(buf), (Nuance &)(entry.nuances[FIXED_FIELD(i,0)]), false);
      for(char *c = buf; *c; c++) if(*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
      if(i) text += " ; ";
      text += buf;
    }
    text += "} ";
    pc = entry.pcroute;
  }
  return text;
}

const char * const kRegNames[48] = {
  "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","r13","r14","r15",
  "r16","r17","r18","r19","r20","r21","r22","r23","r24","r25","r26","r27","r28","r29","r30","r31",
  "cc","rc0","rc1","rx","ry","ru","rv","rz","rzi1","rzi2","xyctl","uvctl","xyrange","uvrange","acshift","svshift" };

// Tries to place one acceptable packet at 'cursor'. Returns its length, or 0.
uint32 PlacePacket(Rng &rng, MPE &mpe, uint8 * const mem, const uint32 cursor, const bool bAllowBranch, uint32 &packetInfo)
{
  for(int attempt = 0; attempt < 2000; attempt++)
  {
    uint32 numInstructions = 0;
    const uint32 length = RandomPacketBytes(rng, mem + (cursor - kCodeBase), numInstructions);
    InstructionCacheEntry entry;
    // Absolute addresses are resolved to host pointers while decoding, so the
    // trace sees them here: a packet naming memory outside the test's is out.
    g_traceOutsideWindow = false;
    a64MemoryTraceHook = TraceHook;
    const bool bDecoded = DecodeAt(mpe, mem, cursor, entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST | DECOMPRESS_OPTIONS_SCHEDULE_MEM_FIRST);
    a64MemoryTraceHook = nullptr;
    if(!bDecoded || g_traceOutsideWindow)
      continue;

    // One instruction per execution unit: a packet with two for the same unit
    // is not a packet the hardware accepts, and the decoder keeps the flags of
    // both but the operation of only one. Such packets decode to fewer
    // operations than they have instructions, as do ones with a no-op in them.
    bool bAccept = !(entry.packetInfo & kRejectMask) && entry.pcroute == cursor + length && entry.nuanceCount == numInstructions
      && IsSafeControlRegisterAccess(entry);
    // And no two instructions writing the same register or flag: which of the
    // two results survives is not defined.
    for(uint32 i = 0; i < entry.nuanceCount && bAccept; i++)
    {
      for(uint32 j = i + 1; j < entry.nuanceCount && bAccept; j++)
      {
        if((entry.scalarOutputDependencies[i] & entry.scalarOutputDependencies[j]) ||
           (entry.miscOutputDependencies[i] & entry.miscOutputDependencies[j]))
          bAccept = false;
      }
    }
    // Vector operands name a register that is a multiple of four. The decoder
    // works out dependencies as if the low two bits were not there, while the
    // interpreter and the emitters both use them, so a misaligned one makes
    // the two disagree about which registers an instruction touches. A field
    // counts as such an operand when it is not a multiple of four and the
    // instruction declares at least three registers of the group it falls in.
    // (A scalar that happens to sit inside another operand's vector is turned
    // down with it; there are plenty of others.)
    for(uint32 i = 0; i < entry.nuanceCount && bAccept; i++)
    {
      const uint32 declared = entry.scalarInputDependencies[i] | entry.scalarOutputDependencies[i];
      for(uint32 field = 1; field < FIELDS_PER_NUANCE && bAccept; field++)
      {
        const size_t value = entry.nuances[FIXED_FIELD(i,field)];
        if(value < 32 && (value & 3) && ((declared >> (value & 0x1C)) & 0x7) == 0x7)
          bAccept = false;
      }
    }
    if(bAccept && (entry.packetInfo & (kBranchMask | PACKETINFO_ECU)) && !bAllowBranch)
      bAccept = false;
    // A slot the decoder claimed but did not fill reads as an ECU NOP: not a
    // real instruction, so not worth a trial.
    for(uint32 i = 0; i < entry.nuanceCount && bAccept; i++)
      bAccept = (entry.handlers[i] != Handler_ECU_NOP) && !IsOutOfSpec(entry, i);

    // An absolute form must point into the MPE's data or instruction RAM. An
    // encoding the decoder gave no pointer at all would crash the interpreter.
    uint8 *pointers[MAX_INSTRUCTIONS_PER_PACKET];
    uint32 numPointers = 0;
    for(uint32 i = 0; i < entry.nuanceCount && bAccept; i++)
    {
      if(!UsesDecodedPointer(entry.handlers[i]))
        continue;
      uint8 * const pointer = (uint8 *)entry.nuances[FIXED_FIELD(i,FIELD_MEM_POINTER)];
      // (From the very start of data RAM: the short forms reach only its first 2K.)
      const uint8 * const lo = &mpe.dtrom[MPE_DTRAM_BASE & MPE_VALID_MEMORY_MASK];
      const uint8 * const hi = &mpe.dtrom[(MPE_DTAGS_BASE - kDataMargin) & MPE_VALID_MEMORY_MASK];
      if(pointer < lo || pointer >= hi)
        bAccept = false;
      else
        pointers[numPointers++] = pointer;
    }

    if(bAccept)
    {
      for(uint32 i = 0; i < numPointers; i++)
        g_runPointers.push_back(pointers[i]);
      packetInfo = entry.packetInfo;
      return length;
    }
  }
  return 0;
}

} // namespace

void A64_DiffTest(const unsigned trials, const unsigned seed)
{
  MPE &mpe = nuonEnv.mpe[0];
  uint8 * const mem = (uint8 *)nuonEnv.GetPointerToMemory(mpe.mpeIndex, kCodeBase, false);
  uint8 * const data = (uint8 *)nuonEnv.GetPointerToMemory(mpe.mpeIndex, kDataBase, false);
  if(!mem || !data || !nuonEnv.compilerOptions.CanCompile())
  {
    Log("Arm64 JIT difftest: cannot run (no test memory, or the native emitter is off)");
    return;
  }

  // HALT cannot be compiled, so it ends every run: search the 16-bit
  // instructions for its encoding rather than hard-coding it.
  uint8 halt[2] = { 0, 0 };
  bool bHaltFound = false;
  for(uint32 word = 0x8000; word < 0x10000 && !bHaltFound; word++)
  {
    mem[0] = (uint8)(word >> 8);
    mem[1] = (uint8)word;
    if(mem[0] >= 0x88 && mem[0] <= 0x9F)
      continue; // start of a longer instruction
    InstructionCacheEntry entry;
    DecodeAt(mpe, mem, kCodeBase, entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST);
    if((entry.packetInfo & PACKETINFO_EXCEPTION) && entry.pcroute == kCodeBase + 2)
    {
      halt[0] = mem[0];
      halt[1] = mem[1];
      bHaltFound = true;
    }
  }
  if(!bHaltFound)
  {
    Log("Arm64 JIT difftest: cannot run (no HALT encoding found)");
    return;
  }

  // Each trial compares one block against the packets it covers. Chaining
  // would let the native side run on into whatever block a taken branch leads
  // to - the run itself, when the branch goes back to its start.
  const unsigned savedChainPackets = a64ChainPackets;
  a64ChainPackets = 0;

  Rng rng = { 0x9E3779B97F4A7C15ULL ^ ((uint64)seed << 1) ^ 1 };
  for(uint32 i = 0; i < kDataBytes; i++)
    data[i] = (uint8)rng.Next();
  uint8 * const local = (uint8 *)nuonEnv.GetPointerToMemory(mpe.mpeIndex, MPE_DTRAM_BASE, false);
  for(uint32 i = 0; i < MPE_DTAGS_BASE - MPE_DTRAM_BASE; i++)
    local[i] = (uint8)rng.Next();

  unsigned numCompared = 0, numNotCompiled = 0, numOutside = 0, numMismatched = 0, numUndefined = 0, numLogged = 0;
  unsigned numWithMemory = 0, numWithBranch = 0, numBranchTaken = 0;
  std::map<std::string, unsigned> mismatchByHandler;
  constexpr uint32 kNumHandlers = Handler_PacketStart;
  static unsigned handlerRuns[kNumHandlers];
  memset(handlerRuns, 0, sizeof(handlerRuns));

  for(unsigned trial = 0; trial < trials; trial++)
  {
    // Build a run of acceptable packets followed by HALT. A delayed branch
    // takes the two packets after it along as its delay slots, so it is only
    // placed where two more plain packets can follow, and it ends the run.
    // (With the emitter off the decoder marks every packet as never to be
    // compiled, and the generator would turn them all down.)
    nuonEnv.compilerOptions.bArm64NativeCode = true;
    const uint32 numPackets = 1 + rng.Below(kMaxPacketsPerRun);
    uint32 cursor = kCodeBase;
    g_runPointers.clear();
    bool bHasBranch = false, bHasMemory = false;
    for(uint32 n = 0; n < numPackets && !bHasBranch; n++)
    {
      uint32 info = 0;
      const uint32 length = PlacePacket(rng, mpe, mem, cursor, true, info);
      if(!length)
        break;
      cursor += length;
      if(info & PACKETINFO_MEM)
        bHasMemory = true;
      if(info & (kBranchMask | PACKETINFO_ECU))
      {
        bHasBranch = true;
        for(int slot = 0; slot < 2; slot++)
        {
          const uint32 slotLength = PlacePacket(rng, mpe, mem, cursor, false, info);
          cursor += slotLength;
          if(info & PACKETINFO_MEM)
            bHasMemory = true;
        }
      }
    }
    const uint32 endAddress = cursor;
    if(endAddress == kCodeBase)
      continue;
    mem[endAddress - kCodeBase] = halt[0];
    mem[endAddress - kCodeBase + 1] = halt[1];

    MachineState initial;
    RandomState(rng, initial);

    // --- Interpreter, packet by packet, noting the memory it reaches.
    nuonEnv.compilerOptions.bArm64NativeCode = false;
    mpe.nativeCodeCache.Flush();
    mpe.instructionCache->Invalidate();
    ApplyState(mpe, initial);

    g_trace.clear();
    g_traceOutsideWindow = false;
    for(uint8 * const pointer : g_runPointers)
      TracePointer(pointer);
    // Push and pop index the MPE's memory directly, unseen by the trace.
    TracePointer(&mpe.dtrom[(initial.sp - 128) & MPE_VALID_MEMORY_MASK]);
    TracePointer(&mpe.dtrom[(initial.sp + 64) & MPE_VALID_MEMORY_MASK]);
    a64MemoryTraceHook = TraceHook;
    bool bUndefinedInput = false;
    for(int step = 0; step < 16; step++)
    {
      if(mpe.ecuSkipCounter == 0 && (mpe.pcexec < kCodeBase || mpe.pcexec >= endAddress))
        break;
      uint32 sequentialNext = 0;
      if(mpe.pcexec >= kCodeBase && mpe.pcexec < endAddress)
      {
        // A multiply whose shift count comes from a register can still meet
        // the undefined left shift by 64 (see IsOutOfSpec) when an earlier
        // packet of the run computed that count.
        InstructionCacheEntry entry;
        DecodeAt(mpe, mem, mpe.pcexec, entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST);
        for(uint32 i = 0; i < entry.nuanceCount; i++)
        {
          if(IsUndefinedBilinearAccess(mpe, entry, i))
            bUndefinedInput = true;
          const Nuance &nuance = (Nuance &)(entry.nuances[FIXED_FIELD(i,0)]);
          // A register written to cc with a coprocessor flag set: as for the
          // constant form (IsSafeControlRegisterAccess), constant propagation
          // drops those bits when it knows the register's value.
          if(entry.handlers[i] == Handler_StoreScalarControlRegisterAbsolute &&
             ((((uint32)nuance.fields[FIELD_MEM_TO]) & 0x1FF0) >> 4) == 0x04 &&
             (mpe.reg_union[nuance.fields[FIELD_MEM_FROM]] & (CC_COPROCESSOR0 | CC_COPROCESSOR1)))
            bUndefinedInput = true;
          uint32 count = 0;
          if(entry.handlers[i] == Handler_MULScalarShiftScalar || entry.handlers[i] == Handler_MULImmediateShiftScalar)
            count = mpe.reg_union[nuance.fields[FIELD_MUL_INFO]];
          else if(entry.handlers[i] == Handler_MULScalarShiftAcshift || entry.handlers[i] == Handler_MULImmediateShiftAcshift)
            count = mpe.acshift;
          else
            continue;
          if((count & 0x7FU) == 0x40U)
            bUndefinedInput = true;
        }
        sequentialNext = entry.pcroute;
      }
      mpe.FetchDecodeExecute();
      // A branch that has been taken ends the run, wherever it went: a target
      // inside the run would be the middle of some instruction.
      if(mpe.ecuSkipCounter == 0 && mpe.pcexec != sequentialNext)
        break;
    }
    a64MemoryTraceHook = nullptr;

    const bool bInterpreterFinished = (mpe.ecuSkipCounter == 0) && (mpe.mpectl & MPECTRL_MPEGO);
    MachineState interpreted;
    CaptureState(mpe, interpreted);
    for(TraceEntry &entry : g_trace)
      memcpy(entry.afterInterpreter, entry.start, kTraceBytes);
    RestoreTracedMemory();

    if(g_traceOutsideWindow || !bInterpreterFinished)
    {
      // The run reached memory the test does not control, looped back into
      // itself, or stopped the processor. Nothing to compare.
      numOutside++;
      continue;
    }

    // --- Native: the whole run as one block.
    nuonEnv.compilerOptions.bArm64NativeCode = true;
    mpe.nativeCodeCache.Flush();
    mpe.instructionCache->Invalidate();
    ApplyState(mpe, initial);

    bool bError = false;
    mpe.superBlock.CompileBlock(kCodeBase, mpe.nativeCodeCache, bError);
    const NativeCodeCacheEntry * const cacheEntry = bError ? nullptr : mpe.nativeCodeCache.pageMap.FindEntry(kCodeBase);
    if(bError || !cacheEntry || cacheEntry->virtualAddress != kCodeBase ||
       cacheEntry->compileType != SuperBlockCompileType::SUPERBLOCKCOMPILETYPE_NATIVE_CODE_BLOCK)
    {
      numNotCompiled++;
      continue;
    }
    const uint32 blockEnd = cacheEntry->nextVirtualAddress;

    // NUANCE_A64_DIFFTEST_TRACE: name each block before running it, for the
    // case where the block itself crashes and takes the report with it.
    static const bool bTrace = getenv("NUANCE_A64_DIFFTEST_TRACE") != nullptr;
    if(bTrace)
      fprintf(stderr, "trial %u: %s\n", trial, Disassemble(mpe, mem, kCodeBase, endAddress).c_str());

    // NUANCE_A64_DIFFTEST_DUMP=<trial>: print that trial's native code as hex
    // words, for a disassembler (llvm-mc --disassemble takes them as is).
    static const int dumpTrial = getenv("NUANCE_A64_DIFFTEST_DUMP") ? atoi(getenv("NUANCE_A64_DIFFTEST_DUMP")) : -1;
    if(dumpTrial == (int)trial)
    {
      const uint8 * const code = (const uint8 *)cacheEntry->entryPoint;
      const uint8 * const codeEnd = mpe.nativeCodeCache.GetEmitPointer();
      fprintf(stderr, "trial %u: %s\ncode:", trial, Disassemble(mpe, mem, kCodeBase, endAddress).c_str());
      for(const uint8 *b = code; b + 4 <= codeEnd; b += 4)
        fprintf(stderr, " 0x%02x 0x%02x 0x%02x 0x%02x", b[0], b[1], b[2], b[3]);
      fprintf(stderr, "\n");
    }

    ApplyState(mpe, initial);
    a64DebugBlockPc = 0;
    mpe.FetchDecodeExecute();
    const bool bRanNatively = (a64DebugBlockPc == kCodeBase);

    // A block may end before the run does (the fetcher stops where it must);
    // the dispatcher interprets the rest, exactly as it would in a game.
    // (A block can also stop early and hand one packet back to the
    // interpreter - an indirect store that turns out to need it does. That
    // is not a branch; the loop below carries on from there.)
    bool bBranched = (mpe.ecuSkipCounter == 0 && mpe.pcexec != blockEnd && !mpe.interpretNextPacket);
    for(int step = 0; step < 16 && !bBranched; step++)
    {
      if(mpe.ecuSkipCounter == 0 && (mpe.pcexec < kCodeBase || mpe.pcexec >= endAddress))
        break;
      uint32 sequentialNext = 0;
      if(mpe.pcexec >= kCodeBase && mpe.pcexec < endAddress)
      {
        InstructionCacheEntry entry;
        DecodeAt(mpe, mem, mpe.pcexec, entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST);
        sequentialNext = entry.pcroute;
      }
      mpe.FetchDecodeExecute();
      bBranched = (mpe.ecuSkipCounter == 0 && mpe.pcexec != sequentialNext);
    }

    MachineState native;
    CaptureState(mpe, native);

    numCompared++;
    for(uint32 pc = kCodeBase; pc < endAddress; )
    {
      InstructionCacheEntry entry;
      if(!DecodeAt(mpe, mem, pc, entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST))
        break;
      for(uint32 i = 0; i < entry.nuanceCount; i++)
      {
        if(entry.handlers[i] < kNumHandlers)
          handlerRuns[entry.handlers[i]]++;
      }
      pc = entry.pcroute;
    }
    if(bHasMemory) numWithMemory++;
    if(bHasBranch) numWithBranch++;
    if(bHasBranch && interpreted.pcexec != endAddress) numBranchTaken++;

    std::string diff;
    if(!bRanNatively)
      diff += " the block was compiled but the dispatcher did not run it;";
    if(native.pcexec != interpreted.pcexec)
    {
      char buf[96];
      snprintf(buf, sizeof(buf), " pcexec native %08X interp %08X;", native.pcexec, interpreted.pcexec);
      diff += buf;
    }
    for(int i = 0; i < 48; i++)
    {
      if(native.regs[i] != interpreted.regs[i])
      {
        char buf[128];
        snprintf(buf, sizeof(buf), " %s native %08X interp %08X (was %08X);", kRegNames[i], native.regs[i], interpreted.regs[i], initial.regs[i]);
        diff += buf;
      }
    }
    if(native.sp != interpreted.sp)
    {
      char buf[96];
      snprintf(buf, sizeof(buf), " sp native %08X interp %08X (was %08X);", native.sp, interpreted.sp, initial.sp);
      diff += buf;
    }
    for(const TraceEntry &entry : g_trace)
    {
      if(memcmp(entry.start, entry.afterInterpreter, kTraceBytes))
      {
        uint32 first = 0;
        while(entry.start[first] == entry.afterInterpreter[first]) first++;
        char buf[160];
        snprintf(buf, sizeof(buf), " memory %+d bytes from a traced address: native %02X%02X%02X%02X interp %02X%02X%02X%02X (was %02X%02X%02X%02X);",
          (int)first - (int)kTraceBefore,
          entry.start[first], entry.start[first+1], entry.start[first+2], entry.start[first+3],
          entry.afterInterpreter[first], entry.afterInterpreter[first+1], entry.afterInterpreter[first+2], entry.afterInterpreter[first+3],
          entry.before[first], entry.before[first+1], entry.before[first+2], entry.before[first+3]);
        diff += buf;
        break;
      }
    }
    RestoreTracedMemory();

    if(diff.empty())
      continue;

    if(bUndefinedInput)
    {
      numUndefined++;
      continue;
    }

    numMismatched++;
    const std::string code = Disassemble(mpe, mem, kCodeBase, endAddress);
    // Key the tally on the mnemonic of a single-instruction run; longer runs go
    // under one heading, since any of their instructions could be the one.
    const bool bSingle = (code.find("} {") == std::string::npos) && (code.find(" ; ") == std::string::npos);
    const std::string key = bSingle ? code.substr(1, code.find_first_of(" }", 1) - 1) : "(several instructions)";
    mismatchByHandler[key]++;

    if(numLogged < 60 && mismatchByHandler[key] <= 3)
    {
      numLogged++;
      Log("Arm64 JIT difftest: MISMATCH trial %u (block ends %08X of %08X): %s->%s", trial, blockEnd, endAddress, code.c_str(), diff.c_str());
      std::string regs;
      for(int i = 0; i < 48; i++)
      {
        char buf[48];
        snprintf(buf, sizeof(buf), " %s=%08X", kRegNames[i], initial.regs[i]);
        regs += buf;
      }
      Log("Arm64 JIT difftest:   initial:%s sp=%08X xybase=%08X uvbase=%08X clutbase=%08X linpixctl=%08X", regs.c_str(),
        initial.sp, initial.xybase, initial.uvbase, initial.clutbase, initial.linpixctl);
    }
  }

  std::string tally;
  for(const auto &entry : mismatchByHandler)
  {
    char buf[96];
    snprintf(buf, sizeof(buf), " %s (%u)", entry.first.c_str(), entry.second);
    tally += buf;
  }

  char stats[1024];
  A64_FormatStats(stats, sizeof(stats));
  Log("Arm64 JIT difftest: %u trials, seed %u: %u compared (%u with memory access, %u with a branch, %u of those taken), %u mismatched%s%s",
    trials, seed, numCompared, numWithMemory, numWithBranch, numBranchTaken, numMismatched, tally.empty() ? "" : " -", tally.c_str());
  Log("Arm64 JIT difftest: not compared: %u not compiled natively, %u reached outside the test's memory or did not finish; %u differed only where the instruction set leaves the result undefined",
    numNotCompiled, numOutside, numUndefined);
  Log("Arm64 JIT difftest: %s", stats);

  // Which instruction handlers that have an emitter never came up. The
  // numbers are positions in the Handlers enum (Handlers.h).
  std::string missing;
  unsigned numCovered = 0, numEmittable = 0;
  for(uint32 h = 0; h < kNumHandlers; h++)
  {
    if(!emitHandlers[h])
      continue;
    numEmittable++;
    if(handlerRuns[h])
      numCovered++;
    else
    {
      char buf[16];
      snprintf(buf, sizeof(buf), " %u", h);
      missing += buf;
    }
  }
  Log("Arm64 JIT difftest: %u of %u handlers with an emitter were exercised%s%s", numCovered, numEmittable, missing.empty() ? "" : "; not exercised:", missing.c_str());

  // The machine is not put back as it was: the test is for a process that
  // exits afterwards (tools/a64_difftest_host.c), not for a game session.
  nuonEnv.compilerOptions.bArm64NativeCode = true;
  a64ChainPackets = savedChainPackets;
  mpe.nativeCodeCache.Flush();
  mpe.instructionCache->Invalidate();
}

#endif // USE_A64_JIT
