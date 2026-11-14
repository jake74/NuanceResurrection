# Arm64 JIT backend: plan

Status: proposed, 2026-11-14. Nothing here is implemented yet.

## Goal

Run the dynamic recompiler natively on Arm64, starting with macOS on Apple
Silicon, so that games stop being interpreter-bound.

Baseline on an M4 Pro (interpreter, libretro core in RetroArch):

| Content | Emulated cycles per 16 ms frame | Result |
|---|---|---|
| Bundled test disc (`NUON.CD`) | ~1,850,000 | 60 fps |
| Tempest 3000 | ~205,000 | ~5 fps |

Tempest 3000 needs roughly 5-10x. About 40% of core CPU time is in
`MPE::FetchDecodeExecute` (dispatch), the rest spread across `Execute_*`
handlers, so only compiled blocks can close the gap. Whether this approach
gets Tempest 3000 all the way to full speed is not known until Milestone 4.

## How the existing JIT is built

- `SuperBlock::EmitCodeBlock` walks a block and calls one `Emit_*` handler per
  NUON instruction: about 219 handlers, ~12,300 lines, in `src/Emit*.cpp`.
- Handlers never write machine code. They call 104 distinct x86-shaped
  primitives on `NativeCodeCache` (`X86Emit_MOVMR`, `X86Emit_ADDIR`,
  `X86Emit_SETZR`, ...), about 4,000 call sites in total.
- `src/nativecodecache.cpp` implements each primitive twice: raw bytes for
  32-bit x86, and `asmjit::x86::Assembler` for x86-64 (`if (asmjitAs)`).
- Block ABI: `PUSHAD`, then `esi` = `&mpe->cc` (live register file) and
  `edi` = `&mpe->tempCC` (packet-start snapshot), body, `POPAD`, `RETN`.
- Helper calls (`X86Emit_CALLI`, 40 sites) pass arguments in `ecx`/`edx`
  (fastcall) and return in `eax`.
- Arm builds currently force the interpreter in three places: `src/mpe.h`
  (`COMPILE_TYPE`, `ALLOW_NATIVE_CODE_EMIT`), `src/SuperBlock.h`
  (`bAllowCompile`), and `CMakeLists.txt` (`NUANCE_X86_64`,
  `ASMJIT_NO_AARCH64`).

## Approach

Keep all 219 handlers untouched and add a third implementation of the 104
primitives that emits Arm64 code with the same observable behaviour as the x86
instruction. asmjit already ships an AArch64 assembler in the vendored
submodule; the build only has to stop disabling it.

A native rewrite of the handlers would produce better code but is months of
work. It can be done later, handler by handler, for whatever the profiler says
is hot.

### Code layout

- New `src/nativecodecache_a64.cpp` holding the Arm64 versions of every
  `NativeCodeCache::X86Emit_*` method. The x86 bodies in `nativecodecache.cpp`
  are compiled out on Arm64, so there are no per-primitive `#ifdef`s.
- New `src/a64_emit.h` for register mapping, memory-operand lowering and the
  flag tracker (the Arm64 counterpart of `asmjit_emit.h`).
- `EmitMEM.cpp` uses asmjit x86 directly in four places (`Emit_LoadBankPtr`).
  Those become a new primitive so handlers stay architecture-neutral.

### Register mapping

| x86 | Arm64 | Why |
|---|---|---|
| `eax`, `ecx`, `edx` | `w9`, `w10`, `w11` | Caller-saved on both sides; handlers already assume calls clobber them |
| `ebx`, `ebp` | `w19`, `w20` | Callee-saved on x86-64, so handlers rely on them surviving calls |
| `esi`, `edi` | `x21`, `x22` | Register-file base pointers; callee-saved means no save/restore around calls |
| `r15` scratch | `x16`, `x17` | Address materialisation and read-modify-write temporaries |
| `xmm0`-`xmm7` | `v16`-`v23` | Caller-saved, like XMM under System V |

`PUSHAD`/`POPAD` become a prologue/epilogue saving `x19`-`x22`, `x29`, `x30`.
`CALLI` moves `w10`/`w11` into `w0`/`w1`, calls through `x16`, and moves `w0`
back to `w9`.

### The hard parts

1. **Condition flags.** Handlers read x86 flags after arithmetic (`SETZ`/`SETS`
   57 each, `SETO`/`SETB` 29 each, 84 `JCC`, ~95 `CMOVcc`, plus `ADC`, `SBB`,
   `RCR`). Differences to bridge:
   - Arm's carry is inverted after subtraction. The emitter tracks the kind of
     the last flag-producing primitive at emit time and picks the condition
     accordingly (`SETB` is `cset lo` after a subtract, `cset hs` after an add).
   - `OR`/`XOR` have no flag-setting form on Arm; follow with `tst`.
   - Shifts do not set flags on Arm. x86 carry-out of a shift has to be
     computed explicitly where a consumer needs it.
   - The tracker resets to "unknown" at labels and asserts in debug builds if a
     consumer asks for a flag the last producer did not materialise. That turns
     silent wrong flags into a build-time list of cases to handle.
