#ifndef EMITMISC_H
#define EMITMISC_H

#include "basetypes.h"
#include "X86EmitTypes.h"
#include "InstructionDependencies.h"

class MPE;
struct InstructionEntry;
struct Nuance;
class NativeCodeCache;

struct EmitterVariables
{
  MPE *mpe;

  InstructionEntry *pInstructionEntry;
  uint32 scalarRegDep;
  uint32 miscRegDep;
  uint32 scalarRegOutDep;
  uint32 miscRegOutDep;

  // Currently forced to false (set in SuperBlock::EmitCodeBlock, but never updated).
  // The 'if(vars->bCheckECUSkipCounter)' guards in EmitECU.cpp would emit a runtime-
  // "skip this ECU op if a prior branch is still in its delay window"-check,
  // mirroring the IL path's per-op 'if(!ecuSkipCounter)' guards. They are unused
  // because FetchSuperBlock bails native compilation when delay-slot packets
  // contain ECU ops, eliminating the case those guards would handle. See
  // comment block at the top of SuperBlock::EmitCodeBlock for a full explanation
#ifdef USE_A64_JIT
  // Set by SuperBlock::EmitCodeBlock for a branch that sits in the second
  // delay slot of a conditional branch (see FetchSuperBlock).
  bool bCheckECUSkipCounter = false;
  // Set by SuperBlock::EmitCodeBlock while it emits a packet whose branch may leave the
  // block early: how many of the block's packets have not run by then. Emit_ExitBlock
  // takes them off the MPE's packet count, which was made for the whole block when it began.
  uint32 a64ExitNotRun = 0;
  // Set while it emits a delay-slot packet of a branch that may already have been taken
  // (ecuSkipCounter 1, the native convention: one step for the dispatcher to resolve it):
  // 2 for the first slot, 1 for the second. A block that leaves from inside such a packet
  // (the bail to the interpreter, which runs that packet and then the other slot) leaves
  // the branch as the interpreter would have it, as it is to be found at the next turn:
  // pending for as many packets as are still to run, plus the one the dispatcher takes off
  // at the end of this turn. Emit_ExitBlock does the multiplication: 1 becomes 3 or 2.
  uint32 a64ExitSlotBump = 0;
#else
  static constexpr bool bCheckECUSkipCounter = false;
#endif
};

inline x86BaseReg GetScalarRegReadBaseReg(const EmitterVariables* const vars, const uint32 regIndex)
{
  return (vars->scalarRegDep & SCALAR_REG_DEPENDENCY_MASK(regIndex)) ? x86BaseReg::x86BaseReg_edi : x86BaseReg::x86BaseReg_esi;
}

inline x86BaseReg GetScalarRegWriteBaseReg(const EmitterVariables* const vars, const uint32 regIndex)
{
  return x86BaseReg::x86BaseReg_esi;
}

inline x86BaseReg GetMiscRegReadBaseReg(const EmitterVariables * const vars, const uint32 regIndex)
{
  const uint32 mask = !regIndex ? DEPENDENCY_FLAG_ALLFLAGS : MISC_REG_DEPENDENCY_MASK(regIndex - 1);

  return (vars->miscRegDep & mask) ? x86BaseReg::x86BaseReg_edi : x86BaseReg::x86BaseReg_esi;
}

inline x86BaseReg GetMiscRegWriteBaseReg(const EmitterVariables * const vars, const uint32 regIndex)
{
  return x86BaseReg::x86BaseReg_esi;
}

inline int32 GetMiscRegEmitDisp(const EmitterVariables * const vars, const uint32 regIndex)
{
  return regIndex*sizeof(uint32);
}

inline int32 GetScalarRegEmitDisp(const EmitterVariables * const vars, const uint32 regIndex)
{
  return -((32 - (int32)regIndex) * ((int32)sizeof(uint32)));
}

void Emit_ExitBlock(const EmitterVariables * const vars);
void Emit_NOP(EmitterVariables * const vars, const Nuance &nuance);
void Emit_SaveRegs(EmitterVariables * const vars, const Nuance &nuance);
void Emit_StoreMiscRegisterConstant(EmitterVariables * const vars, const Nuance &nuance);
void Emit_StoreScalarRegisterConstant(EmitterVariables * const vars, const Nuance &nuance);

#endif
