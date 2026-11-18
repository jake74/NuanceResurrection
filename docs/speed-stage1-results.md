# Speed plan Stage 1: results

Stage 1 of [speed-plan.md](speed-plan.md) compiles packets that are still
interpreted, in the order Stage 0 ([speed-stage0-results.md](speed-stage0-results.md))
gave. Mac only so far.

## S1.2: a ",nop" branch in the first delay slot (2026-11-17)

### What was interpreted

Stage 0 found one loop behind about 3 million of the 10.7 million packets a
second interpreted in the busy stretch of the scripted game (game frames
1770-2320), run by MPE0, MPE1 and MPE2:

```
20300C4E  add r8, r9, r9   bra gt, $20300BF2        ; conditional, delayed
20300C52  sub r9, r24, r9  bra $20300C34, nop       ; non-delayed branch in delay slot 1
20300C5A  addm r6, r3, r6                           ; delay slot 2
```

The text of these packets comes from the perf log's sampled table, which
prints a packet's text when the report is written; the addresses are reliable,
the text may belong to another overlay. After the change the three addresses
are gone from the table of interpreted packets.

### The change

`FetchSuperBlock` (`src/SuperBlock.cpp`, Arm64 only) now compiles a
conditional delayed branch whose first delay slot holds a ",nop" branch (any
kind with `PACKETINFO_BRANCH_NOP`: bra, jmp or jsr, conditional or always),
provided the second slot holds nothing that would stop compilation. It uses
the guard already used for a ",nop" branch in the second slot
(`a64GuardedPacketPc`, `bCheckECUSkipCounter`): the inner branch is skipped
when the outer one was taken, and otherwise evaluated; taken, it leaves the
block at once (`Emit_ExitBlock` in the ",nop" emitters), so the second slot
does not run, as in the interpreter. Switched with the existing
`NUANCE_A64_SLOT_BRANCHES`.

Still interpreted: both slots holding branches, a delayed branch in a slot
(S1.3), an outer `bra` that is always taken, a second slot with an ECU
instruction, indirect memory access or a never-compiled packet when the first
holds a branch, and inner branches of other kinds (rts, rti).

The first slot's packet end carries `SUPERBLOCKINFO_SYNC`. Dead-code
elimination works backwards over the whole block and would drop a write that
the second slot overwrites unread, but on the path where the inner branch
leaves the block the second slot never runs, so the write is live. DCE is
disabled in `nuance.cfg` ("still buggy"), so the game never reaches this; with
DCE enabled in a test, the mark took the mismatches in the "outer not taken,
inner taken" class of the differential test from 444 to 3 (the remaining ones
are DCE's own, on any shape).

Known imprecision: when the inner branch is taken, the block's packet count
still includes the second slot, one more than ran. The count feeds the MPE
speed limit, so in the busy stretch MPE0 to MPE2 are held back by up to about
2% at the shipped limit. The early exit for indirect memory access already has
the same imprecision.

### Correctness

- The differential test generates the new shape (one conditional delayed
  branch in four gets a ",nop" branch in its first slot; switch off with
  `NUANCE_A64_DIFFTEST_SLOT1=0`) and reports it by class. Seed 1, 200,000
  trials: 11,383 runs of the shape; per class (outer not taken / taken, inner
  condition false / true) 2,852, 2,567, 2,489 and 2,139 compared, nearly all
  with the inner branch inside the native block; 0 mismatched.
- Totals, 200,000 trials, 0 mismatched on every seed: seeds 1 to 7 give
  180,888, 180,994, 180,823, 180,828, 180,813, 180,771 and 180,831 compared
  (against 180,563, 180,611 and 180,682 before for seeds 1 to 3); seed 9 gives
  180,988. With the new generation switched off, seed 1 gives 180,563, the
  baseline exactly.
- The harness now keeps stepping when the inner branch's target is the second
  slot itself (the interpreter's reference run treats that as ordinary flow and
  runs the packet); before, it stopped the native side one packet early and
  reported a false mismatch on seeds 3 and 4.
- Headless play, 560 s each, before and after: level one reached, the T3K
  state lines track each other, no hang, crash or error. Both cores showed
  occasional "emulation stalled for ~83 s" events in long runs; they are not
  from this change.

### Gain

Saturated (`NUANCE_MPE_HZ=0`), counters builds, six runs of each, alternating:

| | 770-1550 before | after | 1770-2320 before | after |
|---|---|---|---|---|
| game frames a second | 54.8 ± 0.6 | 54.1 ± 1.0 | 29.1 ± 0.3 (28.5-29.3) | **32.9 ± 0.9 (31.7-33.8)** |
| MPE0-2 interpreted packets a second | 8.64 M | 8.54 M | 10.63 M | **7.85 M** |
| generated code / interpreter / dispatcher proper, % | 57.3 / 6.2 / 16.3 | 57.1 / 6.2 / 16.3 | 55.3 / 8.2 / 18.1 | 62.9 / 6.5 / 14.0 |
| MPE0 packets per running turn | 7.0 | 7.0 | 8.8 | 11.4 |