2. **Sub-registers.** `ah`/`ch`/`dh`/`bh` are bits 8-15; writes to 8- and
   16-bit registers must preserve the rest. Lowered with `ubfx`/`bfi`.
3. **Memory operands.** x86 operands with 32-bit displacements, index and
   scale, absolute 64-bit addresses, and immediate-to-memory or
   read-modify-write forms (`ANDIM`, `ORRM`, `MOVIM`, ...) all become
   load/op/store sequences through `x16`/`x17`.
4. **SSE primitives** (about 180 call sites): `MOVDQU`/`MOVDQA` to `ldr`/`str q`,
   `PSHUFB` to `tbl`, `PMULLD` to `mul`, `PHADDD` to `addp`, `PSRAD`/`PSLLD`
   to `sshr`/`shl`, `UNPCKL` to `zip1`, `MOVLH` to `ins`. `SHUFIR` (20 sites)
   needs its immediates audited; each distinct pattern gets a fixed lowering.
5. **Executable memory.** The code buffer is a plain RWX `mmap` today, which
   Apple Silicon refuses. Allocate it with `MAP_JIT`, flip write protection
   around each compile (`VirtMem::protect_jit_memory`), and flush the
   instruction cache after each block. Write protection is per thread, so the
   flip has to happen on whichever thread compiles. RetroArch's macOS build
   already carries the `allow-jit` entitlement (checked on 1.22.2).

## Verification

- **Differential harness (new).** A host-side test that, for each handler,
  builds a one-instruction block, runs it on randomised MPE state through the
  JIT and through the matching `Execute_*` interpreter handler, and compares
  the full register file and flags. This is the main correctness tool and is
  what makes the flag work tractable.
- **Bundled test disc.** `NUON.CD` runs the instruction set through the
  compiler as well as the interpreter; "all compiler instruction test
  iterations passed" must still appear.
- **Games.** Tempest 3000 boots to gameplay; cycles per frame and fps are
  recorded against the baseline above. At least one other title is checked for
  regressions.
- **x86-64 unchanged.** The Linux x86-64 build must produce the same code as
  before; CI's existing jobs cover that it still builds.

## Milestones

| # | Deliverable | Done when | Rough effort |
|---|---|---|---|
| 1 | Build and plumbing: CMake option, asmjit AArch64 sources, `MAP_JIT` buffer, block prologue/epilogue, labels, `CALLI`, gates in `mpe.h`/`SuperBlock.h` behind a runtime switch | An empty block and a helper-call block execute on the M4; interpreter remains the default | 2-3 days |
| 2 | Integer primitives without flags: moves, memory forms, logic, shifts, `BSWAP`, `LEA`, multiplies, sub-registers | Differential harness passes for handlers that produce no flags | 4-5 days |
| 3 | Flags: tracker, `SETcc`, `JCC`, `CMOVcc`, `ADC`/`SBB`/`RCR`, shift carry-out | Harness passes for all ALU, MUL, ECU and RCU handlers | 4-6 days |
| 4 | SSE-backed handlers and the rest of MEM | Harness passes for every handler; test disc compiler tests pass; Tempest 3000 boots; first fps number | 3-4 days |
| 5 | Stabilise and measure: more games, cache flush and invalidation paths, enable by default on macOS Arm64 | No regressions against the interpreter in the games tried; README updated | 2-3 days |
| 6 | Optional: profile and hand-write native Arm64 for the hottest handlers | Decided from Milestone 4/5 numbers | Open-ended |

That totals roughly three to four working weeks for a developer. Milestone 4
is the decision point: if Tempest 3000 is still far from playable there, the
remaining gap has to come from Milestone 6 or from changes outside the
emitter (for example block linking or running MPEs on separate threads).

## Scope

- In: macOS Arm64 (libretro core). Linux and Android Arm64 should come along
  cheaply since only the memory-protection calls differ, but they are tested
  after macOS works.
- Out: iOS and tvOS (no JIT for ordinary apps), 32-bit Arm, a native macOS
  standalone app, and any change to the x86 emitters.

## Risks

- **Speed-up is smaller than hoped.** Translating x86 idioms costs extra Arm
  instructions, especially flag materialisation into the NUON `cc` register.
  Mitigation: measure at Milestone 4 before investing in polish.
- **Flag cases the audit misses.** Mitigated by the debug assert in the
  tracker and by the differential harness.
- **Cross-MPE timing.** The x86 JIT already needs `bMPE3PacketHack` for
  Tempest 3000; different block timing on Arm could expose similar livelocks.
- **Handlers that assume x86 details** beyond the primitives (stack layout,
  pointer truncation to 32 bits in displacements). Found so far: the four
  direct asmjit uses in `EmitMEM.cpp`. More may surface in Milestone 2.
