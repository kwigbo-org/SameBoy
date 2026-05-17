# SameBoy fork — Roadmap

Fork-specific work tracked here. Upstream sync flows through periodic `Branch sync` merges (see git log); items below should not regress on sync.

---

## Vision — full headless debugging for the SDK

The end state is testing infrastructure that catches three classes of regression deterministically from CI, plus a bridge from interactive play to deterministic replay:

| Regression class | Today | After roadmap |
|---|---|---|
| **Game-logic bugs** — wrong state at frame N for a given input | Partially testable (1-byte memory watches only) | Multi-byte typed watches, region dumps; CPU registers *(Phase 3, gated)* |
| **Visual bugs** — wrong pixels at frame N | Not testable headlessly | Region dumps for OAM/VRAM/palette; screen-hash goldens via libsameboy |
| **Performance regressions** — code-path cycle cost crept up | Invisible until visible lag | Per-frame T-cycle column pinned in goldens |
| **Bug repro flow** — "I saw it once in SDL, can't pin it" | Manual reconstruction of inputs | Live record/stop → replayable `--script` |

Phase 1 is foundation that every other phase consumes. Phases 2–4 are largely independent of each other once Phase 1 lands. Cross-cutting test patterns at the bottom apply across phases.

---

## Phase 1 — Tester CLI extensions

Context: [Tester/main.c](Tester/main.c) carries this fork's `--sym` / `--watch` / `--script` / `--trace-out` extensions (commit 265d676), used by the sibling `kwigbo-gb-sdk` Python harness for headless DMG unit testing. Items 1.1 and 1.2 are reused by Phase 2 (interactive capture) and Phase 4 (differential testing) without modification; 1.3–1.5 are Tester-only.

After landing these, **stop extending Tester** for capabilities the public library already covers — that work goes to Phase 3 (libsameboy binding).

### 1.1 `--watch <name>:width` — typed memory watches

Widths: `u1` / `u2le` / `u2be` / `u4le`. Highest leverage — eliminates the "two adjacent 1-byte watches combined in Python" pattern that dominates current SDK harness usage. ~30 LOC in [Tester/main.c](Tester/main.c), ~10 in `kwigbo-gb-sdk/scripts/tests/harness/harness.py`. No new TSV columns; just wider hex per cell.

### 1.2 `--dump-range <addr>:<len>@<frame|every>` — binary region dumps

Side-files (`dump_<frame>.bin`) alongside the trace. Replaces the would-be-160-watch hack for OAM and palette table verification. Mandatory for any visual-bug regression that needs OAM, VRAM, or palette state at a specific frame.

### 1.3 `--stop-at <symbol>` + `--max-frames` — symbol-conditional halt

Complements existing `--length`. See "Why `--stop-at` lives in Tester regardless" in Phase 3 for the verified-against-source rationale.

### 1.4 `--cycles` — per-frame T-cycle count exposure

Adds a `cycles` column to the trace TSV: the T-cycle count for each emulated frame. SameBoy's Core already tracks total cycles internally; the per-frame delta is what surfaces perf regressions.

**Use case**: a golden trace pinning cycles per frame fails when a code-path cost changes — "PR #X added 200 cycles to the player hot path" surfaces before it manifests as visible lag at runtime. Critical for vblank-budget-sensitive paths (see SDK [CLAUDE.md](../kwigbo-gb-sdk/CLAUDE.md) risk-sensitive paths — player hot-path, vblank/LYC/palette pipeline) and for SDK Phase 7 (optimization sweep) regression discipline.

**Implementation**: one new flag, one new column, ~15 LOC. Mechanism in vblank callback: read total cycles, subtract last frame's value, emit.

### 1.5 `--exit-on <expr>` + structured exit codes

Today the Tester detects stack overflow and deadlock (in its `vblank` callback in [Tester/main.c](Tester/main.c)) but only logs to stderr text — the SDK harness can't distinguish "test passed", "predicate failed", and "ROM crashed" without string-parsing stderr.

**Proposal**: distinct exit codes — `0` normal, `10` stack overflow, `11` deadlock, `12` max-frames-without-stop-at, `13` `--exit-on` triggered. Optional `--exit-on <expr>` evaluates a debugger expression each frame (via existing `GB_debugger_evaluate`) and exits 13 when true. Pushes simple invariant checks below the harness boundary.

