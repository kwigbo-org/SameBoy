# TAD — Source-based SPM distribution of the SameBoy core

**Requesting lane:** GB Editor iOS (request doc received 2026-08-03, verified against fork @ `b0fd2ab`).
**Producing lane:** SameBoy Manager (`kwigbo-org/SameBoy`).
**Pattern:** (a) in-repo TAD — ships in the same PR as the implementation.

## Decisions

| # | Decision | Rationale |
|---|---|---|
| 1 | Source-based SPM C target, no binary xcframework | Artifact host is Linux and cannot build xcframeworks; SPM cross-compiles device + simulator on the consuming Mac and the core stays host-testable. |
| 2 | Target mirrors `make _ios`: `Core/*.c` minus `debugger.c`, `sm83_disassembler.c`, `symbol_hash.c`, `cheat_search.c` | Makefile `_ios` `CORE_FILTER` is the source of truth for what the iOS core build compiles (Makefile lines 312–319). |
| 3 | Defines: `GB_DISABLE_DEBUGGER` only (of the feature flags) + `GB_INTERNAL` for target compilation | `make _ios` passes exactly `-DGB_DISABLE_DEBUGGER`; `gb.h` derives `GB_DISABLE_CHEAT_SEARCH` from it (gb.h lines 12–20), so no extra disable flags are needed. `GB_INTERNAL` matches the Makefile's `Core/%.c.o` rule (line 490). |
| 4 | `GB_VERSION` pinned as a literal in `Package.swift` | `save_state.c` needs it for the BESS name string; SPM manifests can't read `version.mk`. Comment in the manifest marks the sync point. |
| 5 | Public module surface = `Core/gb.h` + `Core/memory.h` via custom module map, `GB_INTERNAL` NOT defined | Consumers get only the opaque-handle API. The shim header defines `GB_DISABLE_DEBUGGER` before including `gb.h` so the consumer-visible declaration set matches what was actually compiled (debugger/cheat-search APIs hidden). |
| 6 | ~~Target `path: "."` + explicit `sources: ["Core"]`, all SPM-specific files under `SPM/`~~ **Amended 2026-08-03:** target `path: "Core"`, public headers at `Core/include/` | Original rationale (isolation) missed that SwiftPM auto-scans the entire target path for bundle resources regardless of the `sources` filter — a root-scoped target picked up `Cocoa/PopoverView.xib` (macOS xib) and hard-failed iOS builds (GB Editor iOS bug report 2026-08-03). Scoping the path to `Core/` leaves nothing for the resource scan to mis-process (`Core/` holds only `.c`/`.h`/`.inc`, verified). `publicHeadersPath` must live inside the target path, so the module map + shim moved to `Core/include/` — a two-file upstream-sync footprint inside `Core/`, accepted as the cost of a working iOS build. Future targets scope their own paths (`AppleCommon/`) the same way. |
| 7 | Only `Core/` is distributed; `iOS/` and `HexFiend/` never enter a package target | Both are excepted from the repository `LICENSE`; `iOS/` requires written permission for App Store distribution. Guardrail stated in `Package.swift`, `SPM/README.md`, and here. |
| 8 | Platforms: iOS 17, macOS 13; `cLanguageStandard: .gnu11`; no `unsafeFlags` | macOS enables the consumer's headless `swift test`. The Makefile's `-std=gnu11` must come from `cLanguageStandard` because `unsafeFlags` would make the package unusable as a remote dependency. |
| 9 | Release tagged `v0.1.0-spm` after merge | Consumer pins it in `Package.resolved`. Suffix distinguishes SPM packaging releases from upstream SameBoy version tags (`v1.0.x`). |

## Proposed design surface

```
Package.swift                swift-tools 5.9, package "SameBoy"
└── product .library("SameBoyCore")
    └── target SameBoyCore                        (amended 2026-08-03)
        path "Core"  exclude [4 CORE_FILTER files, graphics]
        publicHeadersPath "include"
        cSettings: GB_INTERNAL, GB_DISABLE_DEBUGGER, GB_VERSION="1.0.3",
                   _GNU_SOURCE, _USE_MATH_DEFINES, -I. -I../AppleCommon

Core/include/module.modulemap   module SameBoyCore { header "SameBoyCore.h" }
Core/include/SameBoyCore.h      defines GB_DISABLE_DEBUGGER, undefs GB_INTERNAL,
                                includes ../gb.h + ../memory.h
```

Consumer contract reachable through the module (all verified present in the
public headers): `GB_alloc` / `GB_init` / `GB_reset` / `GB_free` / `GB_dealloc`,
`GB_load_rom_from_buffer`, `GB_run_frame`, `GB_set_sample_rate`,
`GB_apu_set_sample_callback`, `GB_set_pixels_output`, `GB_safe_read_memory`,
`GB_set_key_state` (+ `_for_player`).

## Steps

| Step | Action | Validate | Rollback |
|---|---|---|---|
| 1 | Add `Package.swift`, `SPM/` (module map, shim header, README, this TAD), `.gitignore` entries | Native harness on Linux: compile the exact SPM source set with the exact cSettings flag set; compile a consumer TU against only the public headers dir; link; run fake-boot smoke test (`GB_run_frame` ×10, `GB_safe_read_memory` readback); negative-check that cheat-search API is hidden | Revert the PR squash commit — no existing file is modified except `.gitignore` |
| 2 | Mac build-check (Mac lane / operator): `swift build` for iOS device, iOS simulator, macOS; trivial link-and-run check | Acceptance criteria from the request doc | Same as step 1 |
| 3 | Tag `v0.1.0-spm` on the develop squash commit, push tag | Consumer resolves the package pin | `git tag -d` + delete remote tag |

## Client review status

- [x] GB Editor iOS lane — the request document IS the consumer-authored spec
  (sources, defines, module surface, license guardrail, P1 shape all specified
  by the consumer); implementation follows it 1:1. Deviations: none.
  Operator may re-open if the consumer lane wants a formal STATUS:CLEAN pass.

## Downstream commitments

- **GB Editor iOS lane:** thin Swift wrapper over the module (explicitly out of
  scope for this repo); pin `v0.1.0-spm` in `Package.resolved`.
- **Mac lane / operator:** step 2 build-check (this Linux box has no Swift
  toolchain or Apple SDKs — per lane build-host split).
- **This lane (P1, future TAD):** `SameBoyApple` target wrapping `AppleCommon/`
  (Metal video + audio), depending on `SameBoyCore`, gated to iOS/macOS.
  Design room reserved (Decision 6); no restructuring required.

## Progress log

- 2026-08-03 — Request received from GB Editor iOS lane; fork verified @ `b0fd2ab`.
- 2026-08-03 — Step 1 implemented on `next`; native validation harness passed
  (17 core objects, consumer TU, link, smoke run, negative check).
- 2026-08-03 — PR #7 merged (squash `f2af334`); tagged `v0.1.0-spm`.
- 2026-08-03 — **Amendment (truth-fix):** GB Editor iOS lane reported the iOS
  build failing on `Cocoa/PopoverView.xib` — SwiftPM's resource auto-scan
  covers the whole target path regardless of `sources`, which Decision 6's
  original form missed. Target re-scoped to `path: "Core"`, headers moved to
  `Core/include/`. macOS host builds could not catch this (macOS toolchain
  tolerates macOS xibs); iOS-destination build added to the step 2 checklist.
  Tag `v0.1.1-spm` after merge.