The busy stretch is 13% faster, every run after above every run before; the
lighter one, where the loop does not run, is unchanged. Ordinary builds,
`NUANCE_PERF` off, game frames in 80 s including boot: 3455-3488 before,
3523-3574 after (+2.4% over the whole run).

On the Pad (Xiaomi Pad Mini), not measured; played by hand on 2026-11-17: "a little faster and a little smoother on busy scenes".

The gain is larger than the one Stage 0's arithmetic predicted (about 4.6%);
part of it is likely that turns now run longer before they end. Not measured
on the Pad yet, and Merlin Racing has not been run on it.

What remains interpreted in the busy stretch: delay slots of interpreted
branches and never-compiled branch packets (about 5.4 million a second
together) and control-register access (1.5 million).

## What was still interpreted after S1.2, found by decoding at sampling time (2026-11-18)

How it was found. A temporary diagnostic (not in the tree; the patch is kept outside it) made the interpreter, for one packet in 64 of each interpreted-packet reason, print the decoded packet text, its flags and the value of `ecuSkipCounter` at that moment, and added them up by MPE, address and text. This answers the caveat of the sampled table, which prints the text of a packet when the report is written: the text here is that of the packet as it was interpreted. The core was built from `044d2c2` with `-DNUANCE_PERF_COUNTERS=ON`; one run of 80 s, saturated (`NUANCE_MPE_HZ=0`), `tools/stage0/run.sh`; the rates are the mean of the three five-second windows inside game frames 1770-2320 (1788-1941, 1941-2079, 2079-2237). The sampled total is 7.17 million packets a second for MPE0 to MPE3, against 7.85 million for MPE0 to MPE2 in the counters of the S1.2 section (the sampling is by reason, so each class is a little short or long). The addresses are in the overlay at 20300000 and are the same on MPE0, MPE1 and MPE2 unless noted; the rates below are the three MPEs together.

| Rate | Share | Packets | Shape | Item |
|---|---|---|---|---|
| 1.88 M/s | 26% | `20300ACC` `bra gt`, `20300AD0` `bra le` (delayed), `20300AD4` `bra gt` (delayed), `20300AD8` `bra le, nop`, all to `20300AF6`, each with a `cmp` | Tempest 3000's chain of compares: a delayed conditional branch whose first slot is a delayed branch, whose second slot is another delayed branch, followed by a `,nop` branch and a plain packet. The sampled table showed the first and third packets as "never compiled" and the second and fourth as delay slots | S1.3 and "both slots hold branches" |
| 1.16 M/s | 16% | `2030096E` `st_s #$10400040, linpixctl`, `203009C0` and `20300A30` `st_s r30, mdmacptr`, `20300E1E` `st_s r2, mdmacptr`, `203003E4` `odmacptr`, `20300904` `mdmacptr` | Stores to control registers with side effects (DMA, linear pixel control), each a packet of its own | S1.4 (R1) |
| 0.93 M/s | 13% | `203009F8` `jsr (r1)` with `ld_s (r0), r0` and a nop in its slots | A delayed `jsr` with a register-indirect load in its first slot (R4c) | S1.1 |
| 0.59 M/s | 8% | MPE0 only: `20301DAC`, `20301DE2`, `20301FE8`, `20300000`, `2030000A` | The wait loop, marked compiled with no block for the overlay (R9) | tried and dropped before |
| 0.56 M/s | 8% | `20300A4E` `jsr $203002F4` with `st_v v0, (r4)` and `st_s r5, (r4)` in its slots | R4c as above, both slots | S1.1 |
| 0.56 M/s | 8% | `20300A60` `rts` with `st_s r4, mdmacptr` in its first slot | A delayed branch with a never-compiled packet (control register with side effects) in a slot. A new reason, not named in the plan's 4.1 as such: R4 with the blocker "never compile" | none planned |
| 0.29 M/s | 4% | `20300C6A` `bra c0ne` with `bra $20300CEA` (always, delayed) in the first slot | S1.3 with an always-taken inner branch | S1.3, not done |
| 0.23 M/s | 3% | MPE3 `80000060` `bra $F0000018, nop` | BIOS jump table (R2) | no |
| 0.16 M/s | 2% | `20300B16` `bra gt` with `st_s r0, (r1)` in both slots | S1.1 | S1.1 |
| 0.15 M/s | 2% | `203004B2` `bra c1ne` with `st_s r2, (r0)` in slot 1 | S1.1 | S1.1 |
| 0.13 M/s | 2% | `203009CE`, `203009D4`, `203009DA`, `203009D8`: `bra ge`, `bra ge`, `bra lt` | The same chain as the first row, shorter | S1.3 |
| 0.12 M/s | 2% | `2030020A` `bra c0ne` with a delayed `bra $1203001C4` in slot 1 | S1.3 with an always-taken inner branch | S1.3, not done |
| 0.40 M/s | 6% | the rest | a long tail, nothing above 40 thousand a second | |