Enables Phase-4-style "crash-as-fixture" tests (deliberately-broken ROMs assert the right exit code) and lets the SDK harness raise typed exceptions instead of guessing from stderr.

---

## Phase 2 — Interactive capture bridge

A "record/stop" hotkey in the running emulator that captures, during live play, both (a) a per-frame state trace of configured watches/dump-regions and (b) the joypad input transitions. Output: a TSV state trace plus a `--script`-compatible input log.

### Why this shape

The trace + input log compose into deterministic replay: capture an interactive bug session once, then run `Tester --script captured.script.txt --watch <same list> --trace-out replay.tsv` and reproduce it bit-exact for headless verification. Bug repro graduates directly to a regression fixture with no manual reconstruction of "what did I press when." This is the missing bridge between interactive play and the headless harness.

### Mechanism — no Core changes needed

Public Core hooks already cover this:

- `GB_set_vblank_callback` (in [Core/gb.h](Core/gb.h)) — fires once per frame; the frontend's callback writes a TSV row when recording is active.
- `GB_set_key_state` is already routed through the frontend's input dispatch — log every call (with the current frame number) when recording is active.

Frontend changes: a `recording` flag toggled by hotkey, an output file pair opened on start, the watches/dump-regions list reused from Phase 1.1 / 1.2. Model the hotkey on the existing `pending_screenshot` pattern in [SDL/main.c](SDL/main.c) — `pending_record` flag flipped on F-key, acted on in the frame loop.

### Output format

Two files per session, sharing a base path:

- `<name>.trace.tsv` — same header/row format Tester emits (frame / buttons / watch columns; also `cycles` column once Phase 1.4 lands).
- `<name>.script.txt` — same line format Tester's `--script` accepts (`<frame> press|release <buttons>`).

A future replay run consumes the script and produces a fresh trace; diffing against the captured trace verifies bit-exact reproduction.

### Dependencies & scope

- **Blocks on Phase 1.1 and 1.2** — reuses both the configuration shape and the file formats.
- **SDL frontend first.** The development workhorse for this fork.
- **Cocoa and iOS deferred.** Same Core hooks apply but their menu/UI plumbing is more involved; revisit only if SDL alone proves insufficient.

### Pre-decision: WRAM vs. rendering state

For *visual* bugs specifically, WRAM is one step removed from what renders. Useful capture targets vary by bug class:

- Game-logic bugs → WRAM ranges (typically `$C000+` regions the game uses)
- Sprite/positioning bugs → OAM (`$FE00-$FE9F`)
- Tilemap/scroll bugs → VRAM tilemaps + LCDC/SCX/SCY/WX/WY/LYC
- Palette bugs → BGP/OBP0/OBP1 (DMG) or CGB palette RAM

Same mechanism captures any of these; the user picks the watch list per bug class. **No baked-in "WRAM only" default** — it would mislead on visual bugs.

---

## Phase 3 — libsameboy migration (gated, additive)

