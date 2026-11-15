// Differential test for the Arm64 JIT backend.
//
// Writes short runs of random NUON packets into main-bus RAM, compiles each run
// into a native block, and executes it twice from the same random register
// state: once as native code and once through the interpreter. Any difference
// in the 48 registers afterwards is a bug in the backend (or, now and then, a
// place where the emitters and the interpreter disagree by design - those show
// up on x86 too and are for a person to judge).
//
// Runs that cannot be compiled natively are counted, with the reason, so the
// output also says what the backend still lacks.
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
#include "Handlers.h"
#include "InstructionCache.h"
#include "mpe.h"
#include "NativeCodeCache.h"
#include "NuonEnvironment.h"
#include "NuonMemoryMap.h"
#include "SuperBlock.h"

extern NuonEnvironment nuonEnv;
extern NuancePrintHandler printHandlers[];
void libretro_log_info(const char* text);

namespace {

constexpr uint32 kTestBase = MAIN_BUS_BASE + 0x00400000U;
constexpr uint32 kRegionBytes = 512;
constexpr uint32 kMaxPacketsPerRun = 4;

// Packets the test leaves out for now: anything that touches memory or
// changes control flow needs a controlled environment of its own.
constexpr uint32 kRejectMask =
  PACKETINFO_NEVERCOMPILE | PACKETINFO_EXCEPTION | PACKETINFO_NEEDS_PCFETCHNEXT |
  PACKETINFO_BRANCH_ALWAYS | PACKETINFO_BRANCH_CONDITIONAL | PACKETINFO_BRANCH_NOP |
  PACKETINFO_MEMORY_INDIRECT | PACKETINFO_MEMORY_IO | PACKETINFO_MEM | PACKETINFO_ECU |
  PACKETINFO_BREAKPOINT;

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
  // Register values: mostly random, with the edge cases over-represented.
  uint32 Value()
  {
    static const uint32 edges[] = { 0, 1, 2, 0xFFFFFFFFU, 0x80000000U, 0x7FFFFFFFU, 0x0000FFFFU, 0xFFFF0000U, 0x00008000U, 0x00010000U, 31, 32, 33 };
    const uint32 pick = Below(8);
    if(pick == 0) return edges[Below(sizeof(edges)/sizeof(edges[0]))];
    if(pick == 1) return Below(64);
    if(pick == 2) return (uint32)(-(int32)Below(64));
    return Next();
  }
};

void Log(const char * const fmt, ...)
{
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  libretro_log_info(line);
}

void RandomState(Rng &rng, uint32 state[48])
{
  for(int i = 0; i < 32; i++)
  {
    state[i] = rng.Value();
    // Any register can be a multiply's shift count, and a count field of 0x40
    // is the undefined left-shift-by-64 (see IsOutOfSpec).
    if((state[i] & 0x7FU) == 0x40U)
      state[i] ^= 1;
  }
  state[32] = rng.Next() & CC_ALLFLAGS;     // cc
  state[33] = rng.Value() & 0xFFFFU;        // rc0
  state[34] = rng.Value() & 0xFFFFU;        // rc1
  for(int i = 35; i < 46; i++)              // rx ry ru rv rz rzi1 rzi2 xyctl uvctl xyrange uvrange
    state[i] = rng.Value();
  state[46] = rng.Next() & 0x7FU;           // acshift
  if(state[46] == 0x40U)
    state[46] = 0x41U;
  state[47] = rng.Next() & 0x3U;            // svshift
}

// One random instruction, shaped so the decoder sees a complete packet.
uint32 RandomPacketBytes(Rng &rng, uint8 * const p)
{
  for(int i = 0; i < 8; i++)
    p[i] = (uint8)rng.Next();

  const uint32 kind = rng.Below(10);
  if(kind < 5)
  {
    p[0] &= 0x3F;                           // 16-bit ALU, always terminates the packet
    return 2;
  }
  if(kind < 8)
  {
    p[0] = 0x90 | (p[0] & 0x0F);            // 32-bit instruction
    p[2] |= 0x10;                           // packet terminator
    return 4;
  }
  p[0] = 0x88 | (p[0] & 0x07);              // 32-bit extension, then a 32-bit instruction
  p[4] = 0x90 | (p[4] & 0x0F);
  p[6] |= 0x10;
  return 8;
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

std::string Disassemble(MPE &mpe, const uint32 start, const uint32 end)
{
  std::string text;
  uint32 pc = start;
  while(pc < end)
  {
    InstructionCacheEntry entry;
    entry.pcexec = pc;
    mpe.DecompressPacket((uint8 *)nuonEnv.GetPointerToMemory(mpe.mpeIndex, pc, false), entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST);
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
    if(entry.pcroute <= pc) break;
    pc = entry.pcroute;
  }
  return text;
}

const char * const kRegNames[48] = {
  "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","r13","r14","r15",
  "r16","r17","r18","r19","r20","r21","r22","r23","r24","r25","r26","r27","r28","r29","r30","r31",
  "cc","rc0","rc1","rx","ry","ru","rv","rz","rzi1","rzi2","xyctl","uvctl","xyrange","uvrange","acshift","svshift" };

} // namespace

