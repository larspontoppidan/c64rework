# Stage 3 architectural review

Stage 3 replaces the original 6510 program with a hand-written C++ replica.
This is difficult work, and the worker has a detailed reference in
`STAGE3-MANUAL.md`. The worker is responsible for building, running the
play gates, checking the frontier, running lint, and recording the evidence in
`Stage3/NOTES.md` at the game root.

The reviewer should not repeat that work.

## The point of the review

Main must become the game. It owns the original control flow, calculations,
state changes, and hardware programming. Twin may establish when an original
instruction executes and may compare the result, but it must never provide an
answer or execute missing work on Main's behalf.

A passing play is strong evidence, but it is not the definition of success. A
translation can pass because a comparison was weakened, because Twin was
allowed to run through untranslated code, or because a value was borrowed from
Twin. The senior reviewer looks past the green result and asks whether Main
earned it.

Stage 3 code is expected to resemble the original program. Awkward control flow,
machine-oriented state, and functions shaped like 6510 routines are not design
failures here; Stage 4 will detach and reshape them. The concern in Stage 3 is
strict lockstep, surfacing the game logic and passing plays without cheating.

## How to review a slice

Read the current checkpoint, the worker's changes, the relevant original
listing, and any previous `Stage3/REVIEW.md`. Understand what the
original did, what the new C++ now does, and why the frontier moved.

Example questions to focus on:

- Does Main reflect the disassembly in a way that matches every operation with
  sensible variable and function names?
- Are complicated sync constructions being built when `Sync.hpp` already has
  functions intended for the purpose?
- Are interrupts and hardware-sensitive accesses represented for the right
  reason and at the right place in the original execution?
- Have `*AtPc` operations been used for all chip-register accesses that must
  occur at the original instruction's time, rather than only where a failure
  happened to expose the need?
- Does every function representing an original routine begin with the
  corresponding `JoinAtPc`, so Main and Twin meet before the translated body
  runs?
- Can the new code still make sense with Twin absent, or has oracle state leaked
  into gameplay?
- Do the plays reach meaningful KB, screen, and SID comparisons at settled
  points? Sparse checks or carefully avoided `All()` comparisons can conceal
  drift just as effectively as weakening a mask.
- Are functions and variables named for their understood purpose? Names such as
  `var123`, `unknown`, or an address disguised as a name are signs that the
  original has not been understood well enough to translate confidently.
- Is the plugin code accumulated in one or two implementation files? Stage 3
  need not be elegant, but it must remain understandable and reviewable. The code
  should be partitioned into logical units.
- Did new discoveries make it back into the KB with real provenance? Stage 2
  and Stage 3 overlap: when translation exposes weak names, missing state,
  incorrect routine boundaries, or misunderstood data, the work should return
  briefly to Stage 2 analysis and improve the KB before proceeding.

Use [`STAGE3-MANUAL.md`](STAGE3-MANUAL.md) when an exact synchronization rule
or API contract matters. Do not reproduce its mechanical checklist in the
report.

## When to run commands

Treat the worker's recorded build, lint, and play results as inputs to the
review. Do not routinely rerun them.

The reviewer remains read-only on the implementation. Findings go back to the
worker; the reviewer does not fix the code or continue the translation.

## Deliverables

Replace `Stage3/REVIEW.md` with a concise current review containing:

1. **Previous findings.** Carry over findings from previous REVIEW.md that are
   unresolved or still has relevancy.
2. **New findings.** Any new breaches of policy found in the code.
3. **Architectural judgment.** This section is more loose. Give feedback on the
   structure that is being built. You are the senior architect looking at new
   code. Is it nice or horrific? Is it taking shape according to the spirit
   of Stage 3? If not, give feedback to the worker here.
4. **Next slice.** Recommend the next natural unit of work at the earliest
   frontier. Prefer a coherent, observable advance over an arbitrary quota of
   routines or frames.
