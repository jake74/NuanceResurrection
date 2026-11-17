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

The gain is larger than the one Stage 0's arithmetic predicted (about 4.6%);
part of it is likely that turns now run longer before they end. Not measured
on the Pad yet, and Merlin Racing has not been run on it.

What remains interpreted in the busy stretch: delay slots of interpreted
branches and never-compiled branch packets (about 5.4 million a second
together) and control-register access (1.5 million).