Two readings of the table. Of the 2.6 million delay-slot packets a second, 0.77 million are the slots of the chain of compares (`20300AD4`, `20300AD8`), 0.62 million those of `203009F8`, 0.37 million those of `20300A4E`, 0.19 million `20300A66` (a slot of the `rts` with the control-register slot) and the rest many small ones: the "delay slot of an interpreted branch" row of the counters is mostly the three branch shapes above. And one in six interpreted packets is a control-register store whose only cost is that it is interpreted: compiling it (S1.4) would save the turn it takes of its own, not the work of the packet.

Order of expected value, and what was done: the chain of compares first (largest, and made of mechanisms that exist: the run-time check of `ecuSkipCounter`, the exit from the middle of a block), then register-indirect memory access in a delay slot (S1.1: 1.85 M/s counting `9F8`, `A4E`, `B16` and `4B2`). S1.4 was not taken: its saving is the turn, not the packet.

## S1.3 and both slots holding branches: a chain of branches in the delay slots (2026-11-18)

### What was interpreted

The first row of the inventory: a delayed conditional branch (`bra gt, A`) whose first delay slot holds a delayed conditional branch of its own, whose second slot holds another, followed by a `,nop` branch and a plain packet:

```
20300ACC  cmp r22, r0  bra gt, $20300AF6        ; P0, delayed
20300AD0  cmp r21, r4  bra le, $20300AF6        ; P1, delay slot 1 of P0, a delayed branch
20300AD4  cmp r20, r1  bra gt, $20300AF6        ; P2, delay slot 2 of P0 and slot 1 of P1, a delayed branch
20300AD8               bra le, $20300AF6, nop   ; P3, slot 2 of P1 and slot 1 of P2
20300ADC  st_v v1, $20100DA0                    ; P4, slot 2 of P2
```

R4a and R4b of the plan: `FetchSuperBlock` refused the head because a slot held a branch, the head was interpreted, and its slots were interpreted behind it. The other shapes of the same family (a `,nop` branch in both slots; a delayed branch in one slot and a plain packet or a `,nop` branch in the other) were interpreted for the same reason.

### How the interpreter treats it, and the change

In the interpreter a taken delayed branch sets `ecuSkipCounter` to 3 and every packet takes one off; a branch is evaluated only while the counter is 0; so the first branch taken decides where the run ends, wherever it is in the chain, and the branches in the two packets after it are not evaluated. The block therefore ends at a place that depends on which branch was taken: after P2 if the outer branch was, after P3 if P1 was, after P4 if P2 was, at once for a `,nop` branch. The native convention (the branch emitters set the counter to 1 and the dispatcher takes one off and goes to `pcfetchnext`) does not say which branch it was. `FetchSuperBlock` (`src/SuperBlock.cpp`, Arm64 only, switch `NUANCE_A64_NESTED_BRANCHES`, which needs `NUANCE_A64_SLOT_BRANCHES`) now compiles the shape when P0 is a delayed conditional branch, each of P1 and P2 is a plain packet, a `,nop` branch or a delayed conditional branch (not both plain), and, when a slot holds a delayed branch, the packets P3 and P4 that are its slots in turn are decoded and added as well: P3 plain or a `,nop` branch, P4 plain. None of them may hold anything else that stops a block (indirect memory access, a never-compiled packet, a breakpoint, or an instruction that can leave the block midway: see below).