void A64_DiffTest(const unsigned trials, const unsigned seed)
{
  MPE &mpe = nuonEnv.mpe[0];
  uint8 * const mem = (uint8 *)nuonEnv.GetPointerToMemory(mpe.mpeIndex, kTestBase, false);
  if(!mem || !nuonEnv.compilerOptions.CanCompile())
  {
    Log("Arm64 JIT difftest: cannot run (no test memory, or the native emitter is off)");
    return;
  }

  uint8 savedMem[kRegionBytes];
  memcpy(savedMem, mem, kRegionBytes);
  uint32 savedRegs[48];
  memcpy(savedRegs, mpe.reg_union, sizeof(savedRegs));
  const uint32 savedPcexec = mpe.pcexec;

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
    entry.pcexec = kTestBase;
    mpe.DecompressPacket(mem, entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST);
    if((entry.packetInfo & PACKETINFO_EXCEPTION) && entry.pcroute == kTestBase + 2)
    {
      halt[0] = mem[0];
      halt[1] = mem[1];
      bHaltFound = true;
    }
  }
  if(!bHaltFound)
  {
    Log("Arm64 JIT difftest: cannot run (no HALT encoding found)");
    memcpy(mem, savedMem, kRegionBytes);
    return;
  }

  Rng rng = { 0x9E3779B97F4A7C15ULL ^ ((uint64)seed << 1) ^ 1 };
  unsigned numCompiled = 0, numNotCompiled = 0, numMismatched = 0, numUndefined = 0, numLogged = 0;
  std::map<std::string, unsigned> mismatchByHandler;

  for(unsigned trial = 0; trial < trials; trial++)
  {
    // Build a run of acceptable packets followed by HALT.
    const uint32 numPackets = 1 + rng.Below(kMaxPacketsPerRun);
    uint32 cursor = kTestBase;
    for(uint32 n = 0; n < numPackets; n++)
    {
      for(int attempt = 0; attempt < 1000; attempt++)
      {
        const uint32 length = RandomPacketBytes(rng, mem + (cursor - kTestBase));
        InstructionCacheEntry entry;
        entry.pcexec = cursor;
        mpe.DecompressPacket(mem + (cursor - kTestBase), entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST | DECOMPRESS_OPTIONS_SCHEDULE_MEM_FIRST);
        bool bAccept = !(entry.packetInfo & kRejectMask) && entry.pcroute == cursor + length && entry.nuanceCount > 0;
        // A slot the decoder claimed but did not fill reads as an ECU NOP:
        // not a real instruction, so not worth a trial.
        for(uint32 i = 0; i < entry.nuanceCount && bAccept; i++)
          bAccept = (entry.handlers[i] != Handler_ECU_NOP) && !IsOutOfSpec(entry, i);
        if(bAccept)
        {
          cursor += length;
          break;
        }
      }
    }
    const uint32 endAddress = cursor;
    if(endAddress == kTestBase)
      continue;
    mem[endAddress - kTestBase] = halt[0];
    mem[endAddress - kTestBase + 1] = halt[1];

    uint32 initial[48];
    RandomState(rng, initial);

    // Native.
    mpe.nativeCodeCache.Flush();
    mpe.instructionCache->Invalidate();
    memcpy(mpe.reg_union, initial, sizeof(initial));
    mpe.ecuSkipCounter = 0;
    mpe.pcexec = kTestBase;

    bool bError = false;
    const NativeCodeCacheEntryPoint entryPoint = mpe.superBlock.CompileBlock(kTestBase, mpe.nativeCodeCache, bError);
    const NativeCodeCacheEntry * const cacheEntry = bError ? nullptr : mpe.nativeCodeCache.pageMap.FindEntry(kTestBase);
    if(bError || !cacheEntry || cacheEntry->virtualAddress != kTestBase ||
       cacheEntry->compileType != SuperBlockCompileType::SUPERBLOCKCOMPILETYPE_NATIVE_CODE_BLOCK)
    {
      numNotCompiled++;
      continue;
    }
    numCompiled++;

    const uint32 blockEnd = cacheEntry->nextVirtualAddress;
    // NUANCE_A64_DIFFTEST_TRACE: name each block before running it, for the
    // case where the block itself crashes and takes the report with it.
    static const bool bTrace = getenv("NUANCE_A64_DIFFTEST_TRACE") != nullptr;
    if(bTrace)
      fprintf(stderr, "trial %u: %s\n", trial, Disassemble(mpe, kTestBase, blockEnd).c_str());
    entryPoint();
    uint32 nativeResult[48];
    memcpy(nativeResult, mpe.reg_union, sizeof(nativeResult));
    const uint32 nativePc = mpe.pcexec;

    // Interpreter, over exactly the packets the block covered.
    memcpy(mpe.reg_union, initial, sizeof(initial));
    uint32 pc = kTestBase;
    bool bUndefinedShift = false;
    while(pc < blockEnd)
    {
      InstructionCacheEntry entry;
      entry.pcexec = pc;
      mpe.DecompressPacket(mem + (pc - kTestBase), entry, DECOMPRESS_OPTIONS_SCHEDULE_ECU_LAST);
      for(uint32 i = 0; i < entry.nuanceCount; i++)
      {
        // A multiply whose shift count comes from a register can still meet
        // the undefined left shift by 64 (see IsOutOfSpec) when an earlier
        // packet of the run computed that count.
        const Nuance &nuance = (Nuance &)(entry.nuances[FIXED_FIELD(i,0)]);
        uint32 count = 0;
        if(entry.handlers[i] == Handler_MULScalarShiftScalar || entry.handlers[i] == Handler_MULImmediateShiftScalar)
          count = mpe.reg_union[nuance.fields[FIELD_MUL_INFO]];
        else if(entry.handlers[i] == Handler_MULScalarShiftAcshift || entry.handlers[i] == Handler_MULImmediateShiftAcshift)
          count = mpe.acshift;
        else
          continue;
        if((count & 0x7FU) == 0x40U)
          bUndefinedShift = true;
      }
      mpe.pcroute = entry.pcroute;
      mpe.pcexec = entry.pcroute;
      mpe.ExecuteNuances(entry);
      pc = entry.pcroute;
    }

    bool bMismatch = (nativePc != blockEnd);
    for(int i = 0; i < 48 && !bMismatch; i++)
      bMismatch = (nativeResult[i] != mpe.reg_union[i]);

    if(bMismatch && bUndefinedShift)
    {
      numUndefined++;
    }
    else if(bMismatch)
    {
      numMismatched++;
      const std::string code = Disassemble(mpe, kTestBase, blockEnd);
      // Key the tally on the first mnemonic of a single-packet run; longer
      // runs go under one heading, since any of their packets could be it.
      std::string key = (blockEnd == kTestBase + 2 || numPackets == 1) ? code.substr(1, code.find_first_of(" }", 1) - 1) : "(multi-packet)";
      mismatchByHandler[key]++;

      if(numLogged < 40 && mismatchByHandler[key] <= 3)
      {
        numLogged++;
        std::string diff;
        if(nativePc != blockEnd)
        {
          char buf[96];
          snprintf(buf, sizeof(buf), " pcexec native %08X want %08X;", nativePc, blockEnd);
          diff += buf;
        }
        for(int i = 0; i < 48; i++)
        {
          if(nativeResult[i] != mpe.reg_union[i])
          {
            char buf[128];
            snprintf(buf, sizeof(buf), " %s native %08X interp %08X (was %08X);", kRegNames[i], nativeResult[i], mpe.reg_union[i], initial[i]);
            diff += buf;
          }
        }
        Log("Arm64 JIT difftest: MISMATCH trial %u: %s->%s", trial, code.c_str(), diff.c_str());
        std::string regs;
        for(int i = 0; i < 48; i++)
        {
          char buf[48];
          snprintf(buf, sizeof(buf), " %s=%08X", kRegNames[i], initial[i]);
          regs += buf;
        }
        Log("Arm64 JIT difftest:   initial:%s", regs.c_str());
      }
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
  Log("Arm64 JIT difftest: %u trials, seed %u: %u compiled natively, %u not, %u mismatched%s%s (%u more differed only through an undefined shift)",
    trials, seed, numCompiled, numNotCompiled, numMismatched, tally.empty() ? "" : " -", tally.c_str(), numUndefined);
  Log("Arm64 JIT difftest: %s", stats);

  // Leave the machine as it was found.
  memcpy(mem, savedMem, kRegionBytes);
  memcpy(mpe.reg_union, savedRegs, sizeof(savedRegs));
  mpe.pcexec = savedPcexec;
  mpe.ecuSkipCounter = 0;
  mpe.nativeCodeCache.Flush();
  mpe.instructionCache->Invalidate();
}

#endif // USE_A64_JIT
