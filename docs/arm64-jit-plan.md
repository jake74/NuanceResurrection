# Arm64 JIT backend: plan

Status (2026-11-15): milestones 1 to 4 are done. Every block Tempest 3000 and
the test disc ask for compiles to native code, Tempest 3000 runs about three
times faster than on the interpreter, and the differential test finds no
difference from the interpreter across arithmetic, memory and branch
instructions. The emitter is still off by default until someone has played
through real games with it (milestone 5). See [Progress](#progress).

## Goal

Run the dynamic recompiler natively on Arm64, starting with macOS on Apple
Silicon, so that games stop being interpreter-bound.

Baseline on an M4 Pro (interpreter, libretro core in RetroArch): Tempest 3000
runs at about 5 fps and executes roughly 55-58 million packets per second
across the four MPEs, 13-16 million each. Real hardware runs each MPE at
54 MHz, so the interpreter delivers about a quarter of real time. The number
comes from `NUANCE_PERF=1`, which logs packets per second every 300 frames.

Tempest 3000 needs roughly 4-5x. About 40% of core CPU time is in
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

A primitive without an Arm64 body fails the block being compiled, and the
packet the block starts with is left to the interpreter. So the backend can be
filled in one primitive at a time, every block that does compile is made only
of primitives that exist, and the core stays usable throughout.

On Arm64 a block is native or it is not a block. The x86 builds turn blocks
they cannot emit natively into IL blocks (pre-decoded handler lists); Arm64
does not, because Tempest 3000 fails during boot when every block is an IL
block (see Progress).

### Code layout

- `src/nativecodecache_a64.cpp` holds the Arm64 versions of every
  `NativeCodeCache::X86Emit_*` method, most of them still one-line
  "unsupported" stubs. The x86 bodies in `nativecodecache.cpp` are compiled
  out on Arm64, so there are no per-primitive `#ifdef`s.
- `src/a64_emit.h` declares the backend's entry points (the Arm64 counterpart
  of `asmjit_emit.h`).
- `EmitMEM.cpp` uses asmjit x86 directly in `Emit_LoadBankPtr`. On Arm64 that
  is marked unsupported for now; it becomes a primitive of its own in
  milestone 2 so handlers stay architecture-neutral.
- Build: `NUANCE_ARM64_JIT` (CMake, on by default for AArch64 targets other
  than iOS/tvOS) defines `USE_A64_JIT`.
- Runtime: `[Arm64NativeCode]` in `nuance.cfg`, or `NUANCE_ARM64_JIT=1` in the
  environment. Off by default.

### Register mapping

| x86 | Arm64 | Why |
|---|---|---|
| `eax`, `ecx`, `edx` | `w9`, `w10`, `w11` | Caller-saved on both sides; handlers already assume calls clobber them |
| `ebx`, `ebp` | `w19`, `w20` | Callee-saved on x86-64, so handlers rely on them surviving calls |
| `esi`, `edi` | `x21`, `x22` | Register-file base pointers; callee-saved means no save/restore around calls |
| `r15` scratch | `x16`, `x17` | Addresses and immediates |
| `xmm0`-`xmm7` | `v16`-`v23` | Caller-saved, like XMM under System V |

The backend also keeps `w13`-`w15` as temporaries within one primitive, `w8`
for results it throws away, `w12` for the x86 carry flag when that has to
live in a register, and `v24`-`v26` as vector scratch.

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
| 1 (done) | Build and plumbing: CMake option, asmjit AArch64 sources, `MAP_JIT` buffer, block prologue/epilogue, labels, `CALLI`, gates in `mpe.h`/`SuperBlock.h` behind a runtime switch | An empty block and a helper-call block execute on the M4; interpreter remains the default | 2-3 days |
| 2 (done) | Integer primitives without flags: moves, memory forms, logic, shifts, `BSWAP`, `LEA`, multiplies, sub-registers | Differential harness passes for handlers that produce no flags | 4-5 days |
| 3 (done) | Flags: tracker, `SETcc`, `JCC`, `CMOVcc`, `ADC`/`SBB`/`RCR`, shift carry-out | Harness passes for all ALU, MUL, ECU and RCU handlers | 4-6 days |
| 4 (in progress) | SSE-backed handlers and the rest of MEM | Harness passes for every handler; test disc compiler tests pass; Tempest 3000 boots; first fps number | 3-4 days |
| 5 | Stabilise and measure: more games, cache flush and invalidation paths, enable by default on macOS Arm64 | No regressions against the interpreter in the games tried; README updated | 2-3 days |
| 6 | Optional: profile and hand-write native Arm64 for the hottest handlers | Decided from Milestone 4/5 numbers | Open-ended |

That totals roughly three to four working weeks for a developer. Milestone 4
is the decision point: if Tempest 3000 is still far from playable there, the
remaining gap has to come from Milestone 6 or from changes outside the
emitter (for example block linking or running MPEs on separate threads).

