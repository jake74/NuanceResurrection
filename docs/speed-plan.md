# Speed plan: busy scenes in Tempest 3000

Status: proposed, 2026-11-17. Nothing here is started. Written from the code on
branch `macos-arm64` and from [arm64-jit-plan.md](arm64-jit-plan.md), mainly its
section "Speed in busy scenes: what was measured (2026-11-16)". References are
`file:line` in this repository. Where a number is mine and not from the
documents it is called an estimate and its reasoning is given.

Written by an AI coding assistant reading the code without running it, and
spot-checked by a second pass against the source (the chain loop, the round
over the MPEs, the MPE3 packet limit, the cache-flush loop, the save-state
stubs and the stale-mark test all read as described). Nothing in it has been
measured: section 7 lists what is unverified.

## 1. Summary

The busiest scenes of Tempest 3000 run at about 18-26 frames a second on the
Mac mini M4 Pro and the Xiaomi Pad Mini; the aim is 30. The 2026-11-16
measurements put 36-46% of the emulation thread's time in "dispatcher and the
rest", 44-56% in generated code and 8-10% in the interpreter, and name two
remaining routes: compile the packets that are still interpreted, then link
blocks directly. Reading the code changes the picture in three ways.

1. "Dispatcher and the rest" is not only the dispatcher. The sampler state it
   is read from is zero whenever the thread is outside a block and outside the
   interpreter, so it also holds BIOS handler calls, the compiler itself,
   overlay hashing, cache-flush loops, the comm bus, timers, and the
   media-callback tick (section 2.2). Nobody has yet measured how much of the
   36-46% is block-to-block transition.
2. Direct linking can only remove the cost of going from one block to the next
   inside a turn (a "chain step"). It cannot remove a turn, that is the
   return to the round-robin in `src/libretro.cpp:1058`, and the code already
   chains up to 32 packets a turn (`src/mpe.cpp:2179-2233`). How much linking
   can give depends on how many chain steps there are per turn and why turns
   end. That ratio has never been measured; it is the first thing to measure.
3. The frame rate may follow the round rate for a structural reason: MPE3 gets
   one packet per round (section 2.4). That makes the cost of a whole round,
   not only of generated code, the quantity to reduce.

So this plan puts a cheap measurement stage first (Stage 0, about three days
including the Pad), and makes Stages 1 and 2 conditional on its numbers.

Reaching 30 from 22 needs the thread to be 1.36 times faster, from 26 needs
1.15 times (assuming frame rate scales with thread busy time, see section
3.4). The table gives each stage's expected share.

