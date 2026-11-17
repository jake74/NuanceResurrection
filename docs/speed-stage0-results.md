# Speed plan, Stage 0: the measurements (Mac)

Status: done on the Mac mini M4 Pro, 2026-11-17, on branch `macos-arm64`. Nothing is committed; the changes are in the working tree. This is Stage 0 of [speed-plan.md](speed-plan.md), sections 3.1 to 3.4, Mac only. It was written by an AI coding assistant, which ran every measurement reported here; where a figure is derived from others the text says so and shows the working. Nothing in speed-plan.md or arm64-jit-plan.md was edited.

## 1. Summary

**What was built.** Counters for why every turn of every MPE ends, for packets per turn, for the interpreted packets and the reason each was interpreted, for cache flushes and the addresses their loops walk, and for rounds; and a finer split of the sampler's "dispatcher and the rest" into nine new states (section 2). With `NUANCE_PERF` off, the cost measured at saturation is 1.1% of the frame rate (not zero; section 5.2); at the shipped speed limit it cannot be seen. With it on, the game runs 2.6% slower than with the old `NUANCE_PERF` at saturation, and the counting itself is 4.6% of the thread's time.

**Checks.** The differential test passes on three seeds at 200,000 trials (0 mismatches in 180,563, 180,611 and 180,682 compared trials). The game runs at the same rate with and without `NUANCE_PERF` at the shipped limit (36.1 and 36.0 frames a second averaged over 80 seconds including the boot; the game is paced by the clock there).

**Where the time goes** (saturated host, no MPE speed limit, 7 runs, game frames 1770 to 2320, the heavier stretch; the figures for the first stretch of level one are within two percentage points of these, section 7):

- Generated code is 55% of the thread's time, the interpreter 8%, the dispatcher proper 18%, the chain loop between two blocks of one turn 5.5%, the end of the turn and the loop between turns (media tick included) 6.6%, the round level 1.6%, everything else (compiling, flush loops, overlay hashing, decoding, handlers) under 0.5%, and NUANCE_PERF's own counting 4.6%. "Dispatcher and the rest" of the earlier measurements was 36.7% in this setting (32% without the counting); the genuine block-to-block transition inside a turn is 5.5% of it.
- A turn costs about 10 ns of dispatcher time (derived: 17.8% + 6.6% of a second over 24.9 million running turns a second) and an extra turn about 9 to 16 ns (derived from the chain sweep), against 2.3 to 3.2 ns for a chain step (derived). There are about 25 million running turns a second but only 24 million chain steps against 37 million blocks. A round costs 138 ns of busy time and runs about 25 packets of MPE0 to MPE2 (derived). About half or more of the turns of MPE0 to MPE2 are one interpreted packet (47 to 59% in this stretch).
- The interpreted packets in the heavier stretch are almost all one loop, run on all three MPEs: a conditional branch packet that is never compiled, the `bra ..., nop` in its first delay slot, and the packet in its second delay slot. By reason, 40% are delay slots of an interpreted branch, 39% the never-compiled branch packets, 13% control-register accesses and 7% MPE0's wait loop (marked compiled but no block). In the first stretch of level one the wait loop is a third of them.
- Chain sweep, saturated: the frame rate is 0.71, 0.89, 1 and 1.05 times that at the default of 32 for a budget of 0, 8, 32 and 128 packets in the heavier stretch (20.9, 26.0, 29.4 and 30.8 frames a second); in the first stretch 0.74, 0.84, 1 and 1.02 (40.6, 45.8, 54.8, 56.0). 128 against 32 is a gain in the heavier stretch (30.8 ± 0.1 against 29.4 ± 0.3) and not distinguishable in the first (56.0 ± 0.7 against 54.8 ± 0.4).

**The decision rule** (section 9). Direct linking (Stage 2a): D2, the share direct linking can return, is 2.7 to 5.3% of busy time with the E4 block cost and a linked transition of 1 to 2 ns, under the plan's 8%, so no-go for 2a (2c or stop); even the whole chain loop plus the E4 lower bound for block entry and exit is 8.4 to 8.8%. Compiling the interpreted packets (Stage 1): D1 is 11.6 to 16.0% of busy time, over 8%, so go; limited to the packets Stage 1 can address it is 6.6% in the first stretch and 12.6% in the heavier one. The first item of Stage 1 that the data points to is the branch in a delay slot, S1.2. Confidence is moderate: the shares and counts are tight (run-to-run spread under a percentage point), but the in-place cost of a block's entry and exit was not measured (only a hot-loop lower bound), the factor "one third" for compiled against interpreted time is from earlier documents, and the Mac is not host-bound at the shipped limit.

**Not done:** see section 10.

## 2. What was added

All of it is under `USE_A64_JIT`, except where the file is not an Arm64 file at all (the host tool). Nothing in the x86 emitters changed. After removing every `USE_A64_JIT`-only line from `src/mpe.cpp` the text is the same as at the start of the stage apart from two blank lines, so the x86 build sees the same code (checked with a small script, see section 3.4; no x86 compile was possible on this Mac, as for earlier stages).

### 2.1 Counters

All counters are per MPE arrays of `unsigned long long`, declared in `src/a64_emit.h:57-136` and defined in `src/nativecodecache_a64.cpp:1804-1807`. They are incremented only when `a64PerfSampleInterpreted` is set, which `NUANCE_PERF` does (`src/libretro.cpp`, in `retro_run`, as before). None is per packet and none is per chain step.

The design that came out of the cost measurements (section 5.2) is this. The paths of a turn leave *why the turn ended* in a local variable, `a64EndReason`, by plain assignment (a register move, no test). One test of the flag at the end of each running turn (`src/mpe.cpp:2481`) then adds everything. What cannot be had from that one test is derived in the report.

| Counter | Where it is counted | What it costs with `NUANCE_PERF` off |
|---|---|---|
| Turns, by why they ended (15 reasons, section 2.2) | one test at the end of a running turn, `src/mpe.cpp:2481-2509`; the not-running early exit, `src/mpe.cpp:2537`. The reason is left in `a64EndReason` at `:2219` (IL block), `:2254` (native turn, budget unless changed), `:2281-2349` (the chain loop's `break`s and the chain-off case), `:2368` (interpreted), `:2404` (BIOS), `:2423` (PE) | one load and compare of the flag per running turn; a register move at the end of each turn; one local `a64TurnStartPc` (`:1878`) |
| Packets run, turns by packets run (0, 1, 2-3, 4-7, 8-15, 16-31, 32 or more), polling turns (the turn began at the same `pcexec` as that MPE's previous running turn) | same block, `:2485-2490` | nothing beyond the above |
| Chain steps | not counted: derived as the blocks entered (`a64PerfNativeBlocks`, which has always been counted unconditionally, `:2253` and `:2343`) less the native turns. The report no longer zeroes `a64PerfNativeBlocks`, because that racy reset by another thread double-counted now and then; it takes differences | nothing added |
| Native turns | derived: turns that ended in one of the ten native ways | nothing added |
| Turns not given by the round | derived: rounds less turns | nothing added |
| Packets per turn, blocks per native turn, packets per block | derived in the report | nothing added |
| For "no block for the next address": what the next packet is (BIOS or PE address, never decoded, never compilable, marked compiled but no block, below the threshold, block present but not native) | the end block, `:2491-2505`, from `a64EndLookup` (`:2333`) and a look in the page map and instruction cache; nothing is changed by looking | a register move on the no-block exit |
| Interpreted packets by reason (9 reasons: delay slot of an interpreted branch, mid-block bail, newly decoded, marked compiled but no block, below threshold, and four "never compiled" kinds inferred from the packet flags: BIOS jump table or breakpoint, branch packet, control-register access, other) | the interpreter path, `:2368-2382`, one test of the flag per interpreted packet (that path costs far more than the test) | one test of the flag per interpreted packet |
| Native cache flush events (overlay region, main and system bus ranges, whole cache past threshold, whole cache for lack of room), `FlushRegion` calls and addresses walked | at the call sites, `:1972-1992` (overlay and leftover ranges), `:2019-2029` (instruction-cache invalidation), `:2089`, `:2135` (whole cache) | one test of the flag on each of those rare paths |
| Rounds, rounds in which no packet ran, rounds slept as idle | `src/libretro.cpp:1079-1086`, one test per round | one test per round (a round is three or four turns) |

### 2.2 The turn-end reasons

`enum A64TurnEnd` (`src/a64_emit.h:67-84`). Every call of `FetchDecodeExecute` adds to exactly one, so they sum to the number of turns; the report checks this against a second, independent count of turns (the histogram of turns by packets run) and prints both (`the reasons sum to X of Y turns`). In 2,355 of the 2,360 report lines of the runs used here the two agree exactly; in the other five they differ by one turn a second (the counters are read while the emulation thread is running).

Not running; one interpreted packet; a BIOS handler; a Presentation Engine handler; an IL block (never seen on Arm64); and for turns that ran a native block, how the chain loop ended: budget (`a64ChainPackets` packets reached, which includes the case that the first block alone reached it); chain off (limit 0, or MPE3 under the packet hack); branch pending (`ecuSkipCounter` above 1); halted or exception; instruction-cache invalidation pending; mid-block bail (`interpretNextPacket`); interrupt would be taken; breakpoint; the next address is in a local-memory region marked for invalidation; no native block for the next address. These cover every `break` of the chain loop and its two ways of not starting (`src/mpe.cpp:2275-2350`).

### 2.3 The finer time split

The existing sampler thread (`src/libretro.cpp`, `EmuThreadMain`) now reads four bits of `a64DebugBlockRunning` instead of two (`perfState[16]`). The states are `enum A64State` (`src/a64_emit.h:121-136`):

| State | What it holds | Set at |
|---|---|---|
| 0 dispatcher proper | `FetchDecodeExecute` outside everything below: the start of a turn, the interrupt test, the page-map and instruction-cache lookups, the tail | the top of the function, `src/mpe.cpp:1862` (an unconditional store of 0; before this stage the value 0 was stored after each block instead) and back to 0 after rare paths |
| 1 generated code | from the call instruction to the return: prologue and epilogue of the block are in it | as before |
| 2 interpreter | `ExecuteNuances` for one packet | as before |
| 3 asleep | the 50 microsecond sleep | as before |
| 4 BIOS and PE handlers | C++ handler calls | `src/mpe.cpp:2405`, `:2424` |
| 5 compiling | `CompileBlock` | `src/mpe.cpp:2101` |
| 6 chain loop between blocks | the loop that runs the next block of a turn, from the return of one block to the call of the next, including the last iteration of every chain that ends by a break | `src/mpe.cpp:2260`, `:2345` (these replace the old store of 0 after a block) and `:2350` (back to 0) |
| 7 round level | `RunEmulation` after the four turns: comm bus, clock, timers, audio, scheduled interrupts | `src/libretro.cpp:1084` |
| 8 cache flush loops | the native and interpreter cache invalidation loops, `InvalidateICache`, `ClearCompiledStates` | rare paths in `src/mpe.cpp:1950-2140` |
| 9 overlay hashing | `OverlayManager::FindOverlay` | `src/mpe.cpp:1952` |
| 10 packet decode | `DecompressPacket` for a packet seen for the first time | `src/mpe.cpp:2172` |
| 11 media tick and loop between turns | `TickMediaCallbacks`, the end of `FetchDecodeExecute` and the credit arithmetic in `RunEmulation` between two turns, up to the top of the next turn | `src/mpe.cpp:2509` |
| 12 NUANCE_PERF counting | the end-of-turn counting itself, so that what the measurement costs shows as its own share | `src/mpe.cpp:2483` |

What I could not separate: the media tick from the loop between turns (state 11; the tick is one call, the loop is a handful of instructions, and a separate state would need a second test per turn); the block's own prologue and epilogue and the indirect call from the block's body (all state 1; the E4 micro-benchmark, section 8.3, gives only a hot-loop lower bound); the first and later parts of `FetchDecodeExecute`'s own entry and exit (all state 0). The interrupt-service part of the top of a turn is in state 0 as well. The sampler reads a variable, not the program counter, and wakes every 100 microseconds at best, so each share has an error of a few tenths of a percentage point over a 15-second window (the run-to-run spread in the tables below is the better guide).

### 2.4 Other changes

- `src/libretro.cpp`: the report reads the counters as differences between two reads, and `a64PerfNativeBlocks` is no longer reset (see the table above); the line `perf: window ...` gives the window's length and the game frame numbers at its start and end, which is how windows are chosen by game frame below; the watchdog (`NUANCE_A64_WATCHDOG`) now says "inside" only for state 1 (generated code), because the dispatcher's chain loop now also stores a non-zero state when `NUANCE_PERF` is off.
- `src/nativecodecache_a64.cpp`: `NUANCE_A64_E4=<calls>` times calls of a block of the compiler's shape from a loop in C++ (the plan's E4); it logs one line to the standard error and changes nothing else.
- `tools/perf_host.c`: prints the game's own frame count (the core's `perfVidSyncTotal`) after the frame count, so that a run without `NUANCE_PERF` has a frame rate to the frame.
- `tools/stage0/`: the run script and the three analysis scripts used for every table below.

The report is still written once every 300 calls of `retro_run`, as before, which is five seconds in `perf_host` (60 calls a second). The plan asks for once a second; the existing interval was kept, so the reports are rates over five-second windows.

## 3. How to build, test and reproduce

### 3.1 Build

**The counters are a build option, off by default** (added in review, see section 5.2): an ordinary build has none of them, and all the measurements of this document need a core built with `-DNUANCE_PERF_COUNTERS=ON`. Without it `NUANCE_PERF` still gives the old time split and packet counts, but not the turn counts, turn-end reasons or finer states. A separate build directory keeps the two apart:

```
cmake -S /Volumes/External/dev/NuanceResurrection -B <dir> -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRETRO=ON -DNUANCE_PERF_COUNTERS=ON
make -C <dir> -j10 nuance_libretro            # the measuring core
```

The ordinary build directory is `build-libretro` (CMake, Release, `NUANCE_ARM64_JIT=ON`, as arm64-jit-plan.md describes). The headless host is not a CMake target; it is one C file.

```
cd /Volumes/External/dev/NuanceResurrection/build-libretro
make -j10                                    # nuance_libretro.dylib and a64_difftest_host
cc -O2 -o perf_host ../tools/perf_host.c     # the headless host
```

### 3.2 Differential test

The command of arm64-jit-plan.md, 200,000 trials, three seeds (run side by side, which does not matter for it):

```
cd build-libretro
for seed in 1 2 3; do
  NUANCE_ARM64_JIT=1 NUANCE_A64_DIFFTEST=200000,$seed ./a64_difftest_host ./nuance_libretro.dylib ../NUON.CD
done
```

