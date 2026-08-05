# TAD — function-cycle profiling mode for `sameboy_tester`

**Status:** draft, pending review.
**Requested by:** kwigbo-gb-sdk engine lane (SONG_FORMAT arc, that TAD's D11).
**Implementation host:** Linux box — needs `make tester` and the SDK harness.
See [`../CLAUDE.md`](../CLAUDE.md) for the two-host split.
**Drafted on:** the operator's Mac, 2026-08-05.

## Problem

The SONG_FORMAT audio arc needs the driver's per-tick cost (`Music.tick`)
measured in real cycles rather than the hand-estimated "~600 cy" currently
quoted across five SDK docs. The number gates the audio arc's Phase B and
calibrates the iPad editor's budget meter, so it must be **automated and
repeatable** inside the SDK's pytest harness — not a one-shot manual reading.
Emulicious, the current manual method, merges local labels and isn't
scriptable.

The core already has every primitive needed; the headless tester simply
doesn't drive them. This is a wiring job, not new emulation work.

## Decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | **Report T-cycles, and name the unit in the output header.** | `GB_run` returns *8MHz ticks*, not T-cycles (`Core/gb.h` — "Returns the time passed, in 8MHz ticks"). On DMG single-speed, T-cycles = ticks ÷ 2. The run loop's `139810` "≈1/60 s" constant is in 8MHz ticks — that is 69,905 T-cycles, adjacent to the true 70,224. Emitting raw deltas labelled T-cycles makes every budget assertion **2× wrong**, and a budget calibrated against an inflated first measurement becomes self-consistent and stops looking wrong. |
| D2 | **Bracket by stack pointer, not by "the next `ret`".** At entry capture `SP_entry` and `return_addr = read16(SP_entry)`; the call completes when `PC == return_addr && SP >= SP_entry + 2`. | Naive next-`ret` matching breaks on early returns, tail calls, and multi-exit routines. An SP-keyed stack of active frames also handles reentrancy and recursion — `Music.tick` runs from a timer IRQ in-game and can be preempted. |
| D3 | **Entry match is bank-qualified.** | `GB_symbol_t` carries a `bank` field (`Core/symbol_hash.h`), but `resolve_watch_token` discards it (`Tester/main.c` — `out->addr = sym->addr` only). Harmless for the RAM watches `--watch` was built for; wrong for code. A routine in the banked region `0x4000–0x7FFF` would false-trigger whenever any *other* bank maps that address. |
| D4 | **Report inclusive *and* exclusive of interrupt preemption, as separate columns.** | Since `Music.tick` itself runs from a timer IRQ, any interrupt taken *inside* the bracket adds its cycles to an inclusive measure. A budget meter wants exclusive. Detect entry to `0x40/0x48/0x50/0x58/0x60` while bracketed and accumulate a subtraction window. Emitting both lets the consumer choose without a re-spin. |
| D5 | **Accumulate true per-step `GB_run` deltas; do not inherit the frame chunk.** | The `139810` constant is commented "intentionally not the actual length of a frame"; it drives the tester's whole per-frame cadence (`frames++` → `test_length` termination, button scripts, trace rows) — a scheduling boundary, not cycle accounting. Same root cause as D1. |
| D6 | **`--profile-out` is incompatible with `--jobs > 1`; reject the combination.** | Direct precedent: `--trace-out` already rejects it (`Tester/main.c:644–647`) because forked runs interleave writes into one file. The existing check runs in the first-iteration harness-init block (post-parse), not at arg-parse; matching that placement is fine — the decision is the rejection, not its location. |
| D7 | **Output is CSV, one row per completed invocation, plus a summary trailer.** | The request asks for machine-parseable per-invocation counts "and/or max/total/count". Per-call rows are strictly more informative; count/total/max derive from them, and a trailer saves the harness a reduction pass. |
| D8 | **Profiling is inert unless `--profile` is passed — no behavior change to existing modes.** | The SDK's existing golden harness must stay green; acceptance criterion from the request. |
| D9 | **Reported unit is T-cycles, converted tester-side** (closes OQ1, confirms D1). | Client sign-off (PR #10 comment, 2026-08-05): the SDK's `song_cost.py` reasoning is denominated in the 70,224 T-cyc/frame ceiling; conversion belongs in the tester. |
| D10 | **The SDK golden asserts `t_cycles_excl`; both columns still emitted per D4** (closes OQ2). | Client sign-off: in-game `Music.tick` is IRQ-driven and preemptible; the budget wants the routine's own cost. `irq_count` explains any incl/excl divergence. |
| D11 | **A symbol name ambiguous across banks is an error at resolve time; no `bank:name` syntax in v1** (closes OQ3). | Client sign-off: `Music.tick` is unique in their `.sym`; disambiguation syntax can be added later without breaking the contract. |
| D12 | **CSV, one row per completed invocation + `# summary` trailer** (closes OQ4, confirms D7). | Client sign-off: matches the "max/total/count" ask; trivially parseable in the harness. |

## Proposed design surface

```
--profile <symbol>[,<symbol>...]   bracket each symbol entry→return, record cycles
--profile-out <path>               CSV destination (default: stdout)
```

Symbol tokens resolve through the existing `--sym` path (hex literal or a name
from the `.sym` map), extended per D3 to retain the bank.

Schema (values shown as `<n>` — this is a format illustration, not measured
data):

```csv
symbol,bank,call_index,entry_frame,t_cycles_incl,t_cycles_excl,irq_count
Music.tick,3,0,12,<n>,<n>,0
Music.tick,3,1,13,<n>,<n>,1
# summary symbol=Music.tick calls=<n> total_t_cycles_excl=<n> max_t_cycles_excl=<n> max_t_cycles_incl=<n>
```

Column names carry the unit explicitly (D1). `irq_count` is the number of
interrupts taken inside the bracket — it makes any inclusive/exclusive
divergence self-explaining rather than mysterious.

## Steps

| Step | Action | Validate | Rollback |
|---|---|---|---|
| 1 | Implement `--profile` / `--profile-out` in `Tester/main.c` (**Linux lane** — needs `make tester`) | `make tester` builds clean; run against `bin/MusicROM.gb` (VBlank-driven, no reentrancy — the clean bracket the request recommends starting from) and confirm per-call rows; confirm existing modes are unchanged with `--profile` absent | Revert the PR squash commit |
| 2 | Measure the worst-case fixture song (4 voices triggering on one tick + a loop-boundary re-fetch) under both ROMs (**Linux lane**) | Max per-invocation figure is stable across repeat runs — determinism is the whole point of replacing the Emulicious reading | Same |
| 3 | SDK-side wiring: pytest golden + `codegen/song_cost.py` `PROFILES` (**GameBoy Dev lane**, consumer repo) | Golden pins the max; a deliberately regressed tick trips the guard | Revert the consumer PR |
| 4 | `--help` + README update (**Linux lane**) | Options documented; the ~260-LOC `--watch` addition is the size precedent | Same |

## Client review status

- [x] kwigbo-gb-sdk (GameBoy Dev) — owns the parsed format; closed OQ1–OQ4
  "as recommended" with no overrides and posted STATUS:CLEAN on PR #10
  (2026-08-05). Answers folded into D9–D12.

## Downstream commitments

- **GameBoy Dev lane** — step 3 (pytest golden + `song_cost.py` `PROFILES`) in
  the consumer repo, referencing this TAD once merged.
- **SameBoy Manager (Linux box)** — steps 1, 2, 4. These cannot be done from
  the Mac lane: `make tester` and the SDK harness both live on the Linux box.
- **Manager lane** — *resolved before merge:* the draft flagged that
  `agent-server-manager` didn't yet reflect this fork's two-host shape; as of
  2026-08-05 (verified during PR #10 review round 1) Manager's lane table
  lists SameBoy Manager as **Linux + Mac (cross-host)** and its `CLAUDE.md`
  has a *Cross-host setup* section. Nothing remains owed.

## Progress log

- 2026-08-05 — Request received from the kwigbo-gb-sdk engine lane. Drafted on
  the Mac after a read-only review of the request against the code; six
  findings folded in as D1–D6. Two of them would have produced silently wrong
  numbers: the 8MHz-tick/T-cycle unit confusion (D1) and bank-blind symbol
  matching (D3). The request's other premises verified — the debugger *is*
  compiled into the tester (core objects only take `-DGB_DISABLE_DEBUGGER`
  when `DISABLE_DEBUGGER` is set), `--sym` / `--watch` / `--script` /
  `--trace-out` all exist, and the run loop already inspects `gb.pc` between
  `GB_run` calls (`Tester/main.c:809`) — with the caveat carried by that
  check's own comment: during vblank, PC "might not point to the next
  instruction," so the precedent is a single guarded detector, not general
  PC matching. D2's bracket tolerates a missed observation: the SP condition
  (`SP >= SP_entry + 2`) prevents a coincidental PC value from closing a
  frame early, and a genuine return that goes unobserved on one step is
  caught on a later one.
- 2026-08-05 — PR #10 opened; review round 1 (`--lite` panel, docs-only
  scope) returned four accuracy fixes (D5 phrasing, D6 precedent placement,
  PC-precedent caveat, `--watch` LOC figure) and caught that the Manager-docs
  downstream commitment had already been resolved; all folded in. OQ1–OQ4
  handed to the SDK lane via
  `kwigbo-gb-sdk/feedback/SAMEBOY_profile_TAD_open_questions.md`; their
  STATUS:CLEAN on PR #10 is the merge gate.
- 2026-08-05 — SDK lane signed off: OQ1–OQ4 "as recommended", no overrides,
  STATUS:CLEAN posted on PR #10. Folded as D9–D12; Open Questions section
  retired. Client gate satisfied — implementation (steps 1, 4) proceeds as
  follow-up commits on this PR per canon pattern (a).