A future `libsameboy_harness.py` (ctypes/cffi against the Makefile's `lib` target, which emits `libsameboy.{so,dylib,dll,a}` plus cppp-cleaned public headers in [build/include/](build/include/)). **Additive, not replacement** — [Tester/main.c](Tester/main.c) remains the shell-friendly entry for CI smoke and standalone use.

### Capabilities only this phase unlocks

- **CPU register watches** (A/BC/DE/HL/SP/PC) — unreachable from CLI flags without an unbounded explosion of state-dumping syntax. Naturally Python attribute access.
- **Per-frame screen hash** (`xxh64(bitmap)`) — visual regressions become a single hex-column diff. Cheap golden screenshot testing without storing BMPs.
- **Sub-frame / instruction-level stepping** when a bug needs it.

### Migration trigger

When CPU register watches or screen hashes are actively wanted within 2–3 PRs. Most likely triggers are Phase 3 collision-query or Phase 4 FSM restructure work on the SDK side — see [`../kwigbo-gb-sdk/docs/internals/ROADMAP.md`](../kwigbo-gb-sdk/docs/internals/ROADMAP.md).

### Costs identified at decision time (do not lose these on revisit)

- **Subprocess isolation is load-bearing today.** Each `subprocess.run` gives per-test state isolation, kill-on-timeout (Python threading does this poorly), and pytest-xdist parallelism without thread-safety worries. In-process imports those as our problems.
- **Tester/main.c is not deprecated.** Shell-friendly entry stays. Migration is additive.
- **Subprocess overhead is not the bottleneck.** ~30 ms × 6 golden tests = 180 ms; `GB_run_frame` itself dominates wall clock. Speed alone does not justify migration.

### Why `--stop-at` lives in Tester regardless

Verified 2026-05-17 against [Core/debugger.h](Core/debugger.h): the public lib surface does **not** expose `GB_set_breakpoint`. What's available:

- `GB_debugger_break(gb)` — immediate halt, not symbol-conditional
- `GB_debugger_execute_command(gb, "breakpoint Label")` — REPL-string interface, fragile from Python
- `GB_debugger_evaluate(gb, expr, ...)` — runtime expression eval

So even after a libsameboy migration, "stop at symbol" from Python means either a PC-peek loop after each frame, or shoving REPL strings through `execute_command`. Symbol-conditional halt belongs in Tester (Phase 1.3) regardless.

---

## Phase 4 — Cross-emulator differential testing (independent, optional)

A binjgb headless wrapper alongside the SameBoy one, both consuming the same `--script` input timeline, both emitting traces in the Phase 1 TSV format. The SDK harness asserts the traces agree on observable state.

### Why this catches what golden traces don't

- Golden traces pin behavior against the *same* emulator. They catch regressions but not accuracy bugs that affect both runs identically.
- A second emulator with comparable accuracy is a second opinion. Drift means either SameBoy diverged (regression in the fork or upstream sync), binjgb diverged (less likely — vendored at a fixed commit), or the game depends on emulator-specific behavior (a real bug — the cart targets real DMG, not emulators).

### Why this is cheap

- binjgb is already vendored in the SDK at `web/tracker/static/binjgb.{js,wasm}` for the audio tracker, and binjgb's native build has a headless mode.
- Phase 1.1 / 1.2 watch and dump-range formats are emulator-agnostic — both wrappers emit the same TSV.
- Trigger only on demand: run differential mode for known-tricky scenarios, not every test.

### Effort + priority

~200 LOC binjgb wrapper + reuse of Phase 1 formats. Lower priority than 1, 2, 3 — implement when an accuracy bug shows up that the current harness can't catch. Until then, capability is queued.

---

## Cross-cutting test patterns

These are pytest patterns on top of the existing Harness, not Tester or Core changes. Each adds confidence that the regression net itself is sound.

### Determinism re-run guard

A meta-test that runs the same `--script` twice and asserts identical traces. Catches:

- Hidden non-determinism from uninitialized memory reads
- RNG seed leaks (state surviving across "fresh boot" tests)
- Race-like behavior from CGB double-speed switches mid-frame

No tooling work — pure pytest pattern. Worth landing once before the SDK's Phase 2.5h (position audit) is finalized, so the audit baseline itself is provably deterministic.

### Crash-as-fixture

Once Phase 1.5 lands, intentional-crash tests (deliberately-broken ROMs in `fixture-game`) verify the harness reports crash exit codes correctly. Belongs alongside the determinism guard as harness-self-test — proves the regression net detects what it claims to detect.

### Cycle-budget assertion

Once Phase 1.4 lands, golden traces can pin a max-cycles-per-frame assertion as a sidecar to the TSV diff. Failure mode: "trace matches but frame 327 cost 1840 cycles vs. 1640 baseline." Cheap insurance for vblank-budget-sensitive paths.

---

## Out of scope

For [Tester/main.c](Tester/main.c) CLI surface specifically:

- **CPU register watches** — Phase 3 trigger, not a CLI flag.
- **Per-frame screen hashes** — Phase 3 trigger, not a CLI flag.
- **Per-instruction tracing** — Phase 3 if needed; not Tester CLI.

For the roadmap as a whole:

- **Audio capture (APU sample stream)** — unrelated workflow.
- **Per-frame screenshot/video capture** — covered by external tooling.
- **Per-frame full-state snapshotting** — that's what the rewind buffer ([Core/rewind.c](Core/rewind.c)) is for. If a bug needs full-state inspection, snapshot via the existing save state mechanism instead.

---

## Other fork-specific work

*(none tracked yet — add entries here as fork-specific themes emerge that aren't testing-harness work)*