### 3.3 One run

```
tools/stage0/run.sh <core.dylib> <tag> <seconds> [VAR=value ...]
```

with `STAGE0_OUT` (where logs go, default `./stage0`) and `T3K_ISO` (default: the image named in the task) in the environment. It changes to `build-libretro` and runs

```
NUANCE_T3K_STATE=1 NUANCE_PERF=1 NUANCE_LOG_FILE=<out>/logs/<tag>.log \
NUANCE_INPUT_SCRIPT='60-63:start;120-123:start;250-253:a;330-333:a;450-453:a;550-553:a' \
  ./perf_host <core.dylib> . <empty save directory> <seconds> "<Tempest 3000.iso>"
```

plus the extra variables given. The three settings used:

- shipped limit (the baseline): nothing extra, 95 seconds;
- saturated: `NUANCE_MPE_HZ=0`, 80 seconds;
- chain sweep: `NUANCE_MPE_HZ=0 NUANCE_A64_CHAIN=<0|8|32|128>`, 80 seconds.

`NOPERF=1` among the variables leaves `NUANCE_PERF` out. The perf host ends with a segmentation fault in `retro_unload_game` after it has printed its results; it did the same with the core as it was before this stage (checked), so it is not caused by anything here.

### 3.4 Tables

```
export STAGE0_OUT=<the directory used above>
tools/stage0/report.py base 770 1550        # per-run and mean tables for the runs <prefix>_<n>, windows inside the game frames 770-1550
tools/stage0/report.py sat 1770 2320
tools/stage0/report.py sweep                # the chain sweep
tools/stage0/report.py overhead             # the cost of the counters
tools/stage0/report.py split                # the time split of the sampler, four settings side by side
tools/stage0/report.py compare              # shipped-limit against saturated profile
tools/stage0/report.py marginal             # cost of an extra turn from the sweep
tools/stage0/arith.py sat 1770 2320         # the arithmetic of section 9, with its working
```

The check that the x86 text of `src/mpe.cpp` is unchanged was made by deleting all `USE_A64_JIT`-only lines (conditional blocks and their `#else` branches handled) from `git show HEAD:src/mpe.cpp` and from the working file and comparing; the only differences were two blank lines.

## 4. Method

**The route into level one.** The script given in the task, `250-253:a;330-333:a;450-453:a;550-553:a`, left Tempest 3000 on its title screen on this machine (in two runs, of 25 and 90 seconds: frames advanced at 18.8 a second, the title screen's rate, and the game's state words stayed at their title-screen values). Adding two presses of START in the first 130 game frames, `60-63:start;120-123:start`, before the game has begun, puts the game in level one at about game frame 700 every time (the state words change to `a=0 b=0 lane=70000` between frames 660 and 720). This is the only change to the script; START is never pressed again after game frame 123, so the game is not paused. In every run used here the game was running at the rates of the tables (never at zero), and the video frame count kept advancing to the end of the run. The reason the script alone did not work is not known; the save directory was empty each time, which may be what differs from earlier runs.

**Windows of game frames.** The report is written every 300 `retro_run` calls, which is a five-second window, and says which game frames it covers. A window is used if it lies wholly inside a frame range; the runs' windows are pooled by their length. Two ranges are used, both after level one has begun:

- WA, game frames 770 to 1550: the start of level one, about 49 frames a second at the shipped limit;
- WB, game frames 1770 to 2320: the stretch that follows, about 26 frames a second at the shipped limit. This is the heavier scene (more packets per frame), and the closest thing to the busy scenes the plan is about. I do not know what the game shows there.

The frame numbers at which these stretches begin and end are the same from run to run (to within a few frames), because the game is paced by the clock at the shipped limit, and nearly so when saturated.

**Runs and spread.** Five runs for the baseline and the saturated benchmark, four for each setting of the chain sweep, four of each configuration for the cost of the counters. A run is excluded if the perf host's loop fell behind (fewer than 99% of the 60 calls a second it should have made): the Mac stalled a process for tens of seconds now and then while other things were running on it (about one run in ten of the first attempts); excluded runs are named in section 5.3 and replaced by further runs. "Spread" is the sample standard deviation across runs, with minimum and maximum. Two settings are called indistinguishable when their means differ by less than about two standard deviations of the larger spread.

**Cost of the measurement.** The sampler and the counters change what they measure (section 5.2). The time-split tables therefore give the counting's own share as a state, and the arithmetic of section 9 takes it out of the busy time.

## 5. Checks

### 5.1 Build and differential test

Build: the commands of section 3.1; the core and the difftest host build without errors (the warnings are those of the existing code). The differential test, on the final build, with the commands of section 3.2:

| Seed | Trials compared (of 200,000) | With memory access | With a branch (taken) | Mismatched | Not compiled natively | Outside the test's memory or unfinished |
|---|---|---|---|---|---|---|
| 1 | 180,563 | 68,738 | 84,315 (48,033) | 0 | 963 | 18,474 |
| 2 | 180,611 | 68,792 | 84,302 (48,247) | 0 | 989 | 18,400 |
| 3 | 180,682 | 69,448 | 84,380 (48,224) | 0 | 998 | 18,320 |

Seed 1 gives the same 180,563 compared trials as arm64-jit-plan.md records. The self-test passes. The difftest runs with chaining off and with `NUANCE_PERF` off (it does not set the flag), so it does not exercise the new counting code in the chain loop; what exercises that is the equality of two independent turn counts in the report windows (section 2.2) and the game itself running at the same rate (below). The same test also passed on an earlier build of this stage (the first design, section 5.3).

### 5.2 What the counters cost

**Review, 2026-11-17: the counters are now compiled out of ordinary builds.** The table below was the reason. Two of the per-turn stores were first put behind the flag; that build still came out 1.1% behind the pristine core (3435 ± 22 against 3474 ± 23 game frames in 80 s, six runs each, alternating). The counting was then made a build option (`NUANCE_PERF_COUNTERS`, CMake option of the same name, default off; `A64_COUNTING` in `src/a64_emit.h` is a constant false without it, so the compiler removes every counter, test and sampler mark of this stage). The default build is within 1 KB of the pristine core's size (the E4 timing loop, which runs only when `NUANCE_A64_E4` is set). Measured against the pristine core, alternating, six runs each, `NUANCE_PERF` off, no limit: 3424 ± 1 against 3435 ± 24 game frames, not distinguishable. Both sets of runs show the same two clusters, about 60 frames apart (3421-3428 and 3483-3488): which one a run lands in varies from run to run for both cores, so a comparison of a few runs can show a difference of about 1% that is not there. The first comparison of this table may have been partly that; it no longer matters for ordinary builds.

The headless host also crashed on the way out of every run, after its numbers were printed (`VideoCleanup` in `retro_unload_game` called `glIsList` with no graphics context), leaving a crash report each time. It no longer unloads the game before exiting (`tools/perf_host.c`); the 85 crash reports from these runs were removed.

What follows is the table as first measured, with the counters built in.

Frame rate with and without them, saturated (`NUANCE_MPE_HZ=0`, so that the thread is the bottleneck), counted as the game's own frame count after 80 seconds of wall time (`perf_host` prints it), four runs of each configuration, the four configurations in rotating order. "Pristine" is the core built from the commit this stage started from, kept as a copy.

| configuration | runs | game frames in 80 s, per run | mean ± sd | frames a second | against the pristine core, same setting |
|---|---|---|---|---|---|
| pristine core (before this stage), NUANCE_PERF off, no limit | 4 | 3498, 3437, 3466, 3444 | 3461 ± 27 | 43.3 |  |
| this stage, NUANCE_PERF off, no limit | 4 | 3436, 3418, 3408, 3425 | 3422 ± 12 | 42.8 | -1.1% |
| pristine core, NUANCE_PERF on, no limit | 4 | 3384, 3402, 3415, 3411 | 3403 ± 14 | 42.5 |  |
| this stage, NUANCE_PERF on, no limit | 4 | 3312, 3317, 3321, 3306 | 3314 ± 6 | 41.4 | -2.6% |
| this stage, NUANCE_PERF off, shipped limit | 4 | 2889, 2890, 2891, 2895 | 2891 ± 3 | 36.1 |  |
| this stage, NUANCE_PERF on, shipped limit | 4 | 2874, 2873, 2880, 2884 | 2878 ± 5 | 36.0 | -0.5% against NUANCE_PERF off |

Reading it: with `NUANCE_PERF` off the core of this stage is 1.1% slower than the pristine core at saturation (3422 ± 12 against 3461 ± 27 frames; the difference is about two and a half standard errors of the difference, so I call it real, though small). That is the cost of the plain stores and moves added on the per-turn paths and of the one test per running turn, and it is not zero. At the shipped speed limit (the last two rows) the game is paced by the clock; with `NUANCE_PERF` on it is 0.5% behind with it off (2878 ± 5 against 2891 ± 3 frames in 80 seconds, four runs each, boot included), which is what a game that is mostly not host-bound shows (the thread sleeps about 11% of the time there). With `NUANCE_PERF` on, this stage is 3.2% slower than with it off at saturation and 2.6% slower than the old `NUANCE_PERF` (pristine on): that is the price of the new counting, which the sampler shows as 4.6% of the thread's time.

The same on the first design of this stage, which counted at every chain step and at every `break` of the chain loop and read a local flag several times a turn: with `NUANCE_PERF` off it was 2.5% slower than pristine (3378 ± 8 against 3464 ± 38), and with it on 3.7% slower than the pristine core with it on. That was too much for "zero when off", so the counters were redesigned as in section 2.1 and every measurement below was made again on the final build. The first-design logs are not used.

The numbers of the table are per path, for the reviewer: with `NUANCE_PERF` off a running native turn executes one load and compare of `a64PerfSampleInterpreted` (the end of the turn) and five plain stores or moves that were not there before (the sampler state 0 at the top, the initial `a64EndReason`, `a64TurnStartPc`, the reason at the `break`, and a store of 0 after the chain loop); the stores of the sampler state in the chain loop replace the old stores of 0 after each block, so a chain step costs what it did. The interpreter path adds one test per interpreted packet; the BIOS and PE paths one per handler call.

### 5.3 Runs that were left out, and how the first attempts went