| Stage | What | Expected gain (basis) | Effort | Number that decides |
|---|---|---|---|---|
| 0 | Counters for dispatch events, a finer time split, a repeatable saturated benchmark, four no-code experiments (section 3.2) | none; it decides the rest | 2-3 days Mac, 0.5 day Pad | see below |
| 0b | Small fixes the counters may justify: relaxed test before the atomic exchange per round, page-skipping `FlushRegion`, base register for emulator variables (section 6) | each 0.5-3% (estimate: instruction counts) | 0.25-1 day each | the counter that names it |
| 1 | Compile the packets still interpreted: indirect memory in delay slots, branches in delay slots, side-effect control registers as end-of-block helper calls (section 4) | x1.06-1.14 (estimate: two thirds of the interpreter's 8-10% share, plus the turns it causes) | 6-10 days | (interpreted packets a second x cost of a turn) + interpreter share, as a share of thread time: go above 8% |
| 2a | Direct links between blocks, lazy and patched, static exits only (section 5) | x1.08-1.25 (estimate, low confidence: chain-step share plus per-block prologue and epilogue) | 6-9 days | chain-step share + prologue/epilogue share of thread time: go above 15% |
| 2b | Indirect exits (`rts`, `jmp`) through an inline cache | not estimable before Stage 0 (needs the exit-kind counters) | 2-3 days | share of chain-ending exits that are indirect |
| 2c (alternative to 2a) | Cache the successor in the block entry, in C++, no patching | x1.03-1.06 (estimate) | 1 day | same as 2a, with threshold 8% |

Recommendation in brief: do Stage 0 first. If chain steps and the interpreter
together are under about 8% of thread time, neither stage can reach 30 on its
own and the answer is elsewhere (section 6: longer turns, wait-loop skipping,
or MPE threads).

## 2. Where the time goes today

### 2.1 The measured split

From the 2026-11-16 section of arm64-jit-plan.md, taken with the perf log's own
sampler:

| | Mac, no limits | Pad, slow cores (`taskset 0f`) |
|---|---|---|
| Generated code | 56% | 44% |
| Interpreter | 8-9% | 9-10% |
| Dispatcher and the rest | 36% | 46% |

Saturated, MPE0 has 43% of the thread's time, MPE1 21%, MPE2 18%, MPE3 18%
(same section). The same section records five scheduling experiments that gave
nothing on the saturated Pad, among them giving MPE0 one turn in two, four or
eight (frame rate 16, 15, 11, 8 from 16 at every turn).

### 2.2 What each sampler state contains

The sampler is a thread that reads `a64DebugBlockRunning` every
`usleep(100)` and counts it in `perfState[state & 3]`
(`src/libretro.cpp:1186-1200`). The variable is a plain volatile
(`src/nativecodecache_a64.cpp:1800`) written by the emulation thread:

| State | Set at | Covers |
|---|---|---|
| 1 generated code | `src/mpe.cpp:2163` and `:2228`, cleared at `:2168` and `:2231` | everything from the call instruction to the return, so the block's prologue and epilogue (`src/nativecodecache_a64.cpp:833-853`) are in this state |
| 2 interpreter | `src/mpe.cpp:2260`, cleared at `:2264` | `ExecuteNuances` for one packet; this includes the DMA and comm work that control-register stores start, because those stores are never compiled |
| 3 asleep | `src/libretro.cpp:1080-1082` | the 50 microsecond sleep |
| 0 everything else | the default | see below |

State 0 therefore holds: the top of `FetchDecodeExecute` (`src/mpe.cpp:1852-2118`),
the chain loop between blocks (`:2182-2232`), BIOS and Presentation Engine
handler calls (`:2267-2289`, these run C++ and are in no state of their own),
`TickMediaCallbacks` (`:2340`), the compiler (`CompileBlock`, `:2036`: asmjit
assembler construction and emission, `src/nativecodecache_a64.cpp:688-770`),
overlay hashing (`OverlayManager::FindOverlay`, `:1937`), cache-flush loops
(`:1951-1957`, `:1974-1979`), and the round level in `RunEmulation`: the comm
bus (`src/libretro.cpp:1074-1075`), the clock, timer and audio block
(`:1077-1166`) and `TriggerScheduledInterrupts` (`:1168`). The docs'
conclusion that this share is "a chain coming back through the dispatcher" is
therefore an assumption, not a measurement. Stage 0 splits the state (section
3.3).

### 2.3 From the end of one block to the start of the next, one MPE

A block ends (`src/SuperBlock.cpp:448-453`) with a store of the exit address to
`pcexec`, the epilogue and `ret`. A conditional or unconditional branch has
already stored its target in `pcfetchnext` and set `ecuSkipCounter` to 1
(`src/EmitECU.cpp:213-232`, `:258-279`); the exit address is the address after
the branch's two delay slots (`src/SuperBlock.cpp:1173`). Because the branch and
both delay slots are inside the block, the dispatcher only has to resolve the
counter once.

Path A, the chain loop (the common case once a turn has started,
`src/mpe.cpp:2182-2232`). Each iteration does, in order:

1. If `ecuSkipCounter` is set: stop unless it is 1, else clear it and copy
   `pcfetchnext` to `pcexec` (`:2184-2191`).
2. Five loads and tests of state native blocks cannot change in any way the
   test would notice (argued in section 5.2): `mpectl`, `excephalten & excepsrc`,
   `bInvalidateInstructionCaches`, `interpretNextPacket` (`:2193`), and the
   interrupt test on `intsrc`, `intctl`, `inten2sel`, `inten1` (`:2195-2204`).
   Only `interpretNextPacket` can change inside a block (the mid-block bail,
   `src/EmitMEM.cpp:431`, `:742`, `:1169`, `:1489`).
3. Breakpoint compare (`:2210`). `breakpointAddress` is set only by the
   Windows and Linux shells (`src/NuanceMain.cpp:452`), not by the libretro
   build, so it is zero there (not verified: it relies on `nuonEnv` having
   static storage).
4. IRAM test: is `nextPc` inside local memory, then is it inside the
   invalidated region, then OR in `overlayMask` (`:2213-2218`).
5. `pageMap.FindEntry(lookup)`: three dependent loads (root, level 1, level 2;
   `src/PageMap.h:67-84`), then compare `virtualAddress` and `compileType`
   (`:2220-2222`). A level-2 page is 1024 entries of 32 bytes (my arithmetic:
   28 bytes of fields, `PageMap.h:9-23`, padded) indexed by `address & 0x3FF`,
   that is one entry per byte address, so neighbouring blocks do not share
   cache lines.
6. Store `pcexec`, add to `cycleCounter`, `packetsRun`, store
   `a64DebugBlockPc`, `a64DebugBlockRunning`, and increment
   `a64PerfNativeBlocks[mpe]`, all unconditionally (`:2224-2229`). These
   counters and debug stores are paid with `NUANCE_PERF` off.
7. Indirect call through `pNext->entryPoint` (`:2230`), then the block's
   prologue: `stp` x3, two 64-bit immediates for the register-file pointers
   (`src/SuperBlock.cpp:386-387`, `LoadImm64` is up to four instructions,
   `src/nativecodecache_a64.cpp:143-158`), the `pcfetchnext` store if the block
   has a branch (`:389-392`; address as a 64-bit immediate then a store, because
   absolute operands are never register-relative, `:240`), body, the `pcexec`
   store (`:448`), epilogue, `ret`.

   By instruction count that is roughly 25-30 instructions of fixed cost in
   the block and, in the loop, about 20 loads, 15 conditional branches and 8
   stores (my count from the lines above). It is an estimate, not a
   measurement.

Path B, the start of a turn (`FetchDecodeExecute`, `src/mpe.cpp:1852`). In order:
`cycleCounter = 0` and the debug store (`:1854-1856`); `mpectl` test
(`:1858`); the interrupt test again, this time taking the interrupt if due
(`:1872-1906`); the IRAM and invalidated-region test, with overlay hashing
when the region is dirty (`:1924-1968`); otherwise the `bInvalidateInstructionCaches`
test (`:1971-1982`); `FindEntry` again (`:1992`); if no block, the instruction
cache lookup, frequency count and possibly a compile (`:2009-2104`); then
`cycleCounter += numPackets`, the call (`:2123-2166`), the chain loop, the
ecuSkipCounter decrement (`:2294-2330`), `TickMediaCallbacks` (`:2339`), a
breakpoint compare and the halt test (`:2342-2347`). It is a very large member
function, so its own prologue and epilogue are probably not small (not
verified; no disassembly was read).

Path C, one interpreted packet: a whole turn of path B, then `ExecuteNuances`
with its register copy (`src/mpe.h:424-471`). Nothing of the chain survives
it: the next block is entered through path B.

### 2.4 The outer round

`RunEmulation` (`src/libretro.cpp:1007`) repeats: compute `bOthersRun`
(`:1056`); for MPE3 down to MPE0 call `FetchDecodeExecute` if the MPE has
credit (MPE3: only if the other three do and its video wait is zero,
`:1058-1065`); add the packets it ran to its credit (`:1063`); `DoCommBusController`
if requests are pending (`:1074`); every 5000 rounds, or on an idle round, read
the clock, top up credits, and test timers, video field and audio
(`:1077-1166`); and `TriggerScheduledInterrupts` every round (`:1168`). The last
is an atomic exchange with sequentially consistent ordering on every round
(`src/NuonEnvironment.h:185-192`, `src/linux_compat.h:187-189`). It could be a
plain load first. It is a small saving per round, but there is a round for every
three or four turns.

Credits: 54 million per second per MPE (`:1041`), topped up in 4 ms at most
(`:1095-1099`). Halted MPEs keep their credit, so they are polled every round
and return at `:2355` almost at once.

A consequence worth testing: MPE3 takes at most one turn per round
(`:1059`), and in system-bus code, with the packet hack on (the default,
`nuance.cfg:36-37`; `src/SuperBlock.cpp:1007-1013`), one packet per turn, with
chaining off for MPE3 (`src/mpe.cpp:2179`). While it is in such code its packets
per second equal the number of rounds per second. The doc records 7-8 million
MPE3 packets a second on the Mac after the limit went in ("Too fast: a limit
at hardware speed"); if that holds in busy scenes the Mac runs about 7.5
million rounds a second, about 130 ns each, for about 150 million packets a
second from MPE0 to MPE2, so about 20 packets a round and 7 a turn. These
are estimates from figures taken in different sessions. If they are right,
the average turn is short, and the number of turns, not of chain steps, would
be what costs. A direct round counter (3.3) settles it.

### 2.5 What chaining does today, exactly, and where it stops

Chaining is the loop in 2.3 path A. It runs the next block if all of these hold
(`src/mpe.cpp:2182-2222`): fewer than `a64ChainPackets` (32,
`src/nativecodecache_a64.cpp:1801`; `NUANCE_A64_CHAIN`, `src/libretro.cpp:929`)
packets have run in the turn, counted from the first block; nothing the top of
the function would act on is pending; the next address has a valid native block
under the current overlay. It stops at the first miss. It is skipped for MPE3
while the packet hack is on (`:2179`). It does not run interpreted packets,
BIOS calls, or compile: any of those ends the turn, and the next block is found
by path B on the next round. Taken branches are followed (step 1 above), so a
loop of blocks chains. The turn's packet count is the `cycleCounter` the round
reads (`src/libretro.cpp:1061`).

### 2.6 Which costs linking could remove and which not

| Cost | Removed by direct linking | Why |
|---|---|---|
| Path A items 1-5 and the debug and counter stores | Yes, replaced by a budget check and a branch | the work is done by the exit and the link |
| Prologue and epilogue (stack frame, register-file pointers, immediates) | Yes, if the successor is entered inside the predecessor's frame | same MPE, same pointers (`x21`, `x22`) |
| The `pcexec` and `pcfetchnext` stores at each block | Partly (only needed when leaving to the dispatcher) | see 5.1 |
| Round-robin, credit accounting, `TriggerScheduledInterrupts`, `DoCommBusController` | No | once per round, not per block |
| Path B (turn start) | No | one per turn |
| Interpreted packets, BIOS calls, compile, flush, overlay hashing | No | not block-to-block transitions |
| Media tick (`src/media.cpp:156-160`) | No | once per turn (and see the risk in 5.2) |

## 3. Stage 0: measure before building

Stage 0 must be cheap and must answer, with numbers from real play: how many
turns and chain steps there are, what a turn costs, why turns end, and how much
of the "dispatcher and the rest" is something else.

### 3.1 A repeatable busy-scene benchmark

What the code offers:

| Approach | What it takes | Verdict |
|---|---|---|
| Save state (`retro_serialize`) | `retro_serialize_size`, `retro_serialize` and `retro_unserialize` return 0 or false (`src/libretro.cpp:1630-1632`). The state to save: main and system RAM (8 MB each) and flash (`NuonEnvironment.h`); per MPE the register file, control registers, 8 MB of local memory and the emulator variables (`src/mpe.h:267-396`); the video module (about fifty file-level variables, by a rough count of `src/video.cpp`); the media module's queues and tick counters (`src/media.cpp:68-104`, includes a `std::deque` per MPE); timers, comm bus, audio, DMA and BDMA state; the `RunEmulation` statics (`src/libretro.cpp:1017-1021`); the code and instruction caches must be flushed on restore (`MPE::Reset`-style, `src/mpe.cpp:855-860`). The emulation thread must be parked, which `EmuPause` already does (`:1265`). | Valuable beyond benchmarking (RetroArch save states) but 4-7 days by my estimate and easy to get subtly wrong. After a restore the game still is not deterministic, so it gives a start point, not a replay. Defer. |
| Existing dumps | `NUANCE_DUMP_RAM` writes main and system RAM (`src/libretro.cpp:1357-1391`), `NUANCE_SHOT_RAM` adds the four local memories (`:1600-1607`). No loader, no registers. | Not a restore. |
| Start from a later level by script | `NUANCE_INPUT_SCRIPT` (`:179-232`, timed by the game's VidSync count, buttons and x stick only), `NUANCE_AUTOPRESS` (`:258-289`), `NUANCE_T3K_STATE` (`:1306-1316`). | Reaches later levels but, per the 2026-11-16 section, no script reaches a busy scene. |
| Record real inputs, replay | The timers, video field and audio period are driven by the host clock (`src/libretro.cpp:1116-1165`) and MPE speed by credits earned in host time, so the same inputs give a different game each time (the docs say the same). | A deterministic virtual clock would fix that but changes the thing measured. Not recommended. |
| Synthetic load | None exists. | Rejected: it would not have this game's mix of turns. |

Recommendation: two uses, two tools.

- To decide (Stage 0): one real play session of ten to fifteen minutes with
  `NUANCE_PERF=1` and `NUANCE_LOG_FILE=<path>` on the new counters (3.3). These
  are ratios and shares, so they need no repeatability. Jake plays; the log
  is read for the intervals where `VidSync` falls below 30 a second.
- To compare builds (Stages 1 and 2): the saturated scripted run that has
  already been used, so that its frame rate tracks thread throughput:
  `tools/perf_host.c` with `NUANCE_MPE_HZ=0` (no limit, the thread never sleeps;
  `src/libretro.cpp:1041`) on the Mac, and `taskset 0f` with the 30 frame cap
  on the Pad, as in the 2026-11-16 section (about 16 frames a second there).
  Before relying on it, check that it is representative: compare its
  per-MPE turn and chain-step profile with the real-play log from the first
  use. If chain steps per turn, packets per block and the end-of-turn
  histogram agree within about a fifth, use it. If they do not, that is the
  evidence that a save state is worth its cost.

Notes on the tools. `perf_host` is headless, so `NUANCE_SHOT` does nothing in
it (the screenshot code sits inside `if (gl_initialized)`, `src/libretro.cpp:1527`,
`:1560-1611`) and `NUANCE_T3K_STATE` and `NUANCE_T3K_TRACE` still work (they
read RAM, `:1301-1354`). `NUANCE_INPUT_SCRIPT` and `NUANCE_AUTOPRESS` work
headless because `GetInputButtons` is called from `retro_run`
(`:1297`). Do not use `NUANCE_AUTOPRESS` with START after the game has begun:
START pauses (the 2026-11-15 correction); use
`NUANCE_AUTOPRESS_START_UNTIL` (`:285`).

Statistic. The metric is mean frames a second (VidSync calls a second, the perf
log's "game VidSync") over five-second intervals, after discarding the first
60 seconds (compilation warm-up, an estimate; check against the log). Run in
alternating order, A B A B, at least six runs of each, 120 seconds, on both
machines; report the mean of run means, the spread between run means, and the
ratio of the two. The docs say runs differ by several percent; the sample
size is chosen so that a 5% difference is about two standard errors if the
run-to-run standard deviation is 3% (my arithmetic: 3%/sqrt(6) is 1.2% per
arm, 1.7% for the difference). If a measured spread is larger, add runs.
Pin the same cores and let the Pad cool between pairs; the doc found no
slowing over ten minutes, so thermal drift within a pair should be small.

### 3.2 Four experiments that need no new code

| # | Experiment | What it tells |
|---|---|---|
| E1 | Benchmark B1 with `NUANCE_A64_CHAIN` at 0, 8, 32 (default), 128 | Sensitivity of the frame rate to turn length. Chain 0 makes every block a turn; the difference in blocks per second against frame rate gives the marginal cost of a turn over a chain step. A large gain at 128 would also say that a longer turn budget is the cheapest lever (section 6). The cross-MPE interleaving then changes, so watch for hangs; MPE3 is unaffected (`src/mpe.cpp:2179`) |
| E2 | The existing perf log in real play: blocks a second and packets per block per MPE (`src/libretro.cpp:1454-1458`), interpreted packets, never-compilable packets and the four "why" counts (`:1459-1460`), overlay lookups, native and interpreter cache flushes per second (`:1464`) | Rough dispatch-event rate and the flush rate (which decides how Stage 2 invalidates links) before any code is written |
| E3 | `NUANCE_A64_SHOW_BLOCKERS=1` (`src/SuperBlock.cpp:1122-1131`, first 400) and the "interpreted" top-twelve list (`src/libretro.cpp:1444-1452`) | Which delay-slot blockers and which packets exist in T3K, for Stage 1 |
| E4 | A micro-benchmark in the self-test style (`A64_JitSelfTest`, `src/nativecodecache_a64.cpp:1950`): time 10 million calls of (a) an empty block, (b) a block with the two `pcexec`/`pcfetchnext` stores and a branch, from a loop in C++ | The fixed cost of prologue plus epilogue plus call, per device. It is the part of the generated-code share that linking can remove. Half a day; not an emulator run |

### 3.3 Counters

All counters are per MPE, plain `uint64` arrays declared in `src/a64_emit.h` next
to `a64PerfWhy` (`:55`), defined with it (`src/nativecodecache_a64.cpp:1802`),
incremented in `src/mpe.cpp`, printed and reset in the report at
`src/libretro.cpp:1454-1470`. To cost nothing with `NUANCE_PERF` off each
increment is guarded by the flag that already exists for the sampled
interpreter list, `a64PerfSampleInterpreted` (`src/libretro.cpp:1399`,
`src/mpe.cpp:2252`): a load and a predicted branch. (The three existing
unconditional counters at `src/mpe.cpp:2164`, `:2229`, `:2246` could go behind the
same flag; their cost is small and was never measured.)

| Counter | Where | Reported as |
|---|---|---|
| Turns that ran a native block | `src/mpe.cpp:2164` (top-level call only) | per second |
| Chain steps | `:2229` | per second, and per turn |
| Turns that ran an interpreted packet, split by `interpretNextPacket` set (a mid-block bail) or `ecuSkipCounter` set (delay slot of an interpreted branch) | `:2246-2250`; today bucket 0 of `a64PerfWhy` merges them | per second |
| BIOS and Presentation Engine handler calls | `:2271` and `:2283` (not counted anywhere today: `a64PerfInterpreted` is in the `pcexec < ROM_BIOS_BASE` branch only) | per second |
| Halted turns | `:2355` | per second |
| Why a turn's chain ended | a local `reason` set before each `break` in the loop, and after the loop for the budget case (`:2182-2222`): budget; `ecuSkipCounter` above 1; halted or exception; cache invalidate pending; `interpretNextPacket`; interrupt would be taken; breakpoint; IRAM region dirty; no valid block for next address; MPE3 under the hack (no chain) | histogram per MPE, share of turns |
| For "no valid block": what the next packet is | look up the instruction-cache entry for `nextPc` (`FindInstructionCacheEntry`) and classify: never decoded; `NEVERCOMPILE`; `COMPILED` mark but no block; below threshold | extends the above; flag-gated because it costs a lookup |
| Exit kind of every block exit, and taken or not | a small field in `NativeCodeCacheEntry`, set from `SuperBlock` where the branch packet is seen (`src/SuperBlock.cpp:1036-1204`; the ECU handler is `packet.handlers[]`, `src/InstructionCache.h`): fall-through (packet cap or uncompilable next packet), static branch (`bra`, direct `jsr`), indirect (`jmp reg`, indirect `jsr`), return (`rts`), `rti`, guarded `,nop` in slot 2. It fits in the four bytes of padding by my arithmetic (not compiled, check with `sizeof`); guard with `USE_A64_JIT` so the x86 layout is untouched | histogram of exits by kind and taken, per MPE |
| Packets per turn | `libretro.cpp:1061`, `ran` bucketed 0, 1, 2-3, 4-7, 8-15, 16-31, 32+ | histogram per MPE |
| Rounds, idle rounds | `cycles` at `src/libretro.cpp:1044` is a `uint32` local that wraps; keep a `uint64`; idle at `:1072` | per second and per game frame (VidSync) |
| Polling turns | the turn starts at the same `pcexec` as the MPE's previous turn | share of turns and of busy time per MPE (feeds the wait-loop question, section 6) |
| `FlushRegion` span | in `NativeCodeCache::FlushRegion` (`src/NativeCodeCache.h:48-51`) add `end - start + 1` | addresses walked per second. `PageMap::InvalidateRegion` loops once per byte address (`src/PageMap.h:56-65`). A system-bus and main-bus flush is 8 million iterations each (`src/mpe.cpp:1977-1979`, sizes `src/NuonMemoryMap.h:43-44`), and the leftover flush at `:1956` can span up to 2 MB when data RAM was written by DMA. At an estimated 1-3 ns an iteration (levels unallocated are cheap) a single such flush may cost a few ms to tens of ms. The perf log's "emulation stalled" line (`src/libretro.cpp:1089-1091`) would show the large ones |
| Compile events and their time | around `:2036` | compiles per second, state 5 below |

A finer time split. Widen the sampler state from two bits to three
(`& 3` becomes `& 7` at `src/libretro.cpp:1194`, `perfState[8]`) and add:

| State | Meaning | Set at |
|---|---|---|
| 0 | `FetchDecodeExecute` proper | default |
| 4 | BIOS or PE handler | `src/mpe.cpp:2271`, `:2283` |
| 5 | compile, flush, overlay lookup | `:1937`, `:1951-1957`, `:1974-1979`, `:2036` |
| 6 | chain loop between blocks | replace the store of 0 at `:2231` by 6, and restore 0 after the loop |
| 7 | round level | `src/libretro.cpp:1074-1075`, `:1077-1166`, `:1168` |

The only new stores are on rare paths; state 6 replaces an existing store.
The report prints the share of each and, for state 6, its nanoseconds per
chain step (share x seconds / chain steps) and, for state 0, nanoseconds per
turn. Caveats: the sampler reads a variable, not the program counter, so it
has no skid, but its tick is a `usleep(100)` (the real period on the Pad is
not known), so over a five-second interval it takes a few tens of thousands
of samples and each share has an error of about a percentage point.

### 3.4 The decision rule

Notation, all per second of emulation-thread busy time (sleep excluded):
`n_turn` native turns, `n_step` chain steps, `n_int` interpreted packets,
`s_k` the sampler's share in state k, `c_pe` the measured fixed cost of a block
(E4), `c_link` the cost of a linked transition (estimate 1-2 ns: a budget
check of about five instructions and a direct branch).

Assumption A1: the frame rate is inversely proportional to thread busy time.
It holds if the thread is saturated, which B1 guarantees and a busy scene
presumably does (the thread's sleep share is then about zero; the log prints
it). If the game is instead limited by a dependency chain through MPE3's one
packet per round, saving time in MPE0-2 only helps through the shorter round.
The round counter shows which.

Gain of linking, as a share of thread time (Stage 2a):

    D2 = s_6 + n_step * (c_pe - c_link) / 1 s      [chain-step overhead, plus
                                                    prologue and epilogue]

Frame rate becomes `fps / (1 - D2)`:

| D2 | 5% | 8% | 10% | 15% | 20% | 25% | 27% |
|---|---|---|---|---|---|---|---|
| Speed-up | x1.05 | x1.09 | x1.11 | x1.18 | x1.25 | x1.33 | x1.37 |

From 26 frames a second to 30 needs D2 of 13%, from 22 needs 27%, from 18
needs 40%. Rule: D2 under 8% is no-go for 2a (do 2c or stop); 8-15% do 2c
first and then 2a only if 2c left more than 8% of chain-step share; above 15%
go. The thresholds are my judgement, chosen so that a stage which cannot
reach 30 even in the better case (26) is not started.

Gain of compiling interpreted packets (Stage 1):

    D1 = s_2 * (1 - 1/3) + n_int * c_turn / 1 s + (turns that ended because
         the next packet is not compilable) * (c_turn - c_step) / 1 s

where `c_turn` is `s_0 / n_turns` after the split, `c_step` is `s_6 / n_step`,
and the first factor uses the docs' "about three times faster than the
interpreter" (2026-11-15 summary) as the native-to-interpreter cost ratio. Go
above 8%. If the "next packet not compilable" turn-ends are the largest part,
Stage 1 is also the prerequisite of Stage 2 (linking can only cover exits whose
target is a block).

Which stage first: if D1 and D2 are both above 8%, do the one with the
cheaper first step (2c is a day; Stage 1 item 1 is two to four days).

If neither reaches 8%, the numbers point at the other costs in 2.2 (BIOS
calls, compile, flushes, round level), or at turn length and MPE threads
(section 6).

## 4. Stage 1: compile the packets still interpreted

### 4.1 Every reason a packet is interpreted today

The reasons, with where each is decided:

| Reason | Decided at | Notes |
|---|---|---|
| R1. The decoder marks the packet `NEVERCOMPILE`: a control-register access with side effects (the `bException` result of `GetControlRegisterInputDependencies` and `...OutputDependencies`, `src/mpe.h:473-474`), set at `src/DecodeMEM.cpp:119-122`, `:135-138`, `:346-349`, `:692-695`, `:750-751`, `:962-965`, `:992-995`; vector control registers (comm), always `:750`; `halt` (`src/DecodeECU.cpp:273`) | decode | DMA control, comm bus, interrupt control, `mpectl`, `halt`. Docs: "DMA and comm-bus register access" (2026-11-15) |
| R2. The packet is in the BIOS jump-table range, or has a breakpoint | `src/mpe.cpp:2092-2094` | BIOS and PE calls beyond the table are run as C++ (`:2267-2289`), not interpreted |
| R3. Address at or above `ROM_BIOS_BASE` | `:2097-2102` | same |
| R4. Delayed branch whose first or second delay slot has an `ECU` instruction, a register-indirect memory access, or a `NEVERCOMPILE` packet (`DELAY_SLOT_BLOCKERS`, `src/SuperBlock.cpp:954`, test at `:1079`). The exception: a conditional branch with a `,nop` branch in slot 2 (`:1090-1098`; `NUANCE_A64_SLOT_BRANCHES`, `src/libretro.cpp:933`) | `FetchSuperBlock` | Three sub-cases R4a, R4b, R4c below. If the block already holds packets, the block ends before the branch and the branch becomes the next block's first packet (`:1100-1109`); there it makes `FetchSuperBlock` return no packets, `CompileBlock` returns -1 and the branch packet is marked `NEVERCOMPILE` for good (`:490-499`, `src/mpe.cpp:2056-2060`). Its two delay-slot packets are then interpreted through the skip-counter path |
| R5. A handler has no emitter, or a primitive failed on Arm64 | `src/SuperBlock.cpp:762`, `:522-527`, `:556-558`, `:459-466` | None known today; milestone 4 reports every block compiling (arm64-jit-plan.md). The perf log's "stopped by" list names any (`src/libretro.cpp:1442`) |
| R6. A delay-slot packet is interpreted because the branch before it was (counter set) or because a block bailed (`interpretNextPacket`) | `src/mpe.cpp:1989`, `:2250` bucket 0 | a consequence of R4, R1 and the mid-block bail for control-window addresses (`src/EmitMEM.cpp:429-433`, `:1163-1171`) |
| R7. Newly decoded (first run) | `:2083`, `:2250` bucket 1 | cold code |
| R8. Run fewer times than the threshold: 50 in system and main memory (`COMPILE_THRESHOLD`, `src/mpe.h:4`), 3 in local memory (`a64IramThreshold`, `src/nativecodecache_a64.cpp:1803`) | `src/mpe.cpp:2023` | bucket 3. Only the local-memory threshold has an environment variable (`NUANCE_A64_IRAM_THRESHOLD`) |
| R9. Marked compiled (`PACKETINFO_COMPILED`) but no block under the current overlay | `:2023` with `a64RecompileMarked` 0 | bucket 2. MPE0's wait loop. Tried and dropped (2026-11-15 and 2026-11-16) |
| R10. A compile failed for lack of room: flush and the packet runs interpreted this time | `:2061-2066` | rare |
| R11. Mid-block bail: an indirect load or store whose address is in the control or tag window leaves the block with `interpretNextPacket = 1` and `pcexec` at that packet | `src/EmitMEM.cpp:429-433`, `:740-743`, `:1163-1171`, `:1486-1490` | the packet is interpreted, the rest of the block is not run |

Comment/code mismatch to know about: the comment at `src/mpe.cpp:2014-2022` says
a packet with a stale `COMPILED` mark is now compiled. The code only does that
when `a64RecompileMarked` is non-zero (`:2023`), which defaults to 0
(`src/nativecodecache_a64.cpp:1803`). With the default, R9 packets are still
interpreted for good; the doc is right, the comment is not.

Packets that run on the host as C++ handlers (BIOS, PE) are not interpreted
and are counted nowhere today (3.3).

### 4.2 Missing emitter primitives

None. Every one of the 104 primitives the handlers use has an Arm64 body, and
every handler with an emitter compiles (arm64-jit-plan.md, milestone 4; the
perf log's "stopped by" list is the check). What the remaining cases need is
not new instructions but new block structure: an exit from the middle of a
block that leaves a branch pending (R4c, R11 in a slot), a runtime guard on
a whole packet (R4a), and a call into the interpreter from a block (R1).

### 4.3 The items, ordered by expected value

The order below is by what the docs record and by effort; Stage 0's counters
(why turns end, interpreted packets by cause) re-order it. No item has a
measured frequency in the documents beyond the 2026-11-15 list ("delayed
branches whose delay slots hold another branch or an indirect load, the two
packets after each of those, and DMA and comm-bus register access"), and the
2026-11-16 list (the same three, with the first two as "delayed branch in a
delay slot", "branches in the first delay slot" and "indirect memory access in
a delay slot").

**S1.1 Register-indirect memory access in a delay slot (R4c).** Frequency:
named in both lists. Cost: the access is already emitted with a
mid-block bail for addresses in the tag or control window (`src/EmitMEM.cpp:1163-1171`,
`:429-433`); what blocks it in a slot is that the bail leaves the branch pending
in the wrong state. The branch emitters write `ecuSkipCounter = 1`
(`src/EmitECU.cpp:229`, `:275`) because the dispatcher decrements once
(`src/mpe.cpp:2294-2297`); the interpreter sets 3 and decrements per packet
(`src/ExecuteECU.cpp:34-36`). A bail before slot 1 would need the counter to be
3 if the branch was taken, before slot 2 it would need 2, and in both cases
left alone if it was not; `pcfetchnext` is already stored. Emit that fix-up in
the bail path (6-10 instructions, cold). Why it is safe: the decoder is asked
for memory-first ordering inside a packet (`src/SuperBlock.cpp:958`), which
appears to be what makes the existing bail safe, since nothing in the packet has
run when the memory instruction leaves; not verified for every packet shape
(the scheduling code, `src/mpe.cpp:1047-1363`, was only skimmed). Risk:
moderate. Test: see below. Effort: 2-3 days with tests.

**S1.2 `,nop` branch or any non-delayed branch in a delay slot, slot 1 (R4a).**
Frequency: second in the docs' list. What is done today for slot 2 is a guarded
emission (the `bCheckECUSkipCounter` check, `src/EmitECU.cpp:223-226`, enabled
for one packet by `a64GuardedPacketPc`, `src/SuperBlock.cpp:407`, `:1097`). A
branch in slot 1 must be skipped if the first branch was taken (spec: ECU
instructions in the delay slots of a taken branch are not evaluated,
`src/SuperBlock.cpp:265-266`) and evaluated if not. That needs the guard on the
slot-1 packet, which exists, and then the target is resolved at the block
end as with slot 2. For a `,nop` branch in slot 1 nothing more is needed; for
a delayed branch in slot 1 see S1.3. Risk: moderate; the guard is exercised
today by the difftest (about 6,700 extra compared trials per 200,000,
arm64-jit-plan.md 2026-11-15). Effort: 1-2 days.

**S1.3 Delayed branch in a delay slot (R4a/R4b).** Frequency: first in the
2026-11-15 list ("cmp/bra chains"). If the first branch is not taken, the
second branch's own two delay slots are slot 2 and the packet after it, which
lies outside today's block; if the first is taken, the second is ignored. So the
block would have to be the union of both paths: after slot 2, if the first
branch was taken, leave with the first branch's target; otherwise continue
through the packet after slot 2 and end there. The existing counter cannot say
which branch set it (both leave it at 1), so the emitter needs a scratch flag
set where the first branch is taken. Highest risk: the docs call the rules for
a branch in a delay slot subtle (2026-11-15); interrupt blocking and the
`ecuSkipCounter` semantics both depend on it (`src/mpe.cpp:1869-1872`). The
x86 JIT does not compile these (it ends the block, `src/SuperBlock.cpp:1100-1133`),
so there is no reference implementation to compare with; only the interpreter.
Effort: 3-5 days, mostly tests. Do it last, or not at all if the counters show S1.1
and S1.2 cover most of R4.

**S1.4 Side-effect control registers as a helper call at the end of a block
(R1).** Frequency: named in the 2026-11-15 list. Today such a packet is
interpreted in a turn of its own. Instead, let the block run up to the packet,
call the interpreter on it through a helper (`MPE::ExecuteNuances`, wrapped in
a free function), set `pcexec` to the next packet and leave. The register file
the helper sees is the same memory the block used, since `esi` points at
`mpe->cc` (`src/SuperBlock.cpp:386`), so no state sync is needed beyond the
flags snapshot the interpreter does itself (`src/mpe.h:444-447`). The block must
leave after it, since it may start a DMA that dirties the overlay region or
raises an interrupt (`src/dma.cpp:829`, `:850`). Gain: one turn start per
occurrence, not the interpreter's work. Worth it only if the counters show many
such packets: it saves `c_turn`, roughly 15 ns by the section 2.4 estimate.
Risk: low (it runs the same handler). Effort: 2 days.

**S1.5 Threshold (R7, R8).** `COMPILE_THRESHOLD` (50) is a constant and not
settable at run time; add `NUANCE_A64_THRESHOLD` (trivial) and sweep with the
existing `NUANCE_A64_IRAM_THRESHOLD`. Cost is compile time, which the new state
5 measures. Effort: 0.25 day. R9 stays as the docs left it.

### 4.4 How the difftest covers these

The difftest (`src/a64_difftest.cpp`) builds runs of one to four packets
(`kMaxPacketsPerRun`, `:56`), places the code in main-bus memory at `kCodeBase`
(`:51`), and compares the 48 registers, the stack pointer, where execution
ended and the memory touched. It runs with chaining off (`:623-624`).
Current coverage of the Stage 1 cases:

| Case | Generated today? | Needed |
|---|---|---|
| Register-indirect memory access in a delay slot | Yes: `PlacePacket(..., false, ...)` for a slot only refuses branch and ECU packets (`:546-547`); but `FetchSuperBlock` then refuses to compile it (R4), so the trial is "not compiled", about 0.5% of trials (arm64-jit-plan.md, 2026-11-15) | The same generator will start compiling once S1.1 is in; add a generator mode that puts register values in the control and tag windows (`MPE_ITAGS_BASE` to `MPE_RESV_BASE`, `src/NuonMemoryMap.h:10-12`) so the bail is taken, and compare the interpreter's result with the native run followed by the dispatcher finishing the slots |
| `,nop` branch in slot 2 | Yes, one time in three for a conditional first branch (`:667-681`, the `Below(3)` pick at `:671`) | None |
| Branch in slot 1 | No: `bAllowBranch` is false for slot packets, except the `,nop` pick for slot 2 | Allow it for slot 1 |
| Delayed branch in a slot | No | The test run ends at the first taken branch (`:752-754`) and stops at `HALT` after four packets; this case needs the run to continue through the second branch's slots, so the generator must lay out at least five packets and a target inside the run |
| Control-register helper (S1.4) | Control-register stores are limited to plain registers (`IsSafeControlRegisterAccess`) | A mode that allows side-effect registers against a fake MPE, compared after the dispatcher has run |

Because the test is the safety net the docs rely on, add the generator
changes before the emitter changes, so that each starts as a failing test.

Go/no-go measurement: the D1 estimate of 3.4, then after each item the B1
benchmark. An item stays in if it raises mean frame rate by at least what its
counter predicted, within the run-to-run spread.

## 5. Stage 2: direct block linking

### 5.1 Design

The design is the usual translation-block chaining, kept lazy, with static
exits first. Everything is under `USE_A64_JIT` so that the x86 emitters and
the Windows and Linux x86 path do not change.

Block layout (new; the shape is selected by `NUANCE_A64_LINK`, 5.5):

    entry:       prologue (PUSHAD), register-file pointers; branch to body
    link_entry:  budget check and charge (below)
    body:        as today
    exit_n:      one per static exit: a patchable direct branch,
                 initially to the slow stub slow_n
    slow_n:      write exit state, store the site id in a field of the MPE,
                 store pcexec, epilogue, ret

Today every block has one tail (`pcexec` store, epilogue, `ret`;
`src/SuperBlock.cpp:448-453`) and the dispatcher resolves the branch. In the
new shape a block with a branch resolves it itself: at the end it loads
`ecuSkipCounter`. If zero, it goes to the fall-through exit (target
`exitAddress`, known at emission). If 1, it stores 0 and goes to the taken exit
(target the branch's `FIELD_ECU_ADDRESS` for `bra` and direct `jsr`, known at
emission; for `jmp reg`, `jsr reg` and `rts`, in `pcfetchnext` at run time). The
`,nop` forms leave from the middle of the block with `Emit_ExitBlock`
(`src/EmitECU.cpp:235-253`, `src/EmitMisc.cpp:14-18`); that function becomes
the place that emits an exit of the new kind. Every slow exit leaves exactly
the state the dispatcher would have produced for the unlinked block, so that
slow exits and the old tail are indistinguishable to path B.

Where the successor is found. The dispatcher, not the block. After a block
returns through `slow_n`, the dispatcher reads the site id, does the lookup it
already does for the chain loop (`src/mpe.cpp:2209-2222`), and if the successor
qualifies (5.2) patches exit `n` of the predecessor to jump to the
successor's `link_entry`, in a `WriteScope`, followed by an instruction-cache
invalidate of that one word (`FlushInstructionCache`, `src/linux_compat.h:277-287`).
The record of the link (site address, original target, successor key, successor
entry) goes in a per-cache list.

Why patched direct branches, not a per-exit data slot with `ldr`/`br`: a
patched branch costs nothing once predicted and costs nothing to follow; a slot
costs two loads and an indirect branch per transition, which is what chaining
partly removes. A slot would avoid the write window and the cache flush at link
time, but links are made once per site per flush, so those costs do not
matter. Indirect exits (stage 2b) do use a data slot: a one-entry inline cache
holding `(key, link_entry)`, compared to `pcfetchnext`, since the target
cannot be known at emission.

Budget. `cycleCounter` is the turn's packet count, reset at the top of the turn
(`src/mpe.cpp:1854`) and read by the round (`src/libretro.cpp:1061`) for
credits. The dispatcher adds `numPackets` before the call (`:2123`) and the
chain loop adds it before each step (`:2225`) and tests `packetsRun < a64ChainPackets`
before the next one. The same semantics in generated code: `link_entry` loads
`cycleCounter`, compares with the limit, and if at or over it returns to the
dispatcher with `pcexec` set to this block's own start; otherwise it adds the
block's own `numPackets` and falls into the body. It costs about five
instructions per transition and keeps the packet accounting and the turn length
exactly as with the C++ chain loop at the same limit. That is the point: the
first version should change cost and not timing (5.2, fairness). The limit can
be an emission-time immediate, since it is read once from the environment at
load (`src/libretro.cpp:929`).

Frame and registers. Within a chain every block runs in the first block's
frame: `x19`-`x22` and `x29`, `x30` saved by the first prologue
(`src/nativecodecache_a64.cpp:833-840`), and `x21`, `x22` already hold this
MPE's register-file pointers, which are the same for every block of the MPE
(`src/SuperBlock.cpp:386-387`). Nothing else survives from block to block
(`x19`, `x20` and the `w9`-`w15` temporaries are scratch,
arm64-jit-plan.md, register mapping), so `link_entry` needs no setup. Stack
depth does not grow: links are branches, not calls.

Optional second change in the same stage: move the emulator variables out of the
64-bit immediates. `pcexec`, `pcfetchnext`, `ecuSkipCounter`, `cycleCounter`,
`interpretNextPacket`, `overlayMask` sit together after the 8 MB of local
memory in the MPE object (`src/mpe.h:364-373`), so one base register
(`x23`, unused today: no use of `x23`-`x28` in `src/nativecodecache_a64.cpp`)
would make each access one instruction instead of four to six
(`src/nativecodecache_a64.cpp:240`). It needs an extra `stp` in the prologue.

### 5.2 Keeping it correct

**Blocks being invalidated.** Blocks are found through `PageMap` entries, and
invalidation clears an entry's `virtualAddress` to 1 (`src/PageMap.cpp:60-77`,
`:79-97`) without touching the code, which stays in the buffer until `Flush`
resets the emit pointer (`src/nativecodecache.cpp:155-165`). A linked branch
bypasses the page map, so a predecessor outside a flushed range would go on
running a successor inside it. Rule: every call of `FlushRegion` or `Flush`
that invalidates at least one valid entry walks the link list and unpatches
(restores the slow branch of) each link whose successor key lies in the range,
in a `WriteScope`; `Flush` just clears the list, since all code is
unreachable afterwards. Walk cost is the number of links, estimated up to
tens of thousands (two exits per block, 3 MB of code after a minute on MPE1,
arm64-jit-plan.md 2026-11-15), so tens of microseconds. The calls are at
`src/mpe.cpp:1951-1957`, `:1977-1979`, `:2031`, `:2064`, `:2496`. Stage 0
reports flushes a second (E2); if it is in the hundreds, group the list by
overlay. Today's `FlushRegion` does not say whether it invalidated anything;
a wrapper under `USE_A64_JIT` can (`PageMap::InvalidateEntry` returns void).
Only flushes that unlink anything matter, because a flush that changes nothing
leaves the links valid.

**Overlays.** IRAM blocks are keyed by `pc | overlayMask` (`src/mpe.cpp:1967`),
where `overlayMask` is the overlay index shifted (`src/OverlayManager.h:73`, `:90`).
Two rules. (1) Link only if the predecessor and successor are in the same
domain: both outside local memory, or both in local memory under the same
index. A link from system-bus code into local memory would depend on the
overlay current at link time, and the dispatcher changes it between turns.
(2) A link inside an overlay is valid while that overlay's blocks are valid;
when the index is reused for new content the dispatcher flushes the region
(`src/mpe.cpp:1946-1952`), which unlinks by the rule above. Entry to a block
of an overlay that is not current is only by lookup under the current mask,
so stale code of another overlay is unreachable. A chain never crosses an
overlay change by itself: the content changes only by DMA, which runs in the
interpreter or in another MPE's turn (arm64-jit-plan.md, "Tempest 3000 changes
overlay about a thousand times a second").

**Self-modifying and reloaded code.** The only things that mark code stale are
`UpdateInvalidateRegion` (called from DMA and BDMA, `src/dma.cpp:829`, `:850`,
`src/bdma_type5.cpp:326`, `src/bdma_type12.cpp:302`, `src/mpe_alloc.cpp:313`,
`src/media.cpp:301`) and `bInvalidateInstructionCaches` (the `icachectl` and tag
writes, `src/ExecuteMEM.cpp:1229`, `:1582`). Both are acted on at the top of a
turn (`src/mpe.cpp:1924-1982`) and checked per step in the chain loop
(`:2193`, `:2215`). A plain store into local memory from code does not mark
anything today, with or without links. Linking changes none of that, provided
a chain never runs blocks across a point where either could have been set.

That proviso holds because native code cannot do either. The control-register
stores the emitters accept are a fixed list that leaves out `mpectl`,
`intctl`, `inten1`, `inten1set`, `inten2sel`, the DMA registers' side
effects and `icachectl` (`src/EmitMEM.cpp:2593-2840`; `intsrc`, `intctl` and the
enables are commented out); everything else is refused by the decoder (R1) or
bails to the interpreter (R11). The only helper calls from generated code are
the bilinear and pixel helpers (40 `X86Emit_CALLI` sites, all in
`src/EmitMEM.cpp`, all to `GetBilinearAddress`, `_LoadPixel*` and
`_StorePixel*`; no other emitter file calls anything). Not verified: the bodies of
those helpers, and what a native indirect access to another MPE's address
window does (`bankPtrTable[2]` is this MPE's own memory for every `0x2...`
address, `src/mpe.cpp:812-814`).

**Interrupts and timers.** Interrupts are taken only at the top of a turn,
and only when `ecuSkipCounter` is zero (`src/mpe.cpp:1872-1906`); the chain loop
repeats the test after each block (`:2195-2204`). Timers, the video field and
audio act at the round level, never inside a turn (`src/libretro.cpp:1116-1166`),
as does `TriggerScheduledInterrupts`. So within one MPE's turn, nothing
outside it can raise an interrupt; inside it, native code can only clear
`intsrc` bits (`intclr`). A chain entered with no interrupt due therefore
cannot make one due. At the start of a turn the test at `:1872` has either
taken the interrupt or found none firable with `ecuSkipCounter` zero, which a
native entry requires (`:1989`). The per-step interrupt test is redundant
for chains of native blocks. Stage 2 can drop it, and so could the C++ loop
(variant 2c). If 5.2's argument fails when the helpers' bodies are read, add
one load and test of `intsrc` to `link_entry`; cost about two instructions.

**Halting and breakpoints.** `halt` and breakpoint packets are never in a block
(`src/DecodeECU.cpp:273`, `src/SuperBlock.cpp:1032`), so a native chain cannot
halt the MPE or raise an exception. Nothing sets `excepsrc` from native code
except clearing it (`excepclr`).

**Mid-block bails.** The bail paths leave through `Emit_ExitBlock` with
`interpretNextPacket` set and `pcexec` at the packet (`src/EmitMEM.cpp:429-433`).
They stay exits to the dispatcher; they are not links. They must also write
the accounting back: today the dispatcher charges a bailed block its full
`numPackets` (`src/mpe.cpp:2123`) although part of it did not run. Linked
entries charge the same way, so accounting is the same.

**Fairness, budget and credits.** A chain ends after the same number of packets
as the C++ loop at the same limit, so each MPE's share of a round, and what the
credit account sees (`ran = cycleCounter`, `src/libretro.cpp:1061`), are the same.
What can differ is the turn count: one turn today ends at any exit that
cannot be linked, and it still does. If someone raises the limit later,
cross-MPE interleaving becomes coarser; E1 measures that before anyone does.

**MPE3.** Links are never made for MPE3 while `bMPE3PacketHack` is non-zero,
the same condition as the chain loop (`src/mpe.cpp:2179`; the hack covers
values 1 and 2). The hack depends on MPE3 yielding after every packet in
system-bus code (`src/SuperBlock.cpp:990-1013`).

**Media-callback timing.** `TickMediaCallbacks` counts turns, not packets
(`src/media.cpp:156-160`; "Ticks (= MPE::FetchDecodeExecute steps...)",
`:32-34`), and the delivery spacing is 1000 ticks (`:44`), with a note that
about 5000 "barely works with T3K". Anything that changes the number of turns
for the same work changes that timing, chaining already did, and so would a
longer limit. A version that keeps turn boundaries equal does not.

**Apple write window.** `pthread_jit_write_protect_np` is per thread and the
buffer is `MAP_JIT` (`src/nativecodecache.cpp:49-52`, `:107-113`). Every patch
and unpatch is made inside a `WriteScope` on the thread that runs the
emulation (`src/SuperBlock.cpp:481` shows the existing pattern), which is the
thread that executes the code, so no cross-core instruction-stream
synchronisation is needed beyond the cache invalidate. On Android the buffer is
plain read-write-execute (`src/nativecodecache.cpp:61`); the invalidate is
`__builtin___clear_cache` (`src/linux_compat.h:283-285`). Not verified: patching on
Android in an app rather than from `adb shell` (the 2026-11-15 section says an
app is still to be tried).

**Threads.** The code cache, the instruction cache, the overlay manager and the
`SuperBlock` are members of each MPE (`src/mpe.h:398-401`) and are touched by the
emulation thread only. The main thread parks it before reading video state
(`EmuPause`, `src/libretro.cpp:1248-1277`, `:1524`), the sampler and the disc
read-ahead thread do not touch them, and the self-test and difftest run on the
main thread inside `retro_load_game` before the emulation thread exists
(`:937-949`, `:983`). One shared piece of compile-time state is not per MPE:
`g_flags` in the Arm64 emitter (`src/nativecodecache_a64.cpp:130`), which is
fine with a single emulation thread and is a problem for section 6 (MPE threads).

**The watchdog and the sampler.** `a64DebugBlockPc` is written by the dispatcher
only (`src/mpe.cpp:2161`, `:2227`), so after Stage 2 the watchdog names the block
that started a chain (`src/libretro.cpp:1497-1500`), and the sampler counts a
whole chain as generated code. Comparisons between builds must therefore use
frame rate, not the split.

### 5.3 What stays in the dispatcher

Everything at the start of a turn: interrupt service, overlay detection and
hashing, the flushes, cache lookup, the interpreter, compile, BIOS and PE
calls, the media tick, breakpoint and halt tests, and the first block's
call. The dispatcher also keeps the exits that cannot link: an exit whose
target has no native block (the dispatcher compiles it when the packet's count
reaches the threshold, and the slow exit is then relinked by the next arrival),
cross-domain exits, MPE3 under the hack, and the mid-block bails.

### 5.4 Testing

1. The differential test with a chain mode. Today's difftest stops at one block
   (`src/a64_difftest.cpp:621-624`). Add `NUANCE_A64_DIFFTEST_CHAIN=1`: programs
   of two to six blocks laid out in memory, each ending in a random branch (always
   or conditional; delayed with slots, or `,nop`) to another block, a
   fall-through, or the final `HALT`; run the interpreter packet by packet and
   the native side with links and a limit larger than the program, compare
   registers, sp, touched memory and the final `pcexec` as now. Cases to
   force: a loop of three blocks, a block that links to itself, a taken and a
   not-taken exit from the same block, a link whose target is invalidated in
   between runs.
2. Turn equivalence. For the same programs run `NUANCE_A64_LINK=0` and `=1`
   with limits 1, 3, 8, 32 and require identical sequences of (`pcexec`,
   `cycleCounter`, register hash) at every turn boundary. This is the test that
   the first version changes cost and not timing, and it needs no game.
3. Invalidation. Run a program in local memory, change its code and call
   `UpdateInvalidateRegion` as the DMA does, run again with links on; compare
   with the interpreter. Repeat across an overlay index reuse (to force
   `bInvalidateOverlayRegion`) and with a `Flush`.
4. Chain-length stress: a ring of 200 blocks of one packet each with the limit at
   1000; require the packet count per turn to equal the limit within one block.
5. The self-test (`src/nativecodecache_a64.cpp:1950`): two blocks, link, run, unlink, run.
6. The bundled disc: "all compiler instruction test iterations passed" with links
   on (arm64-jit-plan.md, Verification).
7. Tempest 3000 scripted run, links on, 10 minutes on both machines, with the
   watchdog on, then the A/B of section 3.1.

### 5.5 A run-time switch

`NUANCE_A64_LINK`, read where the others are (`src/libretro.cpp:929-936`):
0 emits today's code shape (all comparisons start from here); 1 emits the
link-capable shape and patches; 2 emits the link-capable shape and never
patches, which measures what the shape itself costs. `NUANCE_A64_CHAIN` keeps
its meaning as the limit, 0 disabling both chain loop and links. The default
stays 0 until the A/B shows a gain.

### 5.6 Go/no-go

Before: D2 from section 3.4 above 15%. After: a gain in mean frame rate on B1
of at least 8% on both the Mac and the Pad, with the six-run-per-arm design of
3.1, and a clean difftest and chain test over ten seeds as for the other
changes. Otherwise leave it off.

### 5.7 Variant 2c: the successor in the entry, no patching

If Stage 0 puts D2 between 8% and 15%, try this first. Add two fields to
`NativeCodeCacheEntry` (not growing it, by my arithmetic): a pointer to the
page-map entry last seen as the successor of the taken exit and of the
fall-through, filled by the chain loop. Next time, if `succ->virtualAddress`
equals the computed lookup (and the domain rule holds) skip the three-level
walk and the IRAM tests. Validity is the same test the loop does now, because
the page-map entry array is never freed (`src/PageMap.cpp:31-58`) and an
invalidated entry has `virtualAddress` 1. Also drop the redundant per-step
tests of 5.2 (interrupt, `mpectl`, halt, cache-invalidate). Estimated 3-6%: it
removes about half of the loads and branches in 2.3 path A and none of the
call, prologue and epilogue cost. One day.

## 6. Alternatives considered

Each with why it is or is not worth doing before or instead of the above.

- **Making the dispatcher cheaper without linking.** (a) A relaxed read of
  `schedule_intsrc` before the exchange in `TriggerScheduledInterrupts`
  (`src/NuonEnvironment.h:185-192`): per round, saves an atomic read-modify-write
  with full ordering; tiny, safe, do it with Stage 0 and measure
  (0.25 day). (b) The successor cache of 5.7. (c) Hoisting the per-step tests,
  5.2; the same code. (d) Base register for the emulator variables, 5.1. (e)
  Move the unconditional debug stores and counters behind the perf flag
  (3.3). (f) Make `PageMap::InvalidateRegion` skip unallocated pages: it is
  shared with x86, but the change does not alter what is invalidated, only how
  fast; justified if the "FlushRegion span" counter is large. Together these are
  the cheapest items and likely a few percent; Stage 0 says which.

- **Longer turns, or turns that do not end at an interpreted packet.** Frame
  rate tracked rounds a second in the saturated experiments, and a turn ends
  today at the first interpreted packet, BIOS call or non-native target. Letting
  the turn go on through an interpreted packet while the packet budget lasts (the
  chain loop calling `ExecuteNuances`, BIOS handlers and the compile path, up to
  the same limit) removes turns without compiling anything, and E1 says how
  sensitive the game is to the limit. Cost: about 1.5 days; risk: it changes
  interleaving exactly where turns used to end early, so it needs the
  Tempest 3000 and Merlin Racing runs. It is the best alternative to Stage 1 if
  the counters show many turns ending at interpreted packets; it is also
  complementary to Stage 2.

- **Longer blocks.** The cap is 120 packets (`src/SuperBlock.h:9`) and is not
  what ends blocks: every branch does (`src/SuperBlock.cpp:1036-1204` breaks out
  after the branch and its slots), and MPE3 is capped to one packet
  (`:1009-1013`). Blocks that continue past a conditional branch (superblocks
  proper) would need side exits and are a project of their own (code
  duplication, the interpreter's `ecuSkipCounter` semantics). Not before Stage
  2. Raising the cap does nothing.

- **Detecting and skipping idle and wait loops.** The docs record that MPE0's
  extra packets, once compiled, were its wait loop (2026-11-15, 2026-11-16) and
  that "not chaining a block into itself, and sitting out scheduler steps while in
  a loop that changes no register" recovered part of a loss in Merlin Racing
  and was dropped. Giving MPE0 fewer turns lowered the frame rate (16, 15, 11, 8),
  so a good part of its work is real. Whether a lot of its 43% is polling is
  unknown; the polling-turns counter (3.3) measures it. A wait-loop skip is
  only worth building if that share is large, and it is the one idea that
  attacks a whole MPE's cost; the risk is timing, as with every scheduling
  change here.

- **MPEs on separate host threads.** The structure suggests it is possible
  (each MPE has its own code cache, instruction cache, overlay manager and
  `SuperBlock`, `src/mpe.h:398-401`) and the upper bound is attractive:
  MPE0 alone had 43% of the time on the saturated Pad (2.1). What stops it:
  the game's cross-MPE handshake and MPE3's sensitivity to gaining on the others
  (`src/SuperBlock.cpp:990-1004`, arm64-jit-plan.md) mean MPE speeds must stay
  related, so each thread would need pacing against the others, and host
  scheduling would make the relation different every run; shared state is
  global and unlocked (video, DMA, comm bus, media queues and ticks, the
  emitter's `g_flags` at `src/nativecodecache_a64.cpp:130`, the perf and
  debug globals); the media tick counts a thread's own turns. It also gives
  nothing on a host short of cores, and the Pad's slow cores were the
  stress case. Weeks and a large risk; not before Stages 0 to 2.

- **Bigger swings in native code quality** (milestone 6 of arm64-jit-plan.md). The
  generated-code share is 44-56%, so it is large, and the 25-30 fixed
  instructions per block in 2.3 and the four-to-six instruction immediates for
  every `pcexec` access (`src/nativecodecache_a64.cpp:240`) are cheap wins
  inside it; 5.1's base register is the first. Hand-written handlers belong to
  the open-ended milestone and are not part of this plan.

- **A faster page map.** A direct-mapped cache of `(key, entry)` in front of the
  three-level walk. Subsumed by 5.7 for the chain loop; for path B alone it
  saves the same two loads. Do it only if Stage 0 shows `FindEntry` as the
  largest part of state 0.

## 7. Risks and unknowns

- **The assumption of saturation (A1).** If the game's frame time is set by a
  dependency chain rather than by thread busy time, shaving per-block time may
  move the frame rate less than it moves the busy time. The 2026-11-16
  experiments that did not help were all about scheduling, so they say nothing
  about this. The sleep share in the perf log is the first check.
- **Timing between MPEs.** Anything that changes turn boundaries (a longer
  limit, turns through interpreted packets, wait-loop skipping) is the kind of
  change that hung the game with extra MPE3 turns (arm64-jit-plan.md,
  2026-11-15). Stage 2a is designed not to change them; the turn-equivalence
  test (5.4) is the check.
- **Delay-slot semantics** for S1.3 are the subtlest in the plan.
- **Flush frequency** is unknown: no figure for flushes a second is recorded in
  the documents. If large, Stage 2's invalidation cost changes and the byte-wise
  `FlushRegion` itself may be a cost today.
- **Behaviour in Merlin Racing** and other games is untested for every change
  here; both games were used for the earlier work. Run both.
- **x86.** Everything above is under `USE_A64_JIT` or in code whose behaviour on
  x86 is unchanged (a counter, an environment variable, a wrapper). Nobody can
  run an x86 build here; the check is that it still compiles, as for earlier
  changes. `PageMap.h` and `SuperBlock.cpp` are shared files.

### Not verified

- That `breakpointAddress` is zero in the libretro build (set only in the
  shells; the object has static storage).
- That `sizeof(NativeCodeCacheEntry)` is 32 and has room for a field
  (arithmetic only).
- The cost figures: 15 ns a turn, 130 ns a round, 25-30 fixed instructions a
  block, 1-2 ns for a linked transition, 1-3 ns per `InvalidateRegion`
  iteration. They come from instruction counts and from combining numbers of
  different sessions in the docs; none was measured.
- That MPE3's packets a second equal the round rate in busy scenes (it assumes
  MPE3 stays in system-bus code and does not wait for a video field).
- That native code cannot change interrupt, halt or invalidation state: I read
  the control-register store list and the 40 helper call sites, not the helpers'
  bodies, and not the loads in `src/EmitMEM.cpp` that go through
  `bankPtrTable` (which maps every `0x2...` address to this MPE's own memory).
- The memory-first ordering claim for the mid-block bail; the scheduling code
  was skimmed, not read.
- Whether the interpreter and the native emitters agree on a branch in a delay
  slot of a not-taken conditional branch: the docs call the rules subtle and the
  difftest does not generate it.
- That `FetchDecodeExecute` has a heavy prologue; no disassembly was read.
- The sampler's real period (`usleep(100)` is a minimum) and so its sample count.
- Patching generated code in an Android app, as opposed to a shell process.
- Frequency, in Tempest 3000, of each Stage 1 case beyond the order the docs
  give.
- The effort figures; they are my estimates and include tests.
- The rough size of a full save state (the number of variables in `src/video.cpp`
  is from a pattern count, not a read).

## 8. Order of work and estimates

| Step | What | Days | Where |
|---|---|---|---|
| 0.1 | E2 and E3 from a real play session with the existing log; E1 on the benchmark | 0.5 | Mac; Jake plays |
| 0.2 | Counters and the finer split (3.3); `COMPILE_THRESHOLD` variable | 1-1.5 | Mac |
| 0.3 | E4 micro-benchmark; benchmark runs and the A/B harness (alternating runs, summary) | 0.5-1 | Mac and Pad |
| 0.4 | Real-play session with the new counters; Pad runs of B1; the decision of 3.4 | 0.5 | Mac, Pad, Jake |
| 0b | Items the counters justify (6) | 0.25-1 each | Mac, checked on Pad |
| 1 (S1.1, S1.2) | Delay-slot cases, with difftest generator changes first | 3-5 | Mac; Pad for the A/B |
| 1 (S1.4) | Control-register packets as end-of-block helper calls | 2 | Mac |
| 1 (S1.3) | Delayed branch in a delay slot | 3-5 | Mac; do only if the counters demand it |
| 2c | Successor cache in the entry | 1 | Mac; Pad for the A/B |
| 2a | Direct links: stubs, patching, unlink, switch, chain mode of the difftest, turn-equivalence test | 6-9 | Mac for all but the A/B |
| 2b | Indirect exits through an inline cache | 2-3 | Mac |
| A/B | Six-per-arm alternating runs, each change, both machines | 0.5 per change | Pad needed |

Everything except the final measurements and the Android patching check can be
built and tested on the Mac alone: the difftest, the self-test and the
bundled disc all run there (`tools/a64_difftest_host.c`), and the Pad is
reached with `adb` over Tailscale when away from home. The Pad is needed for
the saturated benchmark (`taskset 0f`), for the cross-check that patching
works from an app, and for deciding go or no-go, since its numbers decide
where the Mac's do not (46% against 36% in the dispatcher share).