## Progress

### Milestone 1 (done, 2026-11-14)

- Build option, asmjit AArch64 sources, `MAP_JIT` code buffer with per-thread
  write/execute switching around block compilation.
- Block prologue/epilogue, helper calls, labels and forward jumps, 32-bit
  moves between registers, memory and immediates; everything else is a stub.
- A self-test at core start (when the switch is on) runs an empty block, a
  helper call, a forward jump and an unsupported-primitive block. It passes on
  the M4 Pro inside RetroArch.
- With the switch on, real blocks made only of the implemented primitives
  already run natively: 3,400 on the test disc, which still passes, and a few
  hundred in Tempest 3000, which still boots. Speed is unchanged, as expected.
- `NUANCE_PERF=1` also logs how many blocks compiled and which primitives
  stopped the rest. For Tempest 3000 the top of that list is `ANDIR`, `BSWAP`,
  `SUBIR`, `ANDIM`, `ADDMR`, `SUBMR`, `TESTIR`, `ADDIR`, `LEA`, which is the
  order to implement them in.

Things milestone 1 turned up:

- **The code buffer was never allocated on Apple Silicon.** The `VirtualAlloc`
  shim asked for `MAP_32BIT`, which macOS defines but cannot satisfy on arm64,
  so the buffer was null and every hot packet retried a compile that could not
  succeed. Fixed; the buffer also falls back to plain read/write memory where
  executable memory is refused.
- **IL-only block compilation breaks Tempest 3000.** Once the buffer existed,
  `[DynamicCompiler] Enabled` made every block an IL block, and MPE3 ran into
  illegal memory addresses during boot, even with blocks capped at one packet
  and constant propagation off. Not investigated further. Arm builds now
  interpret every packet unless the native emitter is switched on, which is
  what the source already said they did. This also changes Linux and Android
  Arm64 builds, where IL blocks had been in use; that is untested there.
- The x86-64 JIT build still compiles and links (cross-compiled here, not run).

### Milestones 2 and 3 (done, 2026-11-15)

Done together: an x86 arithmetic primitive and its flags are one AArch64
instruction, so there was nothing to gain from two passes.

- All integer primitives the handlers use: moves, arithmetic and logic in
  their register, immediate and memory forms, shifts and rotates, double
  shifts, multiplies, `BSWAP`, `BSR`, `LEA`, `SETcc`, `CMOVcc`, `Jcc`, and 8-
  and 16-bit sub-registers.
- Flags are tracked while emitting (`FlagState` in `nativecodecache_a64.cpp`):
  which of N, Z and V the last operation produced, and whether the carry is in
  the AArch64 C flag as x86 would have it (after an addition), inverted (after
  a subtraction), or in a register (after `RCR`). `SETcc`, `Jcc`, `CMOVcc`,
  `ADC`, `SBB` and `RCR` pick their condition from that, and fail the block if
  they need a flag the tracker cannot vouch for. Nothing in the handlers
  currently does.
- Constants that do not fit 31 bits are handled in 64 bits, as the x86-64
  emitter does, because some are host pointers.
- The differential test (`src/a64_difftest.cpp`, run by
  `tools/a64_difftest_host.c` without a frontend) compiles runs of one to four
  random packets and compares the 48 registers against the interpreter:

  ```
  cd build-libretro
  NUANCE_ARM64_JIT=1 NUANCE_A64_DIFFTEST=200000,1 ./a64_difftest_host ./nuance_libretro.dylib ../NUON.CD
  ```

  (trials, then seed; about a minute and a half per 200,000 trials on an M4 Pro)

  At this stage it covered ALU, MUL and RCU instructions: 700,000 trials over
  seven seeds without a mismatch. Milestone 4 extended it.

Emitter bugs the test found, present on x86 too and fixed here:

- `mul_sv`/`mul_p` with `>>svshift` used `esi` as a temporary and then stored
  through it before putting the register-file pointer back (six handlers in
  `EmitMUL.cpp`).
- `butt` wrote its second result before reading its first source for the last
  time, so `butt r9, r21, r8` computed with the wrong r9 (`EmitALU.cpp`).
- `DecompressPacket` used uninitialised stack as a handler index for encodings
  the decoders do not recognise. It now zeroes its scratch entry.

### Milestone 4 (done, 2026-11-15)

- Vector primitives are implemented: the 128-bit and scalar moves, `PAND`,
  `PADDD`, `PSUBD`, `PMULLD`, `PHADDD`, `PSRAD`, `PSLLD`, `PSHUFB`, `SHUFPS`,
  `UNPCKLPS`, `MOVLHPS`. Arm builds report SSSE3 and SSE4.1 as absent, so the
  handlers take their integer paths and most of these are not exercised yet.
- Every block now compiles. Test disc: 6,601 native blocks, none left to the
  interpreter, all compiler tests pass. Tempest 3000: about 4,000 native
  blocks, 2 left to the interpreter, boots to its title menu.