- The scripted route needed two presses of START before the game begins (section 4); without them the game stayed on its title screen.
- While measuring, the Mac stalled the `perf_host` process for tens of seconds in a few runs (the host's loop made 2855 to 3794 of its 4801 calls in 80 seconds, and one run logged an emulation stall of 83 seconds). Such runs are detected (fewer than 99% of the expected calls) and left out by the analysis script. In the final set no run was affected; the earlier sets were thrown away anyway when the build changed, as described next.
- The first complete set of measurements was made on a first design of the counters (section 5.2), then on a second whose time split put the start of every turn after the first into the "media tick" state (the sampler state was no longer reset at the top of the turn). Both were discarded; all tables here are from the final build, run in one session after the last source change, and the build used for them is the one that passed the differential test above.

## 6. Baseline: level one by script, at the shipped limit (C1)

Five runs of 95 seconds, `NUANCE_PERF=1`, the MPE speed limit left at its default of 54 million packets a second, no frame cap (the command of section 3.3 with no extra variables). Two stretches of game frames, WA (770 to 1550) and WB (1770 to 2320), each pooled over the five-second report windows that lie wholly inside it (three windows in WA and four in WB for each run). In every window the game was running (frames advanced at 25 to 50 a second, audio delivered within 0.2% of the rate needed: 31,948 to 32,005 frames a second against 32,000, START never pressed after game frame 123).

At this setting the game is paced by the clock, not by the host: the MPEs have a 54 million a second allowance, MPE0 runs 53.3 and 53.8 million packets a second in the two stretches, and the thread sleeps 11% of the time. The frame rates are therefore the same from run to run to a few tenths of a percent (the game's frame count at the end of the five runs is 3358 or 3359).

| | WA, game frames 770-1550 | WB, game frames 1770-2320 |
|---|---|---|
| game frames a second, mean ± sd (min-max) | 49.8 ± 0.1 (49.7-49.9) | 25.5 ± 0.0 (25.4-25.5) |
| rounds a second (millions) | 8.94 ± 0.05 | 6.91 ± 0.02 |
| rounds per game frame (thousands, derived) | 180 | 272 |
| thread asleep | 11.1 ± 0.4% | 11.0 ± 0.5% |
| packets per round, MPE0 to MPE2 (derived) | 16.8 | 21.8 |

The per-MPE figures (mean over five runs; spread in the appendix tables, all under 2% of the mean):

| | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| **WA** packets a second (millions) | 53.3 | 50.7 | 46.0 | 8.6 |
| running turns a second (millions) | 8.36 | 3.83 | 3.09 | 6.91 |
| native turns | 3.54 | 1.73 | 1.53 | 6.09 |
| chain steps | 4.44 | 5.57 | 4.59 | 0 |
| packets per running turn | 6.37 | 13.22 | 14.87 | 1.24 |
| blocks per native turn | 2.26 | 4.22 | 3.99 | 1.00 |
| interpreted packets | 4.83 | 2.10 | 1.56 | 0.63 |
| **WB** packets a second (millions) | 53.8 | 50.1 | 46.8 | 7.6 |
| running turns a second (millions) | 6.20 | 5.07 | 4.83 | 5.91 |
| native turns | 2.47 | 2.04 | 1.95 | 5.34 |
| chain steps | 7.43 | 7.10 | 6.68 | 0 |
| packets per running turn | 8.67 | 9.88 | 9.68 | 1.29 |
| blocks per native turn | 4.01 | 4.49 | 4.42 | 1.00 |
| interpreted packets | 3.73 | 3.03 | 2.88 | 0.32 |

MPE3 takes one turn a round, as the plan expected (its running turns equal the rounds: 6.91 million a second in WA, rounds 8.94 million; the difference is the rounds in which MPE3 is held back while an MPE is short of credit), and runs 1.2 to 1.3 packets a turn.

Why turns end, as a percentage of all turns of that MPE (WA left, WB right; the mean ± sd over five runs, always under 0.3 points):

| turn ended by | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| interpreted packet | 57.7 / 60.2 | 28.0 / 47.6 | 19.1 / 42.6 | 9.1 / 5.3 |
| BIOS or PE call | 0 / 0 | 0 / 0 | 0 / 0 | 2.7 / 4.4 |
| budget (32 packets) | 10.6 / 14.3 | 13.1 / 13.5 | 11.2 / 11.8 | 0 / 0 |
| no native block for the next packet | 31.6 / 25.5 | 10.0 / 18.5 | 7.6 / 17.1 | 0 / 0 |
| mid-block bail | 0.1 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| interrupt, breakpoint, halted, branch pending, icache invalidate, local memory dirty, IL | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| chain off (MPE3 under the packet hack) | 0 / 0 | 0 / 0 | 0 / 0 | 88.2 / 90.3 |
| MPE not running (halted between jobs) | 0 / 0 | 48.9 / 20.5 | 62.1 / 28.5 | 0 / 0 |

Every other exit reason of the chain loop is zero or below 0.2% of turns in these scenes: the chain never ends for an interrupt, a pending branch, a breakpoint, an invalidation or a dirty local-memory region. It ends for one of three reasons: the next packet is interpreted, the budget is reached, or there is no block for the next address; and for MPE0 more than half of all its turns are a single interpreted packet.

For the turns that ended for lack of a block, the next packet was: never compilable (0.6 to 0.7 million a second for each of MPE0 to MPE2 in WA and 1.2 million in WB); marked compiled but with no block under the current overlay (MPE0 only, 2.0 million a second in WA and 0.4 in WB: the wait loop); decoded for the first time or below the threshold (under 0.02 million). Never a BIOS or PE address.

Interpreted packets by reason (millions a second, mean over five runs; the full tables are in the appendix):

| | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| **WA** delay slot of an interpreted branch | 0.83 | 1.06 | 0.76 | 0.11 |
| marked compiled but no block (the wait loop) | 3.21 | 0 | 0 | 0 |
| never compiled: branch packet | 0.53 | 0.66 | 0.53 | 0.16 |
| never compiled: control-register access | 0.22 | 0.24 | 0.22 | 0.11 |
| newly decoded, below threshold, mid-block bail | 0.04 | 0.15 | 0.05 | 0.06 |
| never compiled: BIOS table or breakpoint | 0 | 0 | 0 | 0.18 |
| **WB** delay slot of an interpreted branch | 1.28 | 1.33 | 1.23 | 0.02 |
| marked compiled but no block (the wait loop) | 0.77 | 0 | 0 | 0 |
| never compiled: branch packet | 1.24 | 1.28 | 1.23 | 0.02 |
| never compiled: control-register access | 0.44 | 0.40 | 0.41 | 0.01 |
| newly decoded, below threshold, mid-block bail | 0.01 | 0.03 | 0.01 | 0.01 |
| never compiled: BIOS table or breakpoint | 0 | 0 | 0 | 0.26 |

The "never compiled" kinds are inferred from the packet's flags when it is interpreted (a branch packet with delay-slot blockers, a control-register access with side effects, the BIOS jump-table range or a breakpoint), not recorded when the compiler refused it; a never-compiled packet that is also in a delay slot counts as never compiled.

Turns by packets run (percentage of all turns, MPE0 / MPE1 / MPE2): one packet 71 / 29 / 20 in WA and 64 / 48 / 43 in WB; 32 or more 11 / 13 / 11 in WA and 14 / 14 / 12 in WB. Most running turns are one packet and 11 to 14% reach the budget; the full histograms are in the appendix.

Not-running turns (MPE1 and MPE2 halted between jobs) are 49% and 62% of their turns in WA and 20% and 29% in WB: the round-robin keeps calling them and each such call returns at once.

Time split of the thread (percentage of the thread's wall time):

| state (percentage of the thread's wall time, mean ± sd over runs) | shipped limit, WA | shipped limit, WB | saturated, WA | saturated, WB |
|---|---|---|---|---|
| generated code | 49.14 ± 0.54 | 48.14 ± 0.47 | 57.17 ± 0.37 | 55.33 ± 0.11 |
| interpreter | 5.61 ± 0.12 | 7.18 ± 0.20 | 6.18 ± 0.11 | 8.08 ± 0.13 |
| asleep | 11.06 ± 0.36 | 11.03 ± 0.50 | 0.00 ± 0.00 | 0.00 ± 0.00 |
| dispatcher proper | 14.52 ± 0.48 | 15.38 ± 0.66 | 16.13 ± 0.21 | 17.84 ± 0.16 |
| BIOS and PE handlers | 0.01 ± 0.00 | 0.01 ± 0.00 | 0.01 ± 0.00 | 0.01 ± 0.00 |
| compiling | 0.13 ± 0.01 | 0.03 ± 0.00 | 0.14 ± 0.02 | 0.03 ± 0.01 |
| chain loop between blocks | 4.85 ± 0.09 | 4.96 ± 0.02 | 5.48 ± 0.17 | 5.49 ± 0.15 |
| round level | 2.22 ± 0.13 | 1.84 ± 0.10 | 1.92 ± 0.08 | 1.61 ± 0.08 |
| cache flush loops | 1.22 ± 0.03 | 0.27 ± 0.02 | 1.41 ± 0.07 | 0.30 ± 0.04 |
| overlay hashing | 0.12 ± 0.01 | 0.03 ± 0.00 | 0.13 ± 0.02 | 0.03 ± 0.01 |
| packet decode | 0.46 ± 0.01 | 0.10 ± 0.01 | 0.54 ± 0.04 | 0.12 ± 0.01 |
| media tick and loop between turns | 6.06 ± 0.43 | 6.36 ± 0.67 | 6.33 ± 0.17 | 6.61 ± 0.16 |
| NUANCE_PERF counting | 4.61 ± 0.70 | 4.67 ± 0.96 | 4.56 ± 0.32 | 4.56 ± 0.18 |
| (the old "dispatcher and the rest": all but generated code, interpreter and sleep) | 34.2 ± 0.4 | 33.7 ± 0.9 | 36.7 ± 0.3 | 36.6 ± 0.1 |
| (the same, without NUANCE_PERF counting) | 29.6 ± 0.4 | 29.0 ± 0.3 | 32.1 ± 0.4 | 32.0 ± 0.2 |

Polling turns (a turn that begins at the pcexec the same MPE's previous running turn began at) are 0.3 to 0.4 million a second per MPE (4% of MPE0's turns in WA, 5.5% in WB): single-address wait loops are a small share of the turns. The wait loop of MPE0 is mostly a loop of several packets, which this counter does not catch; the interpreted-packet table shows it (3.2 million a second in WA, marked compiled but no block).

Cache and overlay costs, shipped limit, WA / WB: overlay lookups and interpreter cache flushes 1,200 to 2,000 a second per MPE in WA and 230 to 330 in WB; native cache flushes (overlay region only; never the 16-million-address main and system bus flush, never the whole cache) were 0.9 to 1.1 a second per MPE in WA and 0.1 in WB, and the `FlushRegion` loops walked 3,800 to 7,600 addresses a second per MPE in WA and 300 to 700 in WB: the byte-by-byte `FlushRegion` is not a cost here. What costs is the interpreter-cache invalidation, an 8192-entry scan at every overlay change: the "cache flush loops" state is 1.2% of the thread in WA and 0.3% in WB.

## 7. A saturated benchmark on the Mac (C2)

**What was done.** Section 3.1 of the plan recommends, to compare builds, the scripted run with the speed limit removed so that the emulation thread is the bottleneck (`NUANCE_MPE_HZ=0`; the thread then never sleeps). A save state is the plan's other route and it was deferred there; I did not implement it (the plan puts it at four to seven days, and there is no loader for the dumps that exist). So the benchmark here is the simplest stand-in: the same scripted scene as the baseline, with `NUANCE_MPE_HZ=0`, so that the frame rate measures how fast the host can run the game on that scene. The frame cap of the Pad runs (`[MaxFrameRate]`) is disabled in the repository's `nuance.cfg` and was left so.

**What it represents and what it does not.** It represents a thread that is never idle on the scripted scene: the asleep share is 0.0% in every window (the plan's assumption A1, a saturated thread, holds). The scene is the start of level one and the stretch after it, played by a fixed script, not a real busy scene from play: nobody has recorded or replayed one (section 10). The game is not the same game from run to run (the MPEs run at host speed and the timers are on the host's clock), but the frame numbers at which the stretches begin and end repeat to a few frames, and the spread of the frame rate over runs is 2.2% (WA, with one run of a single window at 58.0) and 1.4% (WB).

**Is it representative of the shipped-limit profile?** The plan asks for the saturated profile to be compared with a real-play profile and to agree within about a fifth. There is no real-play log here, so the comparison made is with the shipped-limit baseline of section 6, which is the same scene with the limit on:

| scene | MPE | packets per running turn: shipped / saturated | blocks per native turn: shipped / saturated | turns ended by an interpreted packet %: shipped / saturated | by budget % | by no block % | largest relative difference |
|---|---|---|---|---|---|---|---|
| WA | MPE0 | 6.37 / 7.02 | 2.26 / 2.40 | 57.72 / 55.78 | 10.63 / 11.79 | 31.58 / 32.37 | 10.9% |
| WA | MPE1 | 13.22 / 13.35 | 4.22 / 4.32 | 27.99 / 28.13 | 13.10 / 13.13 | 9.96 / 10.21 | 2.5% |
| WA | MPE2 | 14.87 / 15.11 | 3.99 / 4.12 | 19.10 / 20.79 | 11.23 / 12.23 | 7.58 / 8.46 | 11.5% |
| WA | MPE3 | 1.24 / 1.25 | 1.00 / 1.00 | 9.12 / 8.84 | 0.00 / 0.00 | 0.00 / 0.00 | 3.0% |
| WB | MPE0 | 8.67 / 8.85 | 4.01 / 4.00 | 60.16 / 59.30 | 14.31 / 14.69 | 25.52 / 26.00 | 2.7% |
| WB | MPE1 | 9.88 / 9.96 | 4.49 / 4.48 | 47.55 / 49.25 | 13.48 / 14.17 | 18.45 / 19.06 | 5.1% |
| WB | MPE2 | 9.68 / 9.75 | 4.42 / 4.42 | 42.61 / 46.64 | 11.81 / 13.12 | 17.05 / 18.62 | 11.2% |
| WB | MPE3 | 1.29 / 1.29 | 1.00 / 1.00 | 5.33 / 5.32 | 0.00 / 0.00 | 0.00 / 0.00 | 0.2% |

The profile (blocks per turn, packets per turn, why turns end) agrees within 12% in every cell for the three rendering MPEs, so removing the limit does not change what a turn is made of; only more of them are run (rounds a second 8.4 and 6.9 million against 8.9 and 6.9). The saturated host reaches 55.2 frames a second in WA (against 49.8 at the shipped limit, +11%) and 29.2 in WB (against 25.5, +14%): the same size as the 11% of the thread's time that is asleep at the shipped limit. At the shipped limit, therefore, the Mac is not host-bound in these stretches (MPE0 is at its 54-million allowance) and a gain in thread speed would show there only as a longer sleep, not as a higher frame rate. The frame-rate aim of 30 is reached on the Mac only above hardware speed in WB (29.2, and 30.8 with a longer budget).

**Results**, seven runs of 80 seconds (`NUANCE_MPE_HZ=0`), same two stretches (WA has two windows in all runs but one, WB three):

| | WA, game frames 770-1550 | WB, game frames 1770-2320 |
|---|---|---|
| game frames a second, mean ± sd (min-max) | 55.2 ± 1.2 (54.3-58.0) | 29.2 ± 0.4 (28.7-29.7) |
| rounds a second (millions) | 8.38 ± 0.16 | 6.90 ± 0.09 |
| rounds per game frame (thousands, derived) | 152 | 236 |
| packets per round, MPE0 to MPE2 (derived) | 20.2 | 24.7 |
| running turns per round (derived) | 2.93 | 3.61 |
| thread asleep | 0.0% | 0.0% |

| | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| **WA** packets a second (millions) | 58.8 | 57.6 | 52.5 | 10.4 |
| running turns a second (millions) | 8.38 | 4.31 | 3.48 | 8.38 |
| native turns | 3.71 | 1.96 | 1.73 | 7.40 |
| chain steps | 5.18 | 6.49 | 5.41 | 0 |
| packets per running turn | 7.02 | 13.35 | 15.11 | 1.25 |
| blocks per native turn | 2.40 | 4.32 | 4.12 | 1.00 |
| interpreted packets | 4.68 | 2.36 | 1.74 | 0.74 |
| turns ended by an interpreted packet | 55.8% | 28.1% | 20.8% | 8.8% |
| by the budget | 11.8% | 13.1% | 12.2% | 0 |
| by no block | 32.4% | 10.2% | 8.5% | 0 |
| **WB** packets a second (millions) | 61.1 | 56.7 | 52.7 | 8.9 |
| running turns a second (millions) | 6.90 | 5.69 | 5.41 | 6.90 |
| native turns | 2.81 | 2.29 | 2.19 | 6.23 |
| chain steps | 8.41 | 7.99 | 7.49 | 0 |
| packets per running turn | 8.85 | 9.96 | 9.75 | 1.29 |
| blocks per native turn | 4.00 | 4.48 | 4.42 | 1.00 |
| interpreted packets | 4.09 | 3.40 | 3.22 | 0.37 |
| turns ended by an interpreted packet | 59.3% | 49.2% | 46.6% | 5.3% |
| by the budget | 14.7% | 14.2% | 13.1% | 0 |
| by no block | 26.0% | 19.1% | 18.6% | 0 |

The thread's time split in this setting is the right two columns of the table of section 6. The cost of one round is worked out in section 9.

The full per-run tables, with spread, are appendices A to D.

## 8. The no-code experiments of section 3.2 (C3)

### 8.1 E1: the chain budget sweep

Saturated (`NUANCE_MPE_HZ=0`), `NUANCE_A64_CHAIN` at 0, 8, 32 (the default) and 128, four runs of 80 seconds for each, in rotating order (0 8 32 128, 8 32 128 0, 32 128 0 8, 128 0 8 32). "fps" is the game's frame rate over the windows inside each stretch; "vs 32" is the ratio of means. No run hung (the plan's worry for a longer budget), and no run was left out.

### WA (game frames 770-1550)

| NUANCE_A64_CHAIN | runs | fps per run | fps mean ± sd | vs 32 | rounds/s M mean ± sd | running turns/s M | chain steps/s M | blocks/s M | packets/s M (MPE0-2) | busy % |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 4 | 40.5, 40.3, 40.3, 41.2 | 40.6 ± 0.4 | 0.74 | 11.51 ± 0.03 | 38.21 | 0.00 | 29.49 | 133.5 | 100 |
| 8 | 4 | 45.8, 45.4, 45.8, 46.2 | 45.8 ± 0.3 | 0.84 | 9.79 ± 0.08 | 30.81 | 8.79 | 30.27 | 151.1 | 100 |
| 32 | 4 | 54.8, 54.3, 55.1, 55.0 | 54.8 ± 0.4 | 1.00 | 8.41 ± 0.13 | 24.60 | 17.08 | 31.91 | 168.2 | 100 |
| 128 | 4 | 56.4, 55.0, 56.1, 56.5 | 56.0 ± 0.7 | 1.02 | 8.17 ± 0.05 | 22.76 | 20.11 | 32.33 | 175.8 | 100 |

#### WB (game frames 1770-2320)

| NUANCE_A64_CHAIN | runs | fps per run | fps mean ± sd | vs 32 | rounds/s M mean ± sd | running turns/s M | chain steps/s M | blocks/s M | packets/s M (MPE0-2) | busy % |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 4 | 21.1, 20.8, 21.2, 20.5 | 20.9 ± 0.3 | 0.71 | 11.29 ± 0.10 | 41.55 | 0.00 | 32.55 | 122.1 | 100 |
| 8 | 4 | 26.2, 25.9, 26.0, 26.1 | 26.0 ± 0.2 | 0.89 | 8.89 ± 0.07 | 32.49 | 13.10 | 35.34 | 149.9 | 100 |
| 32 | 4 | 29.5, 29.5, 29.5, 29.0 | 29.4 ± 0.3 | 1.00 | 6.92 ± 0.06 | 25.00 | 24.02 | 37.59 | 171.3 | 100 |
| 128 | 4 | 30.8, 31.0, 30.8, 30.7 | 30.8 ± 0.1 | 1.05 | 6.46 ± 0.02 | 23.16 | 27.46 | 38.73 | 179.8 | 100 |

Two settings are distinguishable when the difference of means exceeds about two standard deviations of the larger spread. By that: 0, 8 and 32 differ clearly from each other in both stretches (WA 40.6, 45.8, 54.8; WB 20.9, 26.0, 29.4). 128 against 32 is not distinguishable in WA (56.0 ± 0.7 against 54.8 ± 0.4: a difference of 1.2, twice the larger spread is 1.4) and is a gain in WB (30.8 ± 0.1 against 29.4 ± 0.3, +5%).

Chain 0 makes every block a turn. What that costs per turn, derived from the table (extra frame time divided by the extra running turns per frame; the work done per frame is not exactly constant, because the MPEs' wait loops do more when turns are cut differently, so these are rough):

**WA**

| NUANCE_A64_CHAIN | frame time ms | running turns per frame (thousand) | blocks per frame (thousand) |
|---|---|---|---|
| 0 | 24.64 | 941 | 727 |
| 8 | 21.83 | 673 | 661 |
| 32 | 18.25 | 449 | 582 |
| 128 | 17.86 | 406 | 577 |

| pair | extra frame time ms | extra turns per frame (thousand) | ns per extra turn (derived) |
|---|---|---|---|
| 0 vs 32 | 6.39 | 492 | 13.0 |
| 8 vs 32 | 3.58 | 224 | 16.0 |
| 32 vs 128 | 0.39 | 43 | 9.2 |

**WB**

| NUANCE_A64_CHAIN | frame time ms | running turns per frame (thousand) | blocks per frame (thousand) |
|---|---|---|---|
| 0 | 47.83 | 1988 | 1557 |
| 8 | 38.39 | 1247 | 1357 |
| 32 | 34.05 | 851 | 1280 |
| 128 | 32.44 | 751 | 1256 |

| pair | extra frame time ms | extra turns per frame (thousand) | ns per extra turn (derived) |
|---|---|---|---|
| 0 vs 32 | 13.78 | 1136 | 12.1 |
| 8 vs 32 | 4.34 | 396 | 10.9 |
| 32 vs 128 | 1.61 | 100 | 16.1 |

An extra turn costs 9 to 16 ns, against 2.3 to 3.2 ns for a chain step by the sampler (section 9). This agrees with the plan's estimate of about 15 ns for a turn (section 2.4 of the plan). The budget's marginal gain falls off: going from 8 to 32 packets saves 3.6 to 4.3 ms a frame, from 32 to 128 saves 0.4 to 1.6 ms. So a longer budget is a cheap lever worth a few percent in the heavier stretch, and nothing in the first, but the plan's note stands that it changes how the MPEs interleave.

### 8.2 E2 and E3

**E2** asks for the existing perf log in real play (Jake playing). That was not done; no real-play log exists here. What the log gives for the scripted scenes (blocks and packets per block, interpreted packets and their reasons, never-compilable packets, overlay lookups, native and interpreter cache flushes) is in sections 6 and 7 and the appendices. The flush rate that the plan said decides how Stage 2 invalidates links: at the shipped limit native cache flushes are 0.9 to 1.1 a second per MPE in WA and 0.1 in WB (overlay region flushes only), and the interpreter cache is flushed (an 8192-entry scan) 1,200 to 2,000 times a second per MPE in WA and 230 to 330 in WB there (1,400 to 2,300 and 260 to 380 when saturated). Native flushes are what would unlink direct links, and they happen about once a second per MPE.

**E3** (`NUANCE_A64_SHOW_BLOCKERS=1`, the first 400 branches refused for a delay-slot blocker, one run of 70 seconds saturated, `logs/e3.log`): by what blocks the branch.

| Blocker (slot 1, slot 2) | Count of the 400 |
|---|---|
| slot 2 holds an ECU (branch) instruction | 223 |
| slot 1 holds a register-indirect memory access | 55 |
| slot 2 holds a register-indirect memory access | 40 |
| slot 2 holds a packet marked never to compile | 36 |
| slot 1 holds an ECU instruction | 26 |
| slot 1 holds a `,nop` branch | 18 |
| two blockers (slot 1 ECU and indirect, or slot 1 `,nop` branch and slot 2 indirect) | 2 |

The interpreted-packet top list of the perf log in a window of WB (saturated, run `sat_4`, game frames 1842 to 1996) is the same loop on all three rendering MPEs:

```
interpreted 405280/s MPE2 20300C4E (never compiled): add r8, r9, r9 bra gt, $20300BF2
interpreted 391641/s MPE0 20300C4E (never compiled): add r8, r9, r9 bra gt, $20300BF2
interpreted 385529/s MPE1 20300C4E (never compiled): add r8, r9, r9 bra gt, $20300BF2
interpreted 321640/s MPE2 20300C5A: addm r6, r3, r6
interpreted 319967/s MPE2 20300C52: sub r9, r24, r9 bra $20300C34, nop
interpreted 311371/s MPE0 20300C5A: addm r6, r3, r6
interpreted 311291/s MPE0 20300C52: sub r9, r24, r9 bra $20300C34, nop
interpreted 306981/s MPE1 20300C52: sub r9, r24, r9 bra $20300C34, nop
interpreted 306914/s MPE1 20300C5A: addm r6, r3, r6
interpreted 293493/s MPE3 80000060 (never compiled): bra $F0000018, nop
interpreted 183987/s MPE1 20300ACC (never compiled): cmp r22, r0 bra gt, $20300AF6
interpreted 183452/s MPE0 20300AD4 (never compiled): cmp r20, r1 bra gt, $20300AF6
```

(The counts are one in sixteen of the packets, scaled.) A conditional `bra gt` whose first delay slot is `bra $20300C34, nop` (S1.2 in the plan: a branch in the first delay slot), the two packets that follow it interpreted as delay slots, run by all three MPEs: the nine entries for this one loop add up to 3.06 million interpreted packets a second, of the 10.7 million of MPE0 to MPE2 in this stretch (section 9).

### 8.3 E4: the cost of a block call

`NUANCE_A64_E4=200000000` (new, section 2.4), five runs on the final build, each timing 200 million calls from a C++ loop: an empty C++ function 0.74 to 0.77 ns; a block of the compiler's shape with one absolute store (prologue, the two register-file pointers, the store, epilogue) 1.51 to 1.52 ns; the same with two more absolute stores 1.54 to 1.55 ns. So call, prologue and epilogue of a block cost about 0.76 ns more than a C++ call, and each further store about 0.02 ns, in a loop that calls the same block every time with every branch predicted and everything in the level-1 cache. It is a lower bound: in the emulator the blocks differ, the indirect call is predicted worse and the register-file pointers and `pcexec` are loaded cold. It is the `c_pe` of the plan's rule (section 9).

## 9. The arithmetic of section 3.4, and what the rule says

Method. The inputs are the means over the seven saturated runs (section 7), for each of the two stretches. "Share of wall time" is the sampler's share (measured). Because NUANCE_PERF's own counting is 4.6% of the wall time and would not be there in normal use, every share *of busy time* below is the sampler's share divided by `b`, where `b` = 1 - asleep - counting = 0.954; the nanosecond figures divide the measured wall-time share by the measured event rate of the same runs, so they include nothing of the counting. Rates are per second of wall time and in millions. Every line is marked measured or derived. The scripts that produce the working are `tools/stage0/arith.py sat 770 1550` and `... sat 1770 2320`; their full output follows.

**(1) Block-to-block transition inside turns** (the genuine transition of a chain step, the plan's s_6). Measured: the chain-loop state is 5.48% of wall time in WA and 5.49% in WB. Derived: 5.74% and 5.75% of busy time; per chain step 3.21 ns (WA) and 2.30 ns (WB) (share x 1 s / chain steps: 17.08 and 23.89 million a second). State 6 also holds the last iteration of every chain that ends by a break (it runs the tests and leaves); counting those iterations as well, per iteration 2.56 ns (WA) and 1.94 ns (WB); the iterations that a link could replace are the chain steps, 80% (WA) and 84% (WB) of them.

**(2) Block entry and exit.** Not separable: the call instruction, the block's prologue and epilogue are in the same sampler state as its body (state 1). A lower bound from the E4 micro-benchmark (measured 0.76 ns more than a C++ call, section 8.3): blocks a second (derived: native turns plus chain steps) 31.88 million (WA) and 37.41 million (WB) times 0.78 ns (the E4 figure used by the script) is 2.61% and 3.06% of busy time (derived). The in-place cost is higher than the lower bound by an amount that was not measured.

**(3) The interpreter and the turns it ends.** Measured: the interpreter state is 6.18% (WA) and 8.08% (WB) of wall time, 6.47% and 8.46% of busy time (derived). Each interpreted packet is a turn: derived, 9.52 and 11.07 million interpreted packets a second; the dispatcher's cost per turn, c_turn, is the state-0 share over the running turns (16.13% of a second over 24.56 million running turns = 6.6 ns in WA; 17.84% over 24.90 million = 7.2 ns in WB; derived); so the turns the interpreted packets end cost 6.25% (WA) and 7.93% (WB) of wall time (derived: interpreted packets a second x c_turn). Together with the interpreter's own time that is 13.0% and 16.8% of busy time (derived).

**(4) One round.** Measured: 8.38 million rounds a second in WA and 6.90 million in WB. Derived: busy time per round (b x 1 s / rounds) 113.8 ns (WA) and 138.3 ns (WB); wall time per round 119.3 and 144.9 ns, the difference being the counting. It divides as follows (nanoseconds per round, WA / WB, derived as share x 1 s / rounds; the shares are the section 6 table):

| | WA | WB |
|---|---|---|
| generated code | 68.2 | 80.2 |
| dispatcher proper (the start and end of every turn, lookups) | 19.2 | 25.8 |
| interpreter | 7.4 | 11.7 |
| chain loop between blocks | 6.5 | 8.0 |
| media tick and the loop between turns | 7.5 | 9.6 |
| round level (comm bus, clock, timers, audio, scheduled interrupts) | 2.3 | 2.3 |
| cache flush loops, overlay hashing, packet decode, compiling, handlers | 2.7 | 0.7 |
| NUANCE_PERF counting (not part of normal use) | 5.4 | 6.6 |

A round is, on average, 2.9 to 3.6 running turns (MPE3 one of them), about 20 to 25 packets of MPE0 to MPE2 plus MPE3's 1.3, and 4 to 5 blocks a turn for MPE1 and MPE2, 2 to 4 for MPE0. The plan's estimate of 130 ns a round and about 20 packets a round (section 2.4 of the plan) held: 114 to 138 ns and 20 to 25 packets.

The working, first stretch (WA), as printed by the script:

```
runs 7, windows [2, 2, 2, 2, 2, 1, 2], fps 55.2
shares of wall time (measured, sampler): generated code 57.17%, interpreter 6.18%, asleep 0.00%, dispatcher proper 16.13%, BIOS and PE handlers 0.01%, compiling 0.14%, chain loop between blocks 5.48%, round level 1.92%, cache flush loops 1.41%, overlay hashing 0.13%, packet decode 0.54%, media tick and loop between turns 6.33%, NUANCE_PERF counting 4.56%
busy fraction b = 1 - asleep - counting = 0.9544 (counting, the cost of NUANCE_PERF itself, is 4.56% of wall time and is left out of every share of busy time below; the ns figures use the measured wall-time shares and event rates of the same run)
rounds/s 8.383M (measured); running turns/s 24.558M; native turns/s 14.801M; chain steps/s 17.081M; blocks/s 31.882M; interpreted packets/s 9.518M
(1) block-to-block transition inside turns: s_6 = 5.48% of wall time = 5.74% of busy time (derived: s_6/b); per chain step 3.21 ns (derived: s_6 x 1e9 / steps)
    state 6 also holds the last loop iteration of every chain that ends by a break (it runs the tests and then leaves): iterations/s = steps + native turns of MPE0-2 - turns ended by budget = 17.081M + 7.398M - 3.114M = 21.365M; per iteration 2.56 ns (derived)
(2) block entry and exit: not separable by the sampler (the call, prologue and epilogue are in state 1 with the block body). Lower bound from the E4 micro-benchmark: 0.78 ns per call, so 2.61% of busy time (derived: blocks/s x c_pe / b)
(3) interpreter: s_2 = 6.18% of wall time = 6.47% of busy; the turns it ends: interpreted packets/s x c_turn where c_turn = s_0 x 1e9 / running turns = 6.6 ns (derived), giving 6.25% of wall time (6.55% of busy). Together 13.02% of busy time
(4) one round: busy time / rounds = 113.8 ns (derived: b x 1e9 / rounds); wall time per round 119.3 ns
      generated code                 68.2 ns per round (derived: share x 1e9 / rounds),  59.9% of busy
      interpreter                     7.4 ns per round (derived: share x 1e9 / rounds),   6.5% of busy
      dispatcher proper              19.2 ns per round (derived: share x 1e9 / rounds),  16.9% of busy
      BIOS and PE handlers            0.0 ns per round (derived: share x 1e9 / rounds),   0.0% of busy
      compiling                       0.2 ns per round (derived: share x 1e9 / rounds),   0.2% of busy
      chain loop between blocks       6.5 ns per round (derived: share x 1e9 / rounds),   5.7% of busy
      round level                     2.3 ns per round (derived: share x 1e9 / rounds),   2.0% of busy
      cache flush loops               1.7 ns per round (derived: share x 1e9 / rounds),   1.5% of busy
      overlay hashing                 0.2 ns per round (derived: share x 1e9 / rounds),   0.1% of busy
      packet decode                   0.6 ns per round (derived: share x 1e9 / rounds),   0.6% of busy
      media tick and loop between turns    7.6 ns per round (derived: share x 1e9 / rounds),   6.6% of busy
      NUANCE_PERF counting            5.4 ns per round (derived: share x 1e9 / rounds),   4.8% of busy
D2 (plan 3.4) with c_pe 0.78 ns (E4), c_link 1.0 ns: s_6/b + steps x (c_pe - c_link)/b = 5.74% + -0.39% = 5.34%
D2 (plan 3.4) with c_pe 0.78 ns (E4), c_link 2.0 ns: s_6/b + steps x (c_pe - c_link)/b = 5.74% + -2.18% = 3.55%
     c_pe needed (in place of E4) for D2 = 15% with c_link 1.5 ns: 6.7 ns
D1 (plan 3.4) = [s_2 x 2/3 + n_int x c_turn + (turns ended by a never-compilable next packet = 2.182M/s) x (c_turn - c_step)] / b = [4.12% + 6.25% + 0.73%]/b = 11.63%
   (turns ended for lack of any block: 4.278M/s of which never compilable 2.182M/s)
interpreted packets/s by reason, MPE0-2 (measured): delay slot of an interpreted branch 2.900M, mid-block bail 0.005M, newly decoded 0.198M, marked compiled but no block 2.954M, below the threshold 0.060M, BIOS table or breakpoint 0.000M, branch packet 1.893M, control register 0.767M, other 0.001M
   of which Stage-1 addressable (delay slots, bails, never-compiled of any kind): 5.565M/s; marked-compiled-but-no-block (the R9 wait loop): 2.954M/s; newly decoded or below threshold: 0.258M/s
   interpreter and cache flush: interpreter cache flushes/s (MPE0-3, measured): 1367, 2329, 1422, 0; overlay lookups/s: 1367, 2329, 1422, 0
D1 restricted to the Stage-1-addressable interpreted packets (share of MPE0-2 interpreted packets 63%): [s_2 x 2/3 x share + addressable x c_turn]/b = 6.57%
```

The working, second stretch (WB):

```
runs 7, windows [3, 3, 3, 3, 3, 3, 3], fps 29.2
shares of wall time (measured, sampler): generated code 55.33%, interpreter 8.08%, asleep 0.00%, dispatcher proper 17.84%, BIOS and PE handlers 0.01%, compiling 0.03%, chain loop between blocks 5.49%, round level 1.61%, cache flush loops 0.30%, overlay hashing 0.03%, packet decode 0.12%, media tick and loop between turns 6.61%, NUANCE_PERF counting 4.56%
busy fraction b = 1 - asleep - counting = 0.9544 (counting, the cost of NUANCE_PERF itself, is 4.56% of wall time and is left out of every share of busy time below; the ns figures use the measured wall-time shares and event rates of the same run)
rounds/s 6.900M (measured); running turns/s 24.899M; native turns/s 13.522M; chain steps/s 23.893M; blocks/s 37.414M; interpreted packets/s 11.075M
(1) block-to-block transition inside turns: s_6 = 5.49% of wall time = 5.75% of busy time (derived: s_6/b); per chain step 2.30 ns (derived: s_6 x 1e9 / steps)
    state 6 also holds the last loop iteration of every chain that ends by a break (it runs the tests and then leaves): iterations/s = steps + native turns of MPE0-2 - turns ended by budget = 23.893M + 7.291M - 2.897M = 28.287M; per iteration 1.94 ns (derived)
(2) block entry and exit: not separable by the sampler (the call, prologue and epilogue are in state 1 with the block body). Lower bound from the E4 micro-benchmark: 0.78 ns per call, so 3.06% of busy time (derived: blocks/s x c_pe / b)
(3) interpreter: s_2 = 8.08% of wall time = 8.46% of busy; the turns it ends: interpreted packets/s x c_turn where c_turn = s_0 x 1e9 / running turns = 7.2 ns (derived), giving 7.93% of wall time (8.31% of busy). Together 16.77% of busy time
(4) one round: busy time / rounds = 138.3 ns (derived: b x 1e9 / rounds); wall time per round 144.9 ns
      generated code                 80.2 ns per round (derived: share x 1e9 / rounds),  58.0% of busy
      interpreter                    11.7 ns per round (derived: share x 1e9 / rounds),   8.5% of busy
      dispatcher proper              25.8 ns per round (derived: share x 1e9 / rounds),  18.7% of busy
      BIOS and PE handlers            0.0 ns per round (derived: share x 1e9 / rounds),   0.0% of busy
      compiling                       0.0 ns per round (derived: share x 1e9 / rounds),   0.0% of busy
      chain loop between blocks       8.0 ns per round (derived: share x 1e9 / rounds),   5.7% of busy
      round level                     2.3 ns per round (derived: share x 1e9 / rounds),   1.7% of busy
      cache flush loops               0.4 ns per round (derived: share x 1e9 / rounds),   0.3% of busy
      overlay hashing                 0.0 ns per round (derived: share x 1e9 / rounds),   0.0% of busy
      packet decode                   0.2 ns per round (derived: share x 1e9 / rounds),   0.1% of busy
      media tick and loop between turns    9.6 ns per round (derived: share x 1e9 / rounds),   6.9% of busy
      NUANCE_PERF counting            6.6 ns per round (derived: share x 1e9 / rounds),   4.8% of busy
D2 (plan 3.4) with c_pe 0.78 ns (E4), c_link 1.0 ns: s_6/b + steps x (c_pe - c_link)/b = 5.75% + -0.55% = 5.20%
D2 (plan 3.4) with c_pe 0.78 ns (E4), c_link 2.0 ns: s_6/b + steps x (c_pe - c_link)/b = 5.75% + -3.05% = 2.70%
     c_pe needed (in place of E4) for D2 = 15% with c_link 1.5 ns: 5.2 ns
D1 (plan 3.4) = [s_2 x 2/3 + n_int x c_turn + (turns ended by a never-compilable next packet = 3.914M/s) x (c_turn - c_step)] / b = [5.38% + 7.93% + 1.90%]/b = 15.95%
   (turns ended for lack of any block: 4.394M/s of which never compilable 3.914M/s)
interpreted packets/s by reason, MPE0-2 (measured): delay slot of an interpreted branch 4.286M, mid-block bail 0.001M, newly decoded 0.037M, marked compiled but no block 0.783M, below the threshold 0.018M, BIOS table or breakpoint 0.000M, branch packet 4.189M, control register 1.393M, other 0.000M
   of which Stage-1 addressable (delay slots, bails, never-compiled of any kind): 9.869M/s; marked-compiled-but-no-block (the R9 wait loop): 0.783M/s; newly decoded or below threshold: 0.055M/s
   interpreter and cache flush: interpreter cache flushes/s (MPE0-3, measured): 263, 380, 292, 0; overlay lookups/s: 263, 380, 292, 0
D1 restricted to the Stage-1-addressable interpreted packets (share of MPE0-2 interpreted packets 92%): [s_2 x 2/3 x share + addressable x c_turn]/b = 12.61%
```

**D2, the gain of direct linking** (the plan's formula D2 = s_6 + n_step x (c_pe - c_link)). With the E4 block cost for c_pe (0.78 ns, measured but a lower bound) and the plan's 1 to 2 ns for a linked transition: 5.3% to 3.6% (WA) and 5.2% to 2.7% (WB) of busy time (derived). The table of the plan puts that at a speed-up of 1.03 to 1.06. Even if a link cost nothing at all, the whole chain loop plus the E4 block cost is 5.74 + 2.61 = 8.4% (WA) and 5.75 + 3.06 = 8.8% (WB). D2 would reach the plan's 15% only if the in-place cost of a block's entry and exit were 5 to 7 ns, which is 7 to 9 times E4's figure: the cost of the call, prologue and epilogue when the blocks differ and the indirect call is mispredicted may well be several times the hot-loop figure, but nothing measured here says it is that large.

**D1, the gain of compiling the packets still interpreted** (the plan's D1 = s_2 x 2/3 + n_int x c_turn + (turns that ended because the next packet is not compilable) x (c_turn - c_step)). Derived: WA 4.12% + 6.25% + 0.73% = 11.1% of wall time, 11.6% of busy time; WB 5.38% + 7.93% + 1.90% = 15.2% of wall time, 16.0% of busy time. The factor 2/3 (the interpreter costing three times the native code) is from arm64-jit-plan.md and was not measured here. Not all of these packets are what Stage 1 can compile: the wait loop of MPE0 ("marked compiled but no block") was compiled once with `NUANCE_A64_RECOMPILE_MARKED` and gave nothing at the time (arm64-jit-plan.md, 2026-11-16). Counting only the packets Stage 1 addresses (delay slots, never-compiled branch packets, control-register accesses and mid-block bails: 63% of the interpreted packets of MPE0 to MPE2 in WA and 92% in WB), D1 is 6.6% (WA) and 12.6% (WB) of busy time (derived).

**What the decision rule says to do next.** The plan's rule is: D2 under 8% is no-go for 2a (do 2c or stop); D1 above 8% goes; "if neither reaches 8%" look elsewhere; "if D1 and D2 are both above 8%" take the one with the cheaper first step.

- D2 is 2.7 to 5.3%, under 8%: no go for direct links (Stage 2a), as the rule says, and the rule's alternatives are 2c (a successor cache in the entry, a day, estimated at 3 to 6% in the plan) or stop. The E4 caveat above is the one thing that could change this and I cannot say it does.
- D1 is 11.6 to 16.0% of busy time, over 8% in both stretches (6.6% in WA and 12.6% in WB if only the packets Stage 1 can reach are counted): go for Stage 1, in the heavier stretch clearly and in the first stretch only if the wait loop of MPE0 is counted, which it should not be. The interpreted packets in the heavier stretch are one small loop on three MPEs, a conditional branch refused because its first delay slot holds a branch (S1.2), and their delay slots; 79% of the interpreted packets of MPE0 to MPE2 there are delay slots or never-compiled branch packets. The rule's order puts S1.2 (branch in the first delay slot) first.
- The plan's section 6 alternative that the numbers also support: letting a turn run on through an interpreted packet while the budget lasts would remove turns without compiling anything. Derived: interpreted packets a second times (the marginal cost of a turn from the sweep, 12 to 13 ns, less a chain step, 2.3 to 3.2 ns: about 9.8 ns) = 9.5 million x 9.8 ns = 9.3% (WA) and 11.1 million x 9.8 ns = 10.8% (WB) of the second. It changes where turns end and how the MPEs interleave, which is the kind of change that has hung the game, and it is complementary to Stage 1.
- Where the time is, in short (derived, busy time): the dispatcher proper and the end of the turn together are 23.5% (WA) and 25.6% (WB), generated code 60% and 58%, the interpreter 6.5% and 8.5%, the chain loop 5.7%; the part of the thread's time that Stage 2a addresses is the smallest of the four.

**Confidence.** The shares of the split and the rates are reproducible: the spread over runs is under a percentage point for every share, and the turn-end counts agree with an independent count of turns (to one turn a second in five lines of 2,360). The arithmetic is simple and shown. What it rests on and cannot show: (a) c_pe, the in-place cost of a block's entry and exit, is not measured, only a hot-loop lower bound, and D2 is the one result the rule's threshold is sensitive to; (b) the "one third" factor in D1 is from the old documents; (c) the marginal cost of a turn from E1 is confounded by changes in the MPEs' own work; (d) the scene is the scripted one, never a recorded busy scene; (e) at the shipped limit the Mac is not host-bound in these stretches, so a faster thread would not by itself raise the Mac's frame rate there (section 7). The Pad, where the plan says its numbers decide, was not touched.

## 10. Not measured

Everything in section 3 of the plan that was not done, and why.

- **A real-play session with the new counters** (3.1, E2, step 0.4: ten to fifteen minutes, Jake playing, read for the intervals below 30 frames a second). Needs a person; not done. As a consequence the saturated benchmark could not be compared with a real-play profile (3.1's "agree within about a fifth" test); it was compared with the shipped-limit profile of the same scripted scene instead (section 7), which agrees within 12%. Whether the scripted scene is like the real busy scenes is therefore not known.
- **A save state** (3.1): deferred by the plan (four to seven days) and not implemented. The scripted run is not deterministic from run to run, but the game frame numbers at which the stretches begin and end repeat to a few frames.
- **The Pad** (all of it): out of scope (Mac only; no device used). Every Pad figure the plan quotes (46% in the dispatcher, 16 frames a second saturated) is unchecked here.
- **Statistics of 3.1** as the plan specifies them: six runs of each arm, 120 seconds, alternating A B A B, discarding the first 60 seconds, on both machines. Done instead: five runs of the baseline and seven of the saturated benchmark in blocks, four per setting of the chain sweep and four per configuration of the cost check in rotating order, 80 to 95 seconds, windows chosen by game frame number rather than by time. Run-to-run spreads are tabulated everywhere; they are small (the frame rate's standard deviation is 0.4 to 2.2% of the mean), so a 5% difference would be clear.
- **Exit kind of every block exit, taken or not** (3.3 row 8; needed for Stage 2b): not implemented. It needs a new field in `NativeCodeCacheEntry` and a change in `src/SuperBlock.cpp` (a shared file) and was not needed for the rule's decision.
- **The polling-turns share of busy time** (3.3): the count of polling turns is reported; the time they take is not separated.
- **Compile events and their time** (3.3): the time is in the sampler (compiling is 0.14% of the thread in WA and 0.03% in WB); the number of compiles a second is not counted.
- **Block entry and exit in place** (3.4): not separable with the sampler; only E4's hot-loop lower bound (section 8.3). A better figure needs a disassembly of a block's prologue and epilogue and counters around the calls, or a cycle counter; not attempted.
- **The media tick on its own** (3.3 state): inseparable from the loop between turns without a second test per turn (section 2.3).
- **`NUANCE_A64_THRESHOLD`** (S1.5), **the relaxed test before the atomic exchange per round** and **a page-skipping `FlushRegion`** (0b): not part of Stage 0's measurements. The numbers say the last is not needed (the flush loops walk a few thousand addresses a second) and the round level is 1.6 to 2.2% of the thread, so the first two are worth at most that.
- **Merlin Racing and other games**: not run.
- **An x86 compile**: not possible on this Mac (no x86 toolchain for the shared code). The check made instead is described at the end of section 3.
- **Zero cost when `NUANCE_PERF` is off**: not achieved; 1.1% at saturation (section 5.2). A build-time switch that compiles the counting out would remove it entirely; not done, since the requirement was a runtime flag.
- **The report interval**: once every 300 `retro_run` calls (five seconds in `perf_host`) as before, not once a second.
- **Why the script of the task did not reach level one here.** Not known (section 4).
- **Housekeeping.** Each run unpacks about 5 MB of the disc image into the temporary directory and the core does not remove it, and `perf_host` ends in the segmentation fault noted in section 3.3; the 203 `nuance_*` directories (about 1 GB) that the runs of this stage left in `$TMPDIR` were not removed (the sandbox refused the deletion) and can be deleted by hand.

## Appendix: per-run tables
The tables below are written by `tools/stage0/report.py`; a run is a row, "mean ± sd" is over runs. Rates are per second of wall time in millions. Spreads are sample standard deviations.

### A. Shipped limit, WA (game frames 770-1550), runs base_1 to base_5

| run | windows (s) | game frames | fps | rounds/s (M) | asleep % |
|---|---|---|---|---|---|
| base_1 | 3 (15.0) | 797-1543 | 49.7 | 8.92 | 11.0 |
| base_2 | 3 (15.0) | 796-1544 | 49.9 | 8.89 | 10.9 |
| base_3 | 3 (15.0) | 798-1545 | 49.8 | 8.90 | 10.6 |
| base_4 | 3 (15.0) | 799-1546 | 49.8 | 9.00 | 11.3 |
| base_5 | 3 (15.0) | 798-1546 | 49.9 | 8.99 | 11.5 |
| mean ± sd (min–max) | | | 49.8 ± 0.1 (49.7–49.9) | 8.94 ± 0.05 (8.89–9.00) | 11.1 ± 0.4 (10.6–11.5) |

| packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 53.2 | 50.6 | 45.9 | 8.6 |
| base_2 | 53.2 | 50.7 | 46.0 | 8.6 |
| base_3 | 53.2 | 50.6 | 45.9 | 8.6 |
| base_4 | 53.3 | 50.7 | 46.0 | 8.6 |
| base_5 | 53.3 | 50.7 | 46.0 | 8.6 |
| mean ± sd | 53.3 ± 0.1 | 50.7 ± 0.0 | 46.0 ± 0.0 | 8.6 ± 0.0 |

| turns/s (M), all | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 8.34 | 7.52 | 8.13 | 6.90 |
| base_2 | 8.33 | 7.47 | 8.15 | 6.90 |
| base_3 | 8.34 | 7.53 | 8.14 | 6.94 |
| base_4 | 8.41 | 7.48 | 8.18 | 6.89 |
| base_5 | 8.40 | 7.54 | 8.16 | 6.90 |
| mean ± sd | 8.36 ± 0.04 | 7.51 ± 0.03 | 8.15 ± 0.02 | 6.91 ± 0.02 |

| running turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 8.34 | 3.83 | 3.09 | 6.90 |
| base_2 | 8.33 | 3.84 | 3.09 | 6.90 |
| base_3 | 8.34 | 3.83 | 3.09 | 6.94 |
| base_4 | 8.41 | 3.83 | 3.09 | 6.89 |
| base_5 | 8.40 | 3.83 | 3.09 | 6.90 |
| mean ± sd | 8.36 ± 0.04 | 3.83 ± 0.00 | 3.09 ± 0.00 | 6.91 ± 0.02 |

| native turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 3.53 | 1.73 | 1.53 | 6.09 |
| base_2 | 3.53 | 1.73 | 1.54 | 6.08 |
| base_3 | 3.53 | 1.73 | 1.53 | 6.12 |
| base_4 | 3.55 | 1.73 | 1.53 | 6.07 |
| base_5 | 3.55 | 1.73 | 1.53 | 6.08 |
| mean ± sd | 3.54 ± 0.01 | 1.73 ± 0.00 | 1.53 ± 0.00 | 6.09 ± 0.02 |

| chain steps/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 4.44 | 5.56 | 4.58 | 0.00 |
| base_2 | 4.44 | 5.58 | 4.59 | 0.00 |
| base_3 | 4.43 | 5.56 | 4.58 | 0.00 |
| base_4 | 4.44 | 5.57 | 4.58 | 0.00 |
| base_5 | 4.44 | 5.57 | 4.59 | 0.00 |
| mean ± sd | 4.44 ± 0.00 | 5.57 ± 0.01 | 4.59 ± 0.00 | 0.00 ± 0.00 |

| interpreted packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 4.81 | 2.10 | 1.55 | 0.63 |
| base_2 | 4.80 | 2.10 | 1.56 | 0.63 |
| base_3 | 4.82 | 2.10 | 1.56 | 0.63 |
| base_4 | 4.87 | 2.10 | 1.56 | 0.63 |
| base_5 | 4.85 | 2.10 | 1.56 | 0.63 |
| mean ± sd | 4.83 ± 0.03 | 2.10 ± 0.00 | 1.56 ± 0.00 | 0.63 ± 0.00 |

| packets per running turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 6.38 | 13.22 | 14.87 | 1.24 |
| base_2 | 6.39 | 13.22 | 14.87 | 1.24 |
| base_3 | 6.37 | 13.22 | 14.87 | 1.24 |
| base_4 | 6.34 | 13.22 | 14.87 | 1.24 |
| base_5 | 6.35 | 13.22 | 14.87 | 1.24 |
| mean ± sd | 6.37 ± 0.02 | 13.22 ± 0.00 | 14.87 ± 0.00 | 1.24 ± 0.00 |

| blocks per native turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 2.26 | 4.22 | 3.99 | 1.00 |
| base_2 | 2.26 | 4.22 | 3.99 | 1.00 |
| base_3 | 2.26 | 4.22 | 3.99 | 1.00 |
| base_4 | 2.25 | 4.21 | 3.99 | 1.00 |
| base_5 | 2.25 | 4.21 | 3.99 | 1.00 |
| mean ± sd | 2.26 ± 0.00 | 4.22 ± 0.00 | 3.99 ± 0.00 | 1.00 ± 0.00 |

| polling turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 0.33 | 0.31 | 0.33 | 0.06 |
| base_2 | 0.33 | 0.31 | 0.33 | 0.06 |
| base_3 | 0.33 | 0.31 | 0.33 | 0.06 |
| base_4 | 0.33 | 0.31 | 0.33 | 0.06 |
| base_5 | 0.33 | 0.31 | 0.33 | 0.06 |
| mean ± sd | 0.33 ± 0.00 | 0.31 ± 0.00 | 0.33 ± 0.00 | 0.06 ± 0.00 |

| turn ended by (% of all turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| interpreted packet | 57.7 ± 0.1 | 28.0 ± 0.1 | 19.1 ± 0.0 | 9.1 ± 0.0 |
| BIOS or PE call | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 2.7 ± 0.0 |
| budget | 10.6 ± 0.0 | 13.1 ± 0.1 | 11.2 ± 0.0 | 0.0 ± 0.0 |
| no block | 31.6 ± 0.0 | 10.0 ± 0.0 | 7.6 ± 0.0 | 0.0 ± 0.0 |
| mid-block bail | 0.1 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| interrupt | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| other native (chain off, branch pending, halted, icache, breakpoint, local memory dirty) | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 88.2 ± 0.0 |
| IL block | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| MPE not running | 0.0 ± 0.0 | 48.9 ± 0.2 | 62.1 ± 0.1 | 0.0 ± 0.0 |

| state | mean % of samples | sd | min | max |
|---|---|---|---|---|
| generated code | 49.14 | 0.54 | 48.54 | 49.83 |
| interpreter | 5.61 | 0.12 | 5.48 | 5.80 |
| asleep | 11.06 | 0.36 | 10.60 | 11.52 |
| dispatcher proper | 14.52 | 0.48 | 13.89 | 15.01 |
| BIOS and PE handlers | 0.01 | 0.00 | 0.01 | 0.01 |
| compiling | 0.13 | 0.01 | 0.11 | 0.15 |
| chain loop between blocks | 4.85 | 0.09 | 4.76 | 4.98 |
| round level | 2.22 | 0.13 | 2.07 | 2.39 |
| cache flush loops | 1.22 | 0.03 | 1.19 | 1.27 |
| overlay hashing | 0.12 | 0.01 | 0.11 | 0.14 |
| packet decode | 0.46 | 0.01 | 0.44 | 0.47 |
| media tick and loop between turns | 6.06 | 0.43 | 5.74 | 6.73 |
| NUANCE_PERF counting | 4.61 | 0.70 | 4.04 | 5.66 |

| interpreted packets by reason (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| delay slot of an interpreted branch | 0.830 | 1.056 | 0.762 | 0.110 |
| mid-block bail | 0.005 | 0.000 | 0.000 | 0.057 |
| newly decoded | 0.022 | 0.122 | 0.029 | 0.000 |
| marked compiled but no block | 3.213 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.010 | 0.025 | 0.017 | 0.004 |
| BIOS table or breakpoint | 0.000 | 0.000 | 0.000 | 0.184 |
| branch packet | 0.533 | 0.662 | 0.530 | 0.162 |
| control register | 0.216 | 0.235 | 0.218 | 0.114 |
| other | 0.000 | 0.001 | 0.000 | 0.000 |

| native turns ended for lack of a block, next packet (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| at a BIOS or PE address | 0.000 | 0.000 | 0.000 | 0.000 |
| never decoded | 0.004 | 0.019 | 0.005 | 0.000 |
| never compilable | 0.591 | 0.719 | 0.607 | 0.000 |
| marked compiled but no block | 2.041 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.005 | 0.010 | 0.006 | 0.000 |
| block not native | 0.000 | 0.000 | 0.000 | 0.000 |

| turns by packets run (% of turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| 0 | 0.0 | 48.9 | 62.1 | 0.0 |
| 1 | 70.5 | 28.8 | 20.1 | 85.9 |
| 2-3 | 6.5 | 0.1 | 0.4 | 14.1 |
| 4-7 | 6.6 | 0.4 | 0.2 | 0.0 |
| 8-15 | 2.1 | 4.1 | 2.2 | 0.0 |
| 16-31 | 3.6 | 4.5 | 3.8 | 0.0 |
| 32+ | 10.6 | 13.1 | 11.2 | 0.0 |

| flushes per second | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| overlay region | 0.9 | 1.1 | 0.9 | 0.0 |
| main and system bus ranges | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, past threshold | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, no room | 0.0 | 0.0 | 0.0 | 0.0 |
| FlushRegion calls | 0.9 | 1.1 | 0.9 | 0.0 |
| addresses walked | 7646.2 | 4369.2 | 3823.0 | 0.0 |

### B. Shipped limit, WB (game frames 1770-2320), runs base_1 to base_5

| run | windows (s) | game frames | fps | rounds/s (M) | asleep % |
|---|---|---|---|---|---|
| base_1 | 4 (20.0) | 1806-2315 | 25.4 | 6.90 | 10.6 |
| base_2 | 4 (20.0) | 1807-2316 | 25.5 | 6.94 | 11.6 |
| base_3 | 4 (20.0) | 1806-2316 | 25.5 | 6.93 | 11.6 |
| base_4 | 4 (20.0) | 1807-2316 | 25.4 | 6.90 | 10.8 |
| base_5 | 4 (20.0) | 1807-2316 | 25.4 | 6.91 | 10.6 |
| mean ± sd (min–max) | | | 25.5 ± 0.0 (25.4–25.5) | 6.91 ± 0.02 (6.90–6.94) | 11.0 ± 0.5 (10.6–11.6) |

| packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 53.7 | 50.1 | 46.7 | 7.6 |
| base_2 | 53.8 | 50.1 | 46.8 | 7.6 |
| base_3 | 53.8 | 50.1 | 46.8 | 7.6 |
| base_4 | 53.7 | 50.1 | 46.7 | 7.6 |
| base_5 | 53.8 | 50.1 | 46.8 | 7.6 |
| mean ± sd | 53.8 ± 0.1 | 50.1 ± 0.0 | 46.8 ± 0.0 | 7.6 ± 0.0 |

| turns/s (M), all | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 6.19 | 6.38 | 6.76 | 5.93 |
| base_2 | 6.21 | 6.37 | 6.77 | 5.89 |
| base_3 | 6.21 | 6.37 | 6.77 | 5.90 |
| base_4 | 6.19 | 6.39 | 6.76 | 5.93 |
| base_5 | 6.20 | 6.38 | 6.74 | 5.92 |
| mean ± sd | 6.20 ± 0.01 | 6.38 ± 0.01 | 6.76 ± 0.01 | 5.91 ± 0.02 |

| running turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 6.19 | 5.06 | 4.83 | 5.93 |
| base_2 | 6.21 | 5.07 | 4.84 | 5.89 |
| base_3 | 6.21 | 5.07 | 4.84 | 5.90 |
| base_4 | 6.19 | 5.06 | 4.82 | 5.93 |
| base_5 | 6.20 | 5.07 | 4.83 | 5.92 |
| mean ± sd | 6.20 ± 0.01 | 5.07 ± 0.01 | 4.83 ± 0.01 | 5.91 ± 0.02 |

| native turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 2.47 | 2.03 | 1.95 | 5.35 |
| base_2 | 2.47 | 2.04 | 1.95 | 5.32 |
| base_3 | 2.47 | 2.04 | 1.95 | 5.33 |
| base_4 | 2.47 | 2.03 | 1.95 | 5.36 |
| base_5 | 2.47 | 2.04 | 1.95 | 5.34 |
| mean ± sd | 2.47 ± 0.00 | 2.04 ± 0.00 | 1.95 ± 0.00 | 5.34 ± 0.02 |

| chain steps/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 7.42 | 7.09 | 6.67 | 0.00 |
| base_2 | 7.44 | 7.11 | 6.69 | 0.00 |
| base_3 | 7.44 | 7.11 | 6.69 | 0.00 |
| base_4 | 7.42 | 7.09 | 6.67 | 0.00 |
| base_5 | 7.43 | 7.10 | 6.68 | 0.00 |
| mean ± sd | 7.43 ± 0.01 | 7.10 ± 0.01 | 6.68 ± 0.01 | 0.00 ± 0.00 |

| interpreted packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 3.72 | 3.03 | 2.88 | 0.32 |
| base_2 | 3.74 | 3.04 | 2.88 | 0.31 |
| base_3 | 3.73 | 3.04 | 2.88 | 0.31 |
| base_4 | 3.73 | 3.03 | 2.88 | 0.32 |
| base_5 | 3.73 | 3.03 | 2.88 | 0.32 |
| mean ± sd | 3.73 ± 0.01 | 3.03 ± 0.00 | 2.88 ± 0.00 | 0.32 ± 0.00 |

| packets per running turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 8.68 | 9.89 | 9.68 | 1.29 |
| base_2 | 8.67 | 9.88 | 9.68 | 1.29 |
| base_3 | 8.67 | 9.88 | 9.68 | 1.29 |
| base_4 | 8.67 | 9.89 | 9.68 | 1.29 |
| base_5 | 8.67 | 9.88 | 9.68 | 1.29 |
| mean ± sd | 8.67 ± 0.01 | 9.88 ± 0.00 | 9.68 ± 0.00 | 1.29 ± 0.00 |

| blocks per native turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 4.01 | 4.49 | 4.42 | 1.00 |
| base_2 | 4.01 | 4.49 | 4.42 | 1.00 |
| base_3 | 4.00 | 4.49 | 4.42 | 1.00 |
| base_4 | 4.01 | 4.49 | 4.42 | 1.00 |
| base_5 | 4.01 | 4.49 | 4.42 | 1.00 |
| mean ± sd | 4.01 ± 0.00 | 4.49 ± 0.00 | 4.42 ± 0.00 | 1.00 ± 0.00 |

| polling turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| base_1 | 0.34 | 0.33 | 0.33 | 0.01 |
| base_2 | 0.34 | 0.33 | 0.33 | 0.01 |
| base_3 | 0.34 | 0.33 | 0.33 | 0.01 |
| base_4 | 0.34 | 0.33 | 0.33 | 0.01 |
| base_5 | 0.34 | 0.33 | 0.33 | 0.01 |
| mean ± sd | 0.34 ± 0.00 | 0.33 ± 0.00 | 0.33 ± 0.00 | 0.01 ± 0.00 |

| turn ended by (% of all turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| interpreted packet | 60.2 ± 0.0 | 47.6 ± 0.1 | 42.6 ± 0.1 | 5.3 ± 0.0 |
| BIOS or PE call | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 4.4 ± 0.0 |
| budget | 14.3 ± 0.0 | 13.5 ± 0.0 | 11.8 ± 0.0 | 0.0 ± 0.0 |
| no block | 25.5 ± 0.0 | 18.5 ± 0.0 | 17.1 ± 0.0 | 0.0 ± 0.0 |
| mid-block bail | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| interrupt | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| other native (chain off, branch pending, halted, icache, breakpoint, local memory dirty) | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 90.3 ± 0.0 |
| IL block | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| MPE not running | 0.0 ± 0.0 | 20.5 ± 0.2 | 28.5 ± 0.1 | 0.0 ± 0.0 |

| state | mean % of samples | sd | min | max |
|---|---|---|---|---|
| generated code | 48.14 | 0.47 | 47.61 | 48.55 |
| interpreter | 7.18 | 0.20 | 6.95 | 7.50 |
| asleep | 11.03 | 0.50 | 10.58 | 11.58 |
| dispatcher proper | 15.38 | 0.66 | 14.48 | 16.06 |
| BIOS and PE handlers | 0.01 | 0.00 | 0.01 | 0.01 |
| compiling | 0.03 | 0.00 | 0.02 | 0.03 |
| chain loop between blocks | 4.96 | 0.02 | 4.94 | 4.99 |
| round level | 1.84 | 0.10 | 1.75 | 2.02 |
| cache flush loops | 0.27 | 0.02 | 0.25 | 0.30 |
| overlay hashing | 0.03 | 0.00 | 0.02 | 0.03 |
| packet decode | 0.10 | 0.01 | 0.09 | 0.11 |
| media tick and loop between turns | 6.36 | 0.67 | 5.82 | 7.31 |
| NUANCE_PERF counting | 4.67 | 0.96 | 3.92 | 5.96 |

| interpreted packets by reason (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| delay slot of an interpreted branch | 1.280 | 1.329 | 1.233 | 0.016 |
| mid-block bail | 0.000 | 0.000 | 0.000 | 0.006 |
| newly decoded | 0.005 | 0.018 | 0.009 | 0.000 |
| marked compiled but no block | 0.766 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.002 | 0.009 | 0.005 | 0.000 |
| BIOS table or breakpoint | 0.000 | 0.000 | 0.000 | 0.258 |
| branch packet | 1.239 | 1.275 | 1.228 | 0.022 |
| control register | 0.438 | 0.400 | 0.406 | 0.013 |
| other | 0.000 | 0.000 | 0.000 | 0.000 |

| native turns ended for lack of a block, next packet (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| at a BIOS or PE address | 0.000 | 0.000 | 0.000 | 0.000 |
| never decoded | 0.001 | 0.003 | 0.001 | 0.000 |
| never compilable | 1.180 | 1.170 | 1.150 | 0.000 |
| marked compiled but no block | 0.401 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.001 | 0.003 | 0.002 | 0.000 |
| block not native | 0.000 | 0.000 | 0.000 | 0.000 |

| turns by packets run (% of turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| 0 | 0.0 | 20.5 | 28.5 | 0.0 |
| 1 | 63.7 | 48.0 | 43.3 | 85.1 |
| 2-3 | 3.0 | 1.3 | 1.6 | 14.9 |
| 4-7 | 6.1 | 4.3 | 4.1 | 0.0 |
| 8-15 | 5.0 | 5.4 | 4.2 | 0.0 |
| 16-31 | 7.9 | 7.0 | 6.4 | 0.0 |
| 32+ | 14.3 | 13.5 | 11.8 | 0.0 |

| flushes per second | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| overlay region | 0.1 | 0.1 | 0.1 | 0.0 |
| main and system bus ranges | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, past threshold | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, no room | 0.0 | 0.0 | 0.0 | 0.0 |
| FlushRegion calls | 0.1 | 0.1 | 0.1 | 0.0 |
| addresses walked | 655.3 | 327.7 | 327.7 | 0.0 |

### C. Saturated, WA (game frames 770-1550), runs sat_1 to sat_7

| run | windows (s) | game frames | fps | rounds/s (M) | asleep % |
|---|---|---|---|---|---|
| sat_1 | 2 (10.0) | 806-1355 | 54.9 | 8.49 | 0.0 |
| sat_2 | 2 (10.0) | 806-1354 | 54.8 | 8.47 | 0.0 |
| sat_3 | 2 (10.0) | 807-1355 | 54.8 | 8.53 | 0.0 |
| sat_4 | 2 (10.0) | 790-1333 | 54.3 | 8.11 | 0.0 |
| sat_5 | 2 (10.0) | 793-1341 | 54.8 | 8.25 | 0.0 |
| sat_6 | 1 (5.0) | 1042-1332 | 58.0 | 8.33 | 0.0 |
| sat_7 | 2 (10.0) | 808-1357 | 54.9 | 8.51 | 0.0 |
| mean ± sd (min–max) | | | 55.2 ± 1.2 (54.3–58.0) | 8.38 ± 0.16 (8.11–8.53) | 0.0 ± 0.0 (0.0–0.0) |

| packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 59.1 | 57.5 | 52.6 | 10.6 |
| sat_2 | 58.8 | 57.3 | 52.5 | 10.5 |
| sat_3 | 58.9 | 57.4 | 52.5 | 10.6 |
| sat_4 | 56.8 | 56.0 | 50.8 | 10.1 |
| sat_5 | 57.9 | 56.8 | 51.7 | 10.3 |
| sat_6 | 61.1 | 60.6 | 54.8 | 10.4 |
| sat_7 | 59.2 | 57.6 | 52.8 | 10.6 |
| mean ± sd | 58.8 ± 1.3 | 57.6 ± 1.4 | 52.5 ± 1.2 | 10.4 ± 0.2 |

| turns/s (M), all | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 8.49 | 8.49 | 8.49 | 8.49 |
| sat_2 | 8.47 | 8.47 | 8.47 | 8.47 |
| sat_3 | 8.53 | 8.53 | 8.53 | 8.53 |
| sat_4 | 8.11 | 8.11 | 8.11 | 8.11 |
| sat_5 | 8.25 | 8.25 | 8.25 | 8.25 |
| sat_6 | 8.33 | 8.33 | 8.33 | 8.33 |
| sat_7 | 8.51 | 8.51 | 8.51 | 8.51 |
| mean ± sd | 8.38 ± 0.16 | 8.38 ± 0.16 | 8.38 ± 0.16 | 8.38 ± 0.16 |

| running turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 8.49 | 4.32 | 3.51 | 8.49 |
| sat_2 | 8.47 | 4.31 | 3.50 | 8.47 |
| sat_3 | 8.53 | 4.31 | 3.50 | 8.53 |
| sat_4 | 8.11 | 4.19 | 3.34 | 8.11 |
| sat_5 | 8.25 | 4.26 | 3.42 | 8.25 |
| sat_6 | 8.33 | 4.48 | 3.56 | 8.33 |
| sat_7 | 8.51 | 4.33 | 3.52 | 8.51 |
| mean ± sd | 8.38 ± 0.16 | 4.31 ± 0.09 | 3.48 ± 0.07 | 8.38 ± 0.16 |

| native turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 3.77 | 1.96 | 1.74 | 7.49 |
| sat_2 | 3.75 | 1.95 | 1.74 | 7.47 |
| sat_3 | 3.71 | 1.95 | 1.74 | 7.53 |
| sat_4 | 3.59 | 1.90 | 1.67 | 7.16 |
| sat_5 | 3.69 | 1.93 | 1.70 | 7.28 |
| sat_6 | 3.66 | 2.06 | 1.80 | 7.37 |
| sat_7 | 3.77 | 1.96 | 1.75 | 7.51 |
| mean ± sd | 3.71 ± 0.07 | 1.96 ± 0.05 | 1.73 ± 0.04 | 7.40 ± 0.14 |

| chain steps/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 5.21 | 6.50 | 5.45 | 0.00 |
| sat_2 | 5.19 | 6.48 | 5.43 | 0.00 |
| sat_3 | 5.22 | 6.49 | 5.44 | 0.00 |
| sat_4 | 5.03 | 6.34 | 5.27 | 0.00 |
| sat_5 | 5.13 | 6.43 | 5.37 | 0.00 |
| sat_6 | 5.25 | 6.68 | 5.42 | 0.00 |
| sat_7 | 5.23 | 6.51 | 5.46 | 0.00 |
| mean ± sd | 5.18 ± 0.08 | 6.49 ± 0.10 | 5.41 ± 0.07 | 0.00 ± 0.00 |

| interpreted packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 4.71 | 2.37 | 1.77 | 0.75 |
| sat_2 | 4.72 | 2.36 | 1.76 | 0.75 |
| sat_3 | 4.82 | 2.36 | 1.76 | 0.75 |
| sat_4 | 4.52 | 2.29 | 1.67 | 0.72 |
| sat_5 | 4.56 | 2.33 | 1.71 | 0.74 |
| sat_6 | 4.67 | 2.42 | 1.76 | 0.71 |
| sat_7 | 4.74 | 2.37 | 1.77 | 0.76 |
| mean ± sd | 4.68 ± 0.10 | 2.36 ± 0.04 | 1.74 ± 0.04 | 0.74 ± 0.02 |

| packets per running turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 6.96 | 13.31 | 15.01 | 1.25 |
| sat_2 | 6.95 | 13.31 | 15.01 | 1.25 |
| sat_3 | 6.91 | 13.31 | 15.00 | 1.25 |
| sat_4 | 7.00 | 13.36 | 15.22 | 1.25 |
| sat_5 | 7.02 | 13.34 | 15.14 | 1.24 |
| sat_6 | 7.33 | 13.52 | 15.39 | 1.25 |
| sat_7 | 6.96 | 13.30 | 14.98 | 1.25 |
| mean ± sd | 7.02 ± 0.14 | 13.35 ± 0.08 | 15.11 ± 0.15 | 1.25 ± 0.00 |

| blocks per native turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 2.38 | 4.32 | 4.13 | 1.00 |
| sat_2 | 2.39 | 4.33 | 4.13 | 1.00 |
| sat_3 | 2.41 | 4.32 | 4.13 | 1.00 |
| sat_4 | 2.40 | 4.34 | 4.16 | 1.00 |
| sat_5 | 2.39 | 4.34 | 4.15 | 1.00 |
| sat_6 | 2.43 | 4.24 | 4.01 | 1.00 |
| sat_7 | 2.39 | 4.32 | 4.12 | 1.00 |
| mean ± sd | 2.40 ± 0.02 | 4.32 ± 0.03 | 4.12 ± 0.05 | 1.00 ± 0.00 |

| polling turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 0.34 | 0.32 | 0.35 | 0.07 |
| sat_2 | 0.34 | 0.32 | 0.34 | 0.07 |
| sat_3 | 0.34 | 0.32 | 0.35 | 0.07 |
| sat_4 | 0.32 | 0.31 | 0.33 | 0.06 |
| sat_5 | 0.33 | 0.32 | 0.34 | 0.07 |
| sat_6 | 0.40 | 0.36 | 0.40 | 0.06 |
| sat_7 | 0.34 | 0.32 | 0.35 | 0.07 |
| mean ± sd | 0.34 ± 0.02 | 0.32 ± 0.02 | 0.35 ± 0.02 | 0.07 ± 0.00 |

| turn ended by (% of all turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| interpreted packet | 55.8 ± 0.4 | 28.1 ± 0.5 | 20.8 ± 0.2 | 8.8 ± 0.1 |
| BIOS or PE call | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 2.9 ± 0.0 |
| budget | 11.8 ± 0.5 | 13.1 ± 0.5 | 12.2 ± 0.5 | 0.0 ± 0.0 |
| no block | 32.4 ± 0.7 | 10.2 ± 0.1 | 8.5 ± 0.1 | 0.0 ± 0.0 |
| mid-block bail | 0.1 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| interrupt | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| other native (chain off, branch pending, halted, icache, breakpoint, local memory dirty) | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 88.3 ± 0.1 |
| IL block | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| MPE not running | 0.0 ± 0.0 | 48.5 ± 1.1 | 58.5 ± 0.6 | 0.0 ± 0.0 |

| state | mean % of samples | sd | min | max |
|---|---|---|---|---|
| generated code | 57.17 | 0.37 | 56.90 | 57.98 |
| interpreter | 6.18 | 0.11 | 6.04 | 6.29 |
| asleep | 0.00 | 0.00 | 0.00 | 0.00 |
| dispatcher proper | 16.13 | 0.21 | 15.82 | 16.46 |
| BIOS and PE handlers | 0.01 | 0.00 | 0.01 | 0.02 |
| compiling | 0.14 | 0.02 | 0.11 | 0.19 |
| chain loop between blocks | 5.48 | 0.17 | 5.26 | 5.77 |
| round level | 1.92 | 0.08 | 1.81 | 2.01 |
| cache flush loops | 1.41 | 0.07 | 1.25 | 1.46 |
| overlay hashing | 0.13 | 0.02 | 0.08 | 0.15 |
| packet decode | 0.54 | 0.04 | 0.45 | 0.60 |
| media tick and loop between turns | 6.33 | 0.17 | 6.08 | 6.55 |
| NUANCE_PERF counting | 4.56 | 0.32 | 4.29 | 5.21 |

| interpreted packets by reason (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| delay slot of an interpreted branch | 0.866 | 1.183 | 0.852 | 0.124 |
| mid-block bail | 0.005 | 0.000 | 0.000 | 0.065 |
| newly decoded | 0.025 | 0.140 | 0.033 | 0.000 |
| marked compiled but no block | 2.953 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.011 | 0.028 | 0.020 | 0.004 |
| BIOS table or breakpoint | 0.000 | 0.000 | 0.000 | 0.234 |
| branch packet | 0.570 | 0.736 | 0.586 | 0.183 |
| control register | 0.246 | 0.269 | 0.252 | 0.131 |
| other | 0.000 | 0.001 | 0.000 | 0.000 |

| native turns ended for lack of a block, next packet (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| at a BIOS or PE address | 0.000 | 0.000 | 0.000 | 0.000 |
| never decoded | 0.005 | 0.022 | 0.006 | 0.000 |
| never compilable | 0.663 | 0.823 | 0.696 | 0.000 |
| marked compiled but no block | 2.040 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.006 | 0.011 | 0.007 | 0.000 |
| block not native | 0.000 | 0.000 | 0.000 | 0.000 |

| turns by packets run (% of turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| 0 | 0.0 | 48.5 | 58.5 | 0.0 |
| 1 | 68.6 | 29.0 | 21.8 | 85.9 |
| 2-3 | 6.4 | 0.1 | 0.4 | 14.1 |
| 4-7 | 6.6 | 0.4 | 0.2 | 0.0 |
| 8-15 | 2.4 | 4.0 | 2.3 | 0.0 |
| 16-31 | 4.3 | 4.7 | 4.5 | 0.0 |
| 32+ | 11.8 | 13.1 | 12.2 | 0.0 |

| flushes per second | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| overlay region | 1.4 | 1.6 | 1.4 | 0.0 |
| main and system bus ranges | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, past threshold | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, no room | 0.0 | 0.0 | 0.0 | 0.0 |
| FlushRegion calls | 1.4 | 1.6 | 1.4 | 0.0 |
| addresses walked | 11584.6 | 6611.6 | 5792.3 | 0.0 |

### D. Saturated, WB (game frames 1770-2320), runs sat_1 to sat_7

| run | windows (s) | game frames | fps | rounds/s (M) | asleep % |
|---|---|---|---|---|---|
| sat_1 | 3 (15.0) | 1868-2309 | 29.4 | 6.94 | 0.0 |
| sat_2 | 3 (15.0) | 1862-2296 | 28.9 | 6.82 | 0.0 |
| sat_3 | 3 (15.0) | 1868-2313 | 29.7 | 6.99 | 0.0 |
| sat_4 | 3 (15.0) | 1842-2272 | 28.7 | 6.76 | 0.0 |
| sat_5 | 3 (15.0) | 1855-2292 | 29.1 | 6.87 | 0.0 |
| sat_6 | 3 (15.0) | 1850-2294 | 29.6 | 7.00 | 0.0 |
| sat_7 | 3 (15.0) | 1871-2310 | 29.3 | 6.90 | 0.0 |
| mean ± sd (min–max) | | | 29.2 ± 0.4 (28.7–29.7) | 6.90 ± 0.09 (6.76–7.00) | 0.0 ± 0.0 (0.0–0.0) |

| packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 61.5 | 57.1 | 53.2 | 8.9 |
| sat_2 | 60.4 | 56.0 | 52.2 | 8.8 |
| sat_3 | 62.0 | 57.5 | 53.6 | 9.0 |
| sat_4 | 59.7 | 55.6 | 51.4 | 8.7 |
| sat_5 | 60.8 | 56.3 | 52.4 | 8.8 |
| sat_6 | 61.9 | 57.4 | 53.3 | 9.0 |
| sat_7 | 61.2 | 56.8 | 53.0 | 8.9 |
| mean ± sd | 61.1 ± 0.8 | 56.7 ± 0.7 | 52.7 ± 0.8 | 8.9 ± 0.1 |

| turns/s (M), all | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 6.94 | 6.94 | 6.94 | 6.94 |
| sat_2 | 6.82 | 6.82 | 6.82 | 6.82 |
| sat_3 | 6.99 | 6.99 | 6.99 | 6.99 |
| sat_4 | 6.76 | 6.76 | 6.76 | 6.76 |
| sat_5 | 6.87 | 6.87 | 6.87 | 6.87 |
| sat_6 | 7.00 | 7.00 | 7.00 | 7.00 |
| sat_7 | 6.90 | 6.90 | 6.90 | 6.90 |
| mean ± sd | 6.90 ± 0.09 | 6.90 ± 0.09 | 6.90 ± 0.09 | 6.90 ± 0.09 |

| running turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 6.94 | 5.73 | 5.47 | 6.94 |
| sat_2 | 6.82 | 5.62 | 5.35 | 6.82 |
| sat_3 | 6.99 | 5.78 | 5.51 | 6.99 |
| sat_4 | 6.76 | 5.58 | 5.25 | 6.76 |
| sat_5 | 6.87 | 5.65 | 5.37 | 6.87 |
| sat_6 | 7.00 | 5.76 | 5.47 | 7.00 |
| sat_7 | 6.90 | 5.71 | 5.44 | 6.90 |
| mean ± sd | 6.90 ± 0.09 | 5.69 ± 0.08 | 5.41 ± 0.09 | 6.90 ± 0.09 |

| native turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 2.83 | 2.31 | 2.21 | 6.26 |
| sat_2 | 2.78 | 2.27 | 2.17 | 6.16 |
| sat_3 | 2.85 | 2.33 | 2.23 | 6.31 |
| sat_4 | 2.74 | 2.25 | 2.13 | 6.11 |
| sat_5 | 2.80 | 2.28 | 2.18 | 6.20 |
| sat_6 | 2.84 | 2.32 | 2.21 | 6.32 |
| sat_7 | 2.82 | 2.30 | 2.20 | 6.23 |
| mean ± sd | 2.81 ± 0.04 | 2.29 ± 0.03 | 2.19 ± 0.03 | 6.23 ± 0.08 |

| chain steps/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 8.48 | 8.05 | 7.57 | 0.00 |
| sat_2 | 8.32 | 7.90 | 7.41 | 0.00 |
| sat_3 | 8.54 | 8.11 | 7.63 | 0.00 |
| sat_4 | 8.21 | 7.83 | 7.28 | 0.00 |
| sat_5 | 8.37 | 7.94 | 7.44 | 0.00 |
| sat_6 | 8.53 | 8.09 | 7.57 | 0.00 |
| sat_7 | 8.44 | 8.01 | 7.53 | 0.00 |
| mean ± sd | 8.41 ± 0.12 | 7.99 ± 0.10 | 7.49 ± 0.12 | 0.00 ± 0.00 |

| interpreted packets/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 4.10 | 3.42 | 3.25 | 0.37 |
| sat_2 | 4.04 | 3.36 | 3.18 | 0.36 |
| sat_3 | 4.14 | 3.45 | 3.28 | 0.37 |
| sat_4 | 4.02 | 3.33 | 3.12 | 0.36 |
| sat_5 | 4.08 | 3.38 | 3.19 | 0.37 |
| sat_6 | 4.16 | 3.44 | 3.25 | 0.37 |
| sat_7 | 4.09 | 3.41 | 3.24 | 0.37 |
| mean ± sd | 4.09 ± 0.05 | 3.40 ± 0.05 | 3.22 ± 0.05 | 0.37 ± 0.00 |

| packets per running turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 8.86 | 9.95 | 9.74 | 1.29 |
| sat_2 | 8.86 | 9.96 | 9.75 | 1.29 |
| sat_3 | 8.86 | 9.95 | 9.73 | 1.29 |
| sat_4 | 8.83 | 9.96 | 9.77 | 1.29 |
| sat_5 | 8.85 | 9.96 | 9.76 | 1.29 |
| sat_6 | 8.84 | 9.95 | 9.75 | 1.29 |
| sat_7 | 8.86 | 9.95 | 9.74 | 1.29 |
| mean ± sd | 8.85 ± 0.01 | 9.96 ± 0.01 | 9.75 ± 0.01 | 1.29 ± 0.00 |

| blocks per native turn | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 3.99 | 4.49 | 4.42 | 1.00 |
| sat_2 | 3.99 | 4.49 | 4.42 | 1.00 |
| sat_3 | 4.00 | 4.49 | 4.42 | 1.00 |
| sat_4 | 4.00 | 4.48 | 4.42 | 1.00 |
| sat_5 | 3.99 | 4.48 | 4.42 | 1.00 |
| sat_6 | 4.00 | 4.48 | 4.42 | 1.00 |
| sat_7 | 4.00 | 4.49 | 4.42 | 1.00 |
| mean ± sd | 4.00 ± 0.00 | 4.48 ± 0.00 | 4.42 ± 0.00 | 1.00 ± 0.00 |

| polling turns/s (M) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| sat_1 | 0.39 | 0.37 | 0.38 | 0.01 |
| sat_2 | 0.38 | 0.36 | 0.37 | 0.01 |
| sat_3 | 0.39 | 0.37 | 0.38 | 0.01 |
| sat_4 | 0.37 | 0.36 | 0.36 | 0.01 |
| sat_5 | 0.38 | 0.37 | 0.37 | 0.01 |
| sat_6 | 0.39 | 0.37 | 0.38 | 0.01 |
| sat_7 | 0.39 | 0.37 | 0.37 | 0.01 |
| mean ± sd | 0.38 ± 0.01 | 0.37 ± 0.00 | 0.37 ± 0.01 | 0.01 ± 0.00 |

| turn ended by (% of all turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| interpreted packet | 59.3 ± 0.1 | 49.2 ± 0.1 | 46.6 ± 0.3 | 5.3 ± 0.0 |
| BIOS or PE call | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 4.4 ± 0.0 |
| budget | 14.7 ± 0.0 | 14.2 ± 0.0 | 13.1 ± 0.0 | 0.0 ± 0.0 |
| no block | 26.0 ± 0.1 | 19.1 ± 0.0 | 18.6 ± 0.1 | 0.0 ± 0.0 |
| mid-block bail | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| interrupt | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| other native (chain off, branch pending, halted, icache, breakpoint, local memory dirty) | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 90.3 ± 0.0 |
| IL block | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 | 0.0 ± 0.0 |
| MPE not running | 0.0 ± 0.0 | 17.5 ± 0.2 | 21.6 ± 0.4 | 0.0 ± 0.0 |

| state | mean % of samples | sd | min | max |
|---|---|---|---|---|
| generated code | 55.33 | 0.11 | 55.16 | 55.45 |
| interpreter | 8.08 | 0.13 | 7.81 | 8.19 |
| asleep | 0.00 | 0.00 | 0.00 | 0.00 |
| dispatcher proper | 17.84 | 0.16 | 17.56 | 18.04 |
| BIOS and PE handlers | 0.01 | 0.00 | 0.01 | 0.02 |
| compiling | 0.03 | 0.01 | 0.02 | 0.04 |
| chain loop between blocks | 5.49 | 0.15 | 5.23 | 5.68 |
| round level | 1.61 | 0.08 | 1.52 | 1.72 |
| cache flush loops | 0.30 | 0.04 | 0.27 | 0.38 |
| overlay hashing | 0.03 | 0.01 | 0.02 | 0.03 |
| packet decode | 0.12 | 0.01 | 0.10 | 0.14 |
| media tick and loop between turns | 6.61 | 0.16 | 6.36 | 6.83 |
| NUANCE_PERF counting | 4.56 | 0.18 | 4.38 | 4.91 |

| interpreted packets by reason (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| delay slot of an interpreted branch | 1.419 | 1.491 | 1.377 | 0.018 |
| mid-block bail | 0.001 | 0.000 | 0.000 | 0.007 |
| newly decoded | 0.006 | 0.021 | 0.010 | 0.000 |
| marked compiled but no block | 0.783 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.003 | 0.010 | 0.006 | 0.000 |
| BIOS table or breakpoint | 0.000 | 0.000 | 0.000 | 0.302 |
| branch packet | 1.387 | 1.429 | 1.373 | 0.025 |
| control register | 0.493 | 0.447 | 0.453 | 0.015 |
| other | 0.000 | 0.000 | 0.000 | 0.000 |

| native turns ended for lack of a block, next packet (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| at a BIOS or PE address | 0.000 | 0.000 | 0.000 | 0.000 |
| never decoded | 0.001 | 0.004 | 0.001 | 0.000 |
| never compilable | 1.325 | 1.307 | 1.282 | 0.000 |
| marked compiled but no block | 0.466 | 0.000 | 0.000 | 0.000 |
| below the threshold | 0.001 | 0.004 | 0.002 | 0.000 |
| block not native | 0.000 | 0.000 | 0.000 | 0.000 |

| turns by packets run (% of turns) | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| 0 | 0.0 | 17.5 | 21.6 | 0.0 |
| 1 | 63.0 | 49.7 | 47.5 | 85.1 |
| 2-3 | 3.0 | 1.3 | 1.7 | 14.9 |
| 4-7 | 6.2 | 4.4 | 4.5 | 0.0 |
| 8-15 | 5.1 | 5.6 | 4.6 | 0.0 |
| 16-31 | 8.0 | 7.2 | 7.0 | 0.0 |
| 32+ | 14.7 | 14.2 | 13.1 | 0.0 |

| flushes per second | MPE0 | MPE1 | MPE2 | MPE3 |
|---|---|---|---|---|
| overlay region | 0.0 | 0.0 | 0.0 | 0.0 |
| main and system bus ranges | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, past threshold | 0.0 | 0.0 | 0.0 | 0.0 |
| whole cache, no room | 0.0 | 0.0 | 0.0 | 0.0 |
| FlushRegion calls | 0.0 | 0.0 | 0.0 | 0.0 |
| addresses walked | 0.0 | 0.0 | 0.0 | 0.0 |