- Every branch in P1 to P3 is emitted with the existing run-time check (`bCheckECUSkipCounter`, now for a set of packets: `SuperBlock::A64GuardFor`): it is skipped while the counter is not 0.
- Each delayed branch of the chain, P0 too, leaves `ecuSkipCounter` as a code (8 plus its position) after its packet instead of 1: `if counter == 1 then counter = code`, emitted at the end of its packet (`SuperBlock::A64EmitChainPacketEnd`). Only the first branch taken can leave a 1 there, since the others are skipped while the counter is not 0. The code is as good as 1 for the checks, since they test for non-zero.
- At the end of the packet that ends the delay slots of a delayed branch, if that is before the end of the block: `if counter == code then counter = 1; exit the block` (`Emit_ExitBlock`, as for a `,nop` branch). If it is the end of the block, a final `if counter != 0 then counter = 1` before the block's own return does it (`A64Chain::bNormalise`). A packet that is only a no-op is not in the block, so the check is attached to the last packet of the block that comes before the place it belongs; counts are by position.
- The block carries the packet count of the whole chain; an early exit takes off what did not run (new field `EmitterVariables::a64ExitNotRun`, used by `Emit_ExitBlock`, which subtracts from the low word of `MPE::cycleCounter`; that is exact because the counter is the sum of the counts of the blocks run so far in the turn, under 2^32, and at least this block's). This removes the known one-packet overcount of S1.2 as well (the first-slot `,nop` branch now passes 1), and it is exact for the new exits. The old early exits of other kinds (an indirect access that leaves the block) still count the whole block.
- Dead-code elimination: the end of every packet of the chain before the last carries `SUPERBLOCKINFO_SYNC`, so no write up to an exit is removed because a packet after the exit overwrites it. DCE is disabled in `nuance.cfg`; with it enabled in the difftest, taking the marks out raises the mismatches in the nested classes from 27 to 1,210 (seed 1), with them they are at the level of the rest (see below).

Why the codes and not a flag or a second counter: nothing else in the block needs to know and no register is held across packets; the cost is a load, a compare and a store at the end of each delayed branch's packet, and a load and compare at each end where a delayed branch's slots end. The x86 emitters are not touched: all the new code is inside `USE_A64_JIT` (`SuperBlock.cpp`, `SuperBlock.h`, `EmitMisc.h`, `EmitMisc.cpp`); with those lines taken out, `SuperBlock.cpp`, `SuperBlock.h`, `EmitMisc.cpp` and `EmitMisc.h` are the same as at `044d2c2` apart from one blank line (checked with a small script that removes the `USE_A64_JIT` conditional blocks from both versions and compares the rest).

What a chain still leaves interpreted: a delayed branch in a slot that is of the always-taken kind (`20300C6A`'s `bra $20300CEA`, `2030020C`), a P3 that is a delayed branch, a P4 that is not plain, any packet of the chain that holds something else that blocks a block, and a head with fewer than four packets of room left in the block (`packetCounter`; the lists have two packets to spare, which the five packets of the chain would overrun). Branches of the rts, rti, jmp and jsr kinds in the slots go through the same emitters as anywhere else; the difftest generates them.

A latent problem this exposed, fixed here: `ld_v (Sk)` (`Handler_LoadVectorLinear`) is not marked `PACKETINFO_MEMORY_INDIRECT` by the decoder (only `ld_s (Si)`, `st_s` and `st_v` are), so it is already compiled in the delay slots of a branch today, with the same bail to the interpreter as the others; and the packets that load an unknown control register bail unconditionally. A bail leaves `ecuSkipCounter` as the branch emitters left it, which is wrong in a slot (see S1.1). Packets that can bail are therefore kept out of the chain shapes (`A64PacketCanBail`); the other cases are the subject of the next section.

### Correctness

- The differential test generates the shape (one delayed conditional outer branch in three; slot 1 and slot 2 each plain, `,nop` or delayed, never both plain; P3 plain or `,nop`; P4 plain; switch off with `NUANCE_A64_DIFFTEST_NESTED=0`; own generator, so the other choices are those made without it), reports it by the branch the interpreter found taken first (outer, slot 1, slot 2, the packet after the slots, none) and by what the two slots hold, and compares the packets the block counted as run with the interpreter's. A branch whose target is inside the run is left out for these runs (160 in 15,176 on seed 1): the interpreter's run and the native one then stop at different places, as in the S1.2 section.
- Before the change, seed 1, 200,000 trials (`it1-before`, the harness with the old compiler): 15,176 runs generated; per branch taken first, outer / slot 1 / slot 2 / after the slots / none: 3,579, 1,416, 1,079, 187 and 1,605 compared, of which 1,331, 372, 429, 0 and 851 with the whole chain in the native block (the rest are runs where only a leading part was compiled), and 2,624, 1,202, 722, 211 and 827 not compiled natively. After: 5,928, 2,498, 1,742, 379 and 2,360 compared, 5,678, 2,393, 1,695, 360 and 2,303 with the whole chain inside the block; 275, 120, 59, 19 and 72 not compiled; 0 mismatched in every class. Per combination of slots (slot 1, slot 2): plain/`,nop` 1,547 compared, plain/delayed 1,270, `,nop`/plain 1,480, `,nop`/`,nop` 1,556, `,nop`/delayed 1,418, delayed/plain 1,391, delayed/`,nop` 1,441, delayed/delayed 2,804; 0 mismatched.
- Packet count: 12,429 runs with the whole chain in the block (6,654 with a first-slot `,nop` branch, from S1.2) compared against the interpreter's packets, 0 differing; before the change 1,691 of the 6,654 S1.2 runs differed (the overcount). Mutations of the compiler that the test catches: leaving out the code of the branch in slot 1 gives 668 mismatches on seed 1, all in the class "branch in slot 1 taken"; setting the number of packets that did not run to 0 makes 4,405 of 12,671 runs differ in their count.
- Totals, 200,000 trials, 0 mismatched on every seed; compared per seed, seeds 1 to 7 and 9: 180,421, 180,258, 180,444, 180,411, 180,555, 180,531, 180,327 and 180,733 with the new runs generated (the code of the first slot's `,nop` branch and everything else as in the S1.2 section: baseline 180,888, 180,994, 180,823, 180,828, 180,813, 180,771, 180,831 and 180,988 with the new generation off, and with it off, reproduced exactly with the new compiler code on); the old compiler with the new runs generated: 175,380, 175,369, 175,532, 175,485, 175,525, 175,648, 175,401 and 175,791. The compiler switch `NUANCE_A64_NESTED_BRANCHES=0` with the new runs generated gives the old compiler's numbers exactly (checked on all eight seeds against the first build of the old compiler with the new harness).
- Dead-code elimination switched on (a test of the code, not of the shipped setting), seed 1, measured on the first build of the change: 617 mismatches in all against 605 for the old compiler with the same runs; in the nested classes 27 of 13,030 compared (0.2%), at the level of the rest (the first-slot `,nop` classes have 15 of 6,765). Without the SYNC marks: 1,210 in the nested classes.
- Headless play, 300 s each, concurrently, `tools/stage0/run.sh`: level one reached at game frame about 960, the `t3k state` lines of both cores equal (`lane=2E672`, `1F0B9`, `36BF2`, `EABC2`, `3F6AF` at the same frames), game frames 9,167 before and 9,188 after at the shipped limit, no crash or hang. Each core logged about fifteen "emulation stalled" notes of 23 to 51 ms (the two runs shared the machine).
- Commands: `cmake -S <repo> -B <dir> -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRETRO=ON` then `make -C <dir> -j10 nuance_libretro a64_difftest_host`; `cd <dir> && NUANCE_ARM64_JIT=1 NUANCE_A64_DIFFTEST=200000,<seed> ./a64_difftest_host ./nuance_libretro.dylib <repo>/NUON.CD`.

### Gain

Saturated (`NUANCE_MPE_HZ=0`), counters builds, alternating, 80 s each; the machine slowed from the sixth run of a batch on (the perf host kept 4,801 calls in 80 s for the first runs and 2,800 to 3,400 later, for both cores alike), and the runs that fell behind are left out as in the Stage 0 method, so there are five runs before and four after, not six:

| | 790-1550 before | after | 1800-2320 before | after |
|---|---|---|---|---|
| game frames a second | 54.3 ± 0.2 (54.1-54.4) | 53.7 ± 0.9 (52.6-54.8) | 34.0 ± 0.2 (33.7-34.2) | 34.8 ± 0.8 (33.8-35.7) |
| MPE0-2 interpreted packets a second | 8.94 M | 8.72 M | 8.00 M | 6.03 M |
| generated code / interpreter / dispatcher proper, % | 57.1 / 6.3 / 16.5 | 57.5 / 6.0 / 16.2 | 63.6 / 6.3 / 13.8 | 69.0 / 4.6 / 10.8 |
| MPE0 packets per running turn | 6.91 | 7.03 | 11.51 | 13.39 |

Plain builds (`NOPERF=1`), saturated, game frames in 80 s including the boot, the machine in its slow state (the perf host kept 2,850 of 4,801 calls): 2,159 and 2,268 before, 2,325 and 2,325 after (two pairs, alternating; +3.8% on the means; the clean-state pair of the batch had lost its partner to a stall of the machine). The earlier figure of this document for S1.2's plain runs, 3,523 to 3,574, was taken with the machine in its fast state and is not comparable with these.

Reading: correct, and the busy stretch interprets 1.97 million packets a second fewer; the frame rate there is up 2.4% (0.8 frames a second, about the spread of the after runs; every after run is within or above the before runs), and unchanged in the lighter stretch where the chain does not run. The gain is small next to the 13% of S1.2 for about the same number of packets; the time split moves five points from the interpreter and dispatcher to generated code, and MPE0 to MPE2 run 7% more packets a second, so part of what is saved is spent in the MPEs' waiting loops, which count as work (a reading, not measured). The decision is the reviewer's; I would keep it: no run is slower and it is the larger part of what S1.1 then reaches.

## S1.1: register-indirect memory access in a delay slot (2026-11-18)

### What was interpreted

`ld_s (Si), Sk`, `st_s Sj, (Si)` and `st_v Vj, (Sk)` are marked `PACKETINFO_MEMORY_INDIRECT` by the decoder, and a delayed branch with one in either delay slot was not compiled (R4c): the branch was interpreted as a packet of its own, its slots behind it. In the busy stretch: `203009F8` `jsr (r1)` with `ld_s (r0), r0` in slot 1, `20300A4E` `jsr` with `st_v v0, (r4)` and `st_s r5, (r4)` in its slots, `20300B16` and `203004B2`: 1.85 million packets a second (the inventory above).

### Why they were kept out, and the change

The emitters of these three handlers (and of loads of control registers that the emitter has no code for) leave the block when the address turns out to be in the control or tag window, where an access has side effects: `interpretNextPacket` is set, `pcexec` is the packet, and the interpreter runs it (`src/EmitMEM.cpp`, the bail). Nothing of the packet has run when it leaves (the decoder is asked for memory instructions first). Inside the delay slots of a branch this left the branch in the wrong state: the branch emitters set `ecuSkipCounter` to 1 for the dispatcher to resolve at the end of the block, and a block that leaves in a slot with it at 1 has the dispatcher go to `pcfetchnext` at once (or the chain loop do so), so the slot packet that bailed and the one after it were never run. The interpreter has the counter at 2 as it starts slot 1 and at 1 as it starts slot 2, and the dispatcher takes one off at the end of the turn of the block as well, so a bail from slot 1 must leave 3 and one from slot 2 must leave 2, for a branch that was taken (0 for one that was not).

`Emit_ExitBlock` now does that for the instructions of a slot packet (`EmitterVariables::a64ExitSlotBump`, set by `SuperBlock::EmitCodeBlock` from `SuperBlock::a64SlotPc`): the counter, which is 0 or 1 there, is multiplied by 3 or 2 (`mov; lea; mov`, no flags and no labels, since the emitters that call `Emit_ExitBlock` have labels of their own pending), and the packet count is reduced by the packets that did not run (2 or 1). `FetchSuperBlock` compiles a delayed branch whose slots hold register-indirect access and nothing else that blocks (no ECU instruction, nothing never to be compiled, no breakpoint), switch `NUANCE_A64_SLOT_MEMORY` (default 1). The writes of the packets before a slot that can leave are kept by `SUPERBLOCKINFO_SYNC` on the end of the branch packet and of the first slot, for dead-code elimination. The bump applies to every slot packet of every delayed branch block, not only to the newly admitted ones: it also corrects the existing case of `ld_v (Sk)` and of unknown control-register loads, which the decoder does not mark as indirect and which were already compiled in slots with the wrong counter on a bail (not seen in the game: the access has to be to a control register in a slot of a taken branch). For a `,nop` branch's packet that is guarded (the S1.2 shapes) the exit that resolves the branch and a bail in the same packet have different counts; `EmitCodeBlock` gives the last instruction of a guarded packet (the branch) the first and the others the second.

What stays interpreted: an indirect access in a slot of a block that is also one of the chain shapes (such a packet is kept out of those, see above), a never-compiled packet in a slot (the `rts` at `20300A60`, whose first slot is a store to `mdmacptr`), and the other shapes of S1.3.

### Correctness

- The differential test has a new mode (`NUANCE_A64_DIFFTEST_SLOTMEM=0` leaves it out; own generator): for one delayed branch in three, one slot is made a scalar `ld_s` or `st_s` through a register, and for half of those the register holds the address of a control register that is plain state (not cc), so that the block leaves for the interpreter in the middle of the branch's slots and the test compares the whole run against the interpreter's. It is not done when an earlier packet of the run writes the register, which would send the access anywhere. Vector accesses are not given control addresses (those registers have side effects). `NUANCE_A64_DIFFTEST_TRACE_SM=1` prints each such run.
- Before the change (the compiler of S1.3 with the new runs), seed 1, 200,000 trials, per slot and register: slot 1 with a random address 2,851 generated, 390 compared, 457 not compiled natively; slot 1 with a control register address 2,751 generated, 1,219 compared, 1,338 not compiled; slot 2 random 2,984, 394, 457; slot 2 control 2,560, 1,107, 1,235. After: 748 compared and 99 not compiled; 2,219 compared (1,950 of them left the native block for the interpreter) and 338 not compiled; 851 and 0; 2,342 (all 2,342 left the block) and 0; 0 mismatched in all four. What is not compiled in slot 1 is a head with something else that blocks in the other slot.
- A mutation the test catches: without the bump 1,311 of the 2,219 and 1,348 of the 2,342 runs that leave the block mismatch (2,659 in all), with it none.
- Totals, 200,000 trials, 0 mismatched on every seed, compared per seed (seeds 1 to 7 and 9): 177,313, 177,347, 177,555, 177,308, 177,253, 177,324, 177,253 and 177,494 with the new runs; the compiler of S1.3 with the new runs: 174,073, 174,102, 174,188, 173,897, 173,919, 174,014, 174,018 and 174,082. With the new runs left out (`NUANCE_A64_DIFFTEST_SLOTMEM=0`): 180,945, 180,747, 180,929, 180,876, 181,009, 181,034, 180,860 and 181,229 (the slots hold indirect access by chance in some runs, which now compile). With the compiler switched off (`NUANCE_A64_SLOT_MEMORY=0`) and the new runs left out: 180,421, 180,258, 180,444, 180,411, 180,555, 180,531, 180,327 and 180,733, which is the S1.3 total exactly.
- Dead-code elimination on, seed 1: 603 mismatches in all against 602 before; in the new classes 1 of 4,561 compared with the control-register address and 2 of 1,599 with the register left at random.
- The packet count at a bail is not checked by the difftest.
- Headless play, 300 s each, concurrently, S1.3 against S1.1 (host busy with others: 22 calls a second instead of 60, so the game had only reached frame 3,600): level one reached at frame about 780, the `t3k state` lines equal at the same frames, no crash.
- The x86 text: the same check as for S1.3, no difference outside `USE_A64_JIT` in the four files.

### Gain

Saturated, counters builds, alternating, 80 s each; five runs each in the busy stretch, as above (the host was loaded by other work, and the perf host kept 2,800 of 4,801 calls in all but a few runs of both; the frame rate of the stretch is measured inside the emulation by game frames and is what the tables give):

| | 790-1550 S1.3 | S1.1 | 1800-2320 S1.3 | S1.1 |
|---|---|---|---|---|
| game frames a second | 55.1-56.6 (one run 43.2 with a stall) | 60.5-62.0 (61.6 ± 0.6) | 33.9 ± 1.8 (32.6-36.8) | **39.1 ± 0.5 (38.2-39.4)** |
| MPE0-2 interpreted packets a second | 8.51 M | 7.56 M | 6.12 M | 4.00 M |
| generated code / interpreter / dispatcher proper, % | 57.1 / 6.1 / 15.8 | 61.4 / 5.2 / 14.3 | 68.3 / 4.6 / 10.6 | 74.1 / 3.7 / 8.0 |
| MPE0 packets per running turn | 7.18 | 8.06 | 13.38 | 15.97 |

Plain builds (`NOPERF=1`), saturated, game frames in 80 s including the boot, alternating: in the machine's fast state (4,801 calls kept) 3,698 and 3,607 before and 3,802 after; in its slow state (about 2,850 calls kept) 2,226, 2,331, 2,214 and 2,275 before and 2,378, 2,271, 2,276 and 2,467 after (+3.8% on the means). One after run lost 4,000 calls to a stall of the machine (259 game frames) and is left out. The whole-run gain is smaller than that of the two stretches, because 80 s of the script also holds the boot and the scenes after frame 2,320, which these items hardly touch (a reading, not measured).

Reading: a gain of 15% in 1800-2320 (every after run above every before run but the one best run, 36.8) and 11 to 14% in the opening stretch (the best runs, leaving out the one with a stall); 2.1 million fewer packets interpreted a second in the busy stretch. Not measured on the Pad.

## What remains

After both items the busy stretch of the scripted game still interprets about 4.0 million packets a second on MPE0 to MPE2 (counters, 1800-2320), by the counters' reasons: control-register access 1.8 M/s (S1.4, which saves a turn each and not the packet), MPE0's wait loop 0.84 M/s (R9), delay slots of interpreted branches 0.69 M/s (mostly the `rts` at `20300A60` with a control-register store in its first slot, and the two always-taken branches in a slot, `20300C6A` and `2030020A`), branch packets never compiled 0.59 M/s (the heads of the same), and a tail. S1.3's remaining cases and the packet with a never-compiled slot are the next compile-side items; S1.4 is the largest single one left and has the smallest saving per packet. The chain code's counter codes and the bail bump both rely on the block ending in the branch's slots, which is why they are kept apart (a bail packet is not allowed in a chain); a chain with an indirect access would need the bump to know the codes.

## Review of S1.3 and S1.1 (2026-11-18)

**A crash in the differential test, not in the compiler.** Seed 11 crashed
the test (SIGSEGV in `MPE::WriteControlRegister`). The cause: the test fills
memory with random words, and a generated run loaded one, `204E71FC`, and
stored through it. `Execute_StoreScalarLinear` (`src/ExecuteMEM.cpp`) sends an
address in `[MPE_ITAGS_BASE, MPE_RESV_BASE)` that is not in the tag window to
`WriteControlRegister(address - MPE_CTRL_BASE)`; below the control base that
offset wraps, and the register file's default case writes about a gigabyte
outside the process (`src/MPEControlRegisters.cpp`, the `default:` of
`WriteControlRegister`; `ReadControlRegister` has the same). It happens in the
interpreter's reference run, before any generated code, and with the S1.2-era
compiler as well: an emulator robustness gap that only random code reaches,
left as it is. The test now rewrites any random word in its memory that falls
in that address range before a run. Seeds 1 to 12, both items on: 177,322,
177,352, 177,557, 177,315, 177,256, 177,329, 177,256, 177,324, 177,499,
177,422, 177,169 and 177,472 compared, 0 mismatched on every seed. (The
per-seed totals quoted in the sections above predate this change and may
differ from a run today by a few runs; with every new part switched off,
seed 1 gave S1.2's 180,888 exactly before the change and gives 180,892 after
it: the four runs the rewritten memory now lets finish.)

**Timing on the Mac, ordinary builds, `NUANCE_PERF` off, no limit.** The Mac
ran most measurement runs of this day at reduced speed: the headless host made
about 2,800 of its 4,801 calls in 80 seconds, with no heat warning, no other
busy process and Spotlight idle. Not explained. Within a round the three cores
ran one after another under the same conditions, so they are compared round by
round (game frames in 80 s, boot included):

| round | S1.2 | S1.2 + S1.3 | + S1.1 | S1.3 against S1.2 | both against S1.2 |
|---|---|---|---|---|---|
| 2 | 2,265 | 2,332 | 2,499 | +3.0% | +10.3% |
| 3 | 2,286 | 2,319 | 2,501 | +1.4% | +9.4% |
| 5 | 3,626 | 3,713 | (disturbed) | +2.4% | |
| 6 | 2,252 | 2,284 | 2,437 | +1.4% | +8.2% |

S1.3 is small but ahead in every comparable round; both items are kept.

On the Pad: not measured. Played by hand after S1.3 and S1.1 (2026-11-18), with the frame rate counter: 28-30 in ordinary play, around 20 in heavy scenes, about 12 at worst in a very busy scene. Whether that worst case is a scene not met before or a regression is not known: the earlier notes give 18-26 for the busiest scenes, without saying which.

## A busy scene on demand, on the Pad (2026-11-18)

Jake pointed out what makes the game busy: the superzapper (B) and later
levels, chosen on the level select with right. A scripted game on the Pad now
reaches frame rates like his own play, with the app's frame counter in
screenshots (`adb exec-out screencap`):

```
script: 60-63:start;120-123:start;250-253:a;330-333:a;450-453:a;
        then right pressed 14 times, 6 frames each, every 20 frames from 470;
        780-783:a (start level 7, "V SIGN": the level select stopped there);
        880-2300:a (fire); the claw swept with x=70 and x=-70 alternately,
        40 frames each, from 900; 1100-1104:b;1500-1504:b;1900-1904:b
am start -n gg.dekiru.nagori3k/.MainActivity --es game '<image>' --ez perf true --es script '<script>'
```

The counter read 28-30 most of the time, 24-25 in busy moments, 20 when the
superzapper went off, and 17 at worst, with the claw sweeping, firing and
enemies on the web. (Jake's own worst is about 12, in a busier scene.)

The Pad's time split in this game (ordinary build, `--ez perf true`):
generated code 39-48%, interpreter 3-6%, dispatcher and the rest 43-54%,
asleep 3-7%. On the Mac after S1.1, in the scripted busy stretch: generated
code 74%, interpreter 4%, dispatcher 8%. On the Pad the time outside
generated code is the larger half, so what Stage 0 found on the Mac does not
carry over as it is: the Pad needs its own measurement with the counters
built in.

## The Pad measured: busy scene at the console's own speed (2026-11-18)

Headless on the Pad (`perf_host` from `/data/local/tmp/nagori/stage0`, the
busy-scene script above, 150 s, frame cap 30, as in the app), counters built
in, three runs; then release 0.3's core (`d766e8c`) against today's
(`cef6b20`), ordinary builds, three runs each, alternating.

**Today's core is faster than 0.3's on the Pad**, not slower: 3,563, 3,564 and
3,574 game frames in 150 s for 0.3; 3,757, 3,759 and 3,762 for today's
(+5.4%, the light stretches included). Jake's worst of about 12 is therefore
most likely a heavier scene than the ones measured before, not a regression.

The runs repeat closely: every counters run holds 29-30 in the light
stretches and 21-22 in a heavy one that lasts the final minute.

| | light stretch (29-30 fps) | heavy stretch (21-22 fps) |
|---|---|---|
| generated code | 39.9% | 61.2% |
| interpreter | 3.7% | 4.8% |
| dispatcher proper | 21.1% | 6.9% |
| round level | 15.8% | 6.3% |
| media tick and loop between turns | 12.1% | 10.7% |
| asleep | 3.9% | 7.4% |
| rounds a second | 13.4 M | 3.9 M |
| MPE0 packets per running turn | 4.1 | 14.1 |

In the light stretch the game is held by the frame cap; MPE1 and MPE2 are
halted most of the time and MPE0 spins in its wait loop, interpreted, about
7 million turns a second. That is why the dispatcher and the round level look
large there: the thread is busy waiting, not short of time. (It costs power,
not frames.)

**In the heavy stretch the Pad runs the processors at the console's own
speed.** MPE0 runs 53.6 million packets a second against the speed limit of 54
million, MPE1 50.9 and MPE2 46.9, and the thread still sleeps 7% of the time.
So 21-22 fps there is what the original hardware would give, as far as the
packet count is a measure of it; making the generated code faster cannot raise
it. With the speed limit removed (`NUANCE_MPE_HZ=0`, frame cap still 30, two
runs) the same stretch gives 22-23 fps (3,886 and 3,884 game frames in 150 s,
+4.3%): MPE0 58.0 million a second, and the thread no longer sleeps. The Pad
has about 8% to spare over the console in this scene.

What follows: on the Pad, compiler work helps only in scenes where the Pad
falls below the console's speed (perhaps Jake's 12 fps one), or if the game is
allowed to run its processors faster than the console's, which would keep busy
scenes smoother than the original. Either way the time is now mostly in
generated code (61-66%), not in the interpreter or the dispatcher.