- Tempest 3000 on an M4 Pro, at the title menu (`NUANCE_PERF=1`):

  | | Packets per second | `VidSync` calls per second |
  |---|---|---|
  | Interpreter | 33-37 million | 3.2-3.4 |
  | Arm64 JIT | 80-110 million | 9.6-12 |

  `VidSync` calls are the nearest thing to the game's frame rate the emulator
  can see. MPE0-2 run about three times faster; MPE3, which the packet hack
  holds to one packet per block in system-bus code, does not speed up at all
  (about 9 million packets per second either way) and is now what limits the
  game.
- The differential test now builds whole packets of up to four instructions
  across the encoding space, including memory instructions (loads, stores,
  pixel and bilinear addressing, push and pop) and branches with their delay
  slots, and runs both sides through the real dispatcher. It compares the 48
  registers, the stack pointer, where execution ended up, and the memory the
  run touched. Of the 216 instruction handlers that have an emitter it
  exercises 206. The ten it does not: three that access control registers
  (left out because they reconfigure the machine under the test), two that
  only constant propagation produces, four that no decoder ever generates,
  and the ECU no-op.
- Result, ten seeds of 200,000 trials each: 1,801,064 runs compared, 664,042
  of them with memory access and 842,799 with a branch (447,967 taken), and
  no mismatch. The rest of the 2,000,000 trials were not compared: about 9%
  reached memory outside what the test controls or halted the processor, and
  about 0.5% contained a branch the block fetcher will not compile.
- What the test leaves out on purpose, because the instruction set does not
  define the result and the interpreter and the emitters each do something of
  their own (on x86 as well): a multiply whose shift field asks for a left
  shift by 64; `butt` with r31 as destination; byte, word, scalar and vector
  access through (xy)/(uv) addressing at an unaligned address or under a
  sub-byte pixel type; two instructions in one packet writing the same
  register or flag, or belonging to the same execution unit; and vector
  operands that are not a multiple of four, for which the decoder's
  dependency information does not match what the handlers touch.

### Milestone 5 so far, and first speed work (2026-11-15)

- Tempest 3000 has been played with a controller on the JIT and plays well.
  Other games are still to be tried.
- Sound was choppy. That was not the JIT: the libretro core delivered about
  29,500 audio frames a second where playback consumes 32,000, on the
  interpreter as well, because each period was scheduled from when the
  previous one happened to be pushed. It now keeps to the schedule and
  delivers what is needed (`NUANCE_PERF=1` logs both numbers).
- A profile showed the dispatcher costing as much as the generated code, and
  about one dispatch in three going to the interpreter. Three changes:
  - **Chaining.** When the block that follows is already compiled, the
    dispatcher runs it directly, up to 32 packets per scheduler step
    (`NUANCE_A64_CHAIN=<packets>`, 0 to turn it off). MPE3 is left alone
    while the packet hack is on.
  - **Texture-base stores compile.** Writes to `xybase`, `uvbase` and
    `clutbase` fell through to "never compile" in the decoder's table,
    although the emitter has always handled them; Tempest 3000 sets `uvbase`
    inside its drawing loops.
  - **Plain control-register access in delay slots.** A branch whose delay
    slots load or store plain machine state (`st_s #0, ru` and the like) is
    no longer kept out of native blocks. Registers with side effects are
    still excluded, by the decoder.

  The last two are in code shared with the x86 builds.
- Tempest 3000, same scene as the table above, auto-pressing START:

  | | Packets per second | `VidSync` calls per second |
  |---|---|---|
  | Interpreter | 32-33 million | 3.1 |
  | JIT before these changes | 78-80 million | 9.5 |
  | JIT now | 102-105 million | 13.2-13.6 |

- The differential test now also covers loads and stores of the control
  registers that are plain state (207 of 216 handlers per 200,000 trials), and runs with
  chaining off so each trial is exactly one block. Ten seeds, no mismatch.
- What is still interpreted, by frequency in Tempest 3000: delayed branches
  whose delay slots hold another branch (`cmp`/`bra` chains) or an indirect
  load, the two packets after each of those, and DMA and comm-bus register
  access. The first of these is the next candidate, and needs care: the
  rules for a branch inside a delay slot are subtle.

Testing aids added along the way, all environment variables:
`NUANCE_A64_WATCHDOG=1` reports where the emulation is if frames stop for five
seconds, `NUANCE_AUTOPRESS=<frames>` presses START periodically so an
unattended run gets past prompts, `NUANCE_A64_DIFFTEST_TRACE=1` names each
block before the differential test runs it, and
`NUANCE_A64_DIFFTEST_DUMP=<trial>` prints that trial's generated code as hex
for a disassembler.

One thing to know when testing from a shell on macOS: RetroArch started as a
background process stops being given frames after a few seconds. Start it
with `open -n -a RetroArch --env NUANCE_ARM64_JIT=1 --args ...` instead.

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
