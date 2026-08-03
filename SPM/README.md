# Swift Package Manager distribution

This directory, together with the root `Package.swift`, is a fork-only
(kwigbo-org) addition that packages the SameBoy emulator core as a
**source-based SPM C target**. Everything SPM-specific lives here or in
`Package.swift` so upstream (`LIJI32/SameBoy`) syncs stay clean.

## `SameBoyCore` target

- **Sources:** `Core/*.c` minus the iOS `CORE_FILTER` set from the Makefile's
  `_ios` target (`debugger.c`, `sm83_disassembler.c`, `symbol_hash.c`,
  `cheat_search.c`). The generated border tables in `Core/graphics/*.inc` are
  git-tracked and textually `#include`d, so there is no RGBDS/boot-ROM
  generation step.
- **Defines:** `GB_DISABLE_DEBUGGER` (as `make _ios` passes; `gb.h` derives
  `GB_DISABLE_CHEAT_SEARCH` from it) plus `GB_INTERNAL` for the target's own
  compilation only, matching the Makefile's `Core/%.c.o` rule. `GB_VERSION`
  is pinned in `Package.swift` — keep it in sync with `version.mk`.
- **Module surface:** `include/module.modulemap` + `include/SameBoyCore.h`
  expose `Core/gb.h` and `Core/memory.h` (for `GB_safe_read_memory`) as the
  importable `SameBoyCore` module, without `GB_INTERNAL` — consumers see the
  opaque-handle API only.

## License scope

Only `Core/` is distributed by the package target. The repository `LICENSE`
(vendored with any package checkout) excepts the `iOS/` and `HexFiend/`
directories from the MIT grant — `iOS/` requires written permission to ship
on the App Store. **Never add `iOS/` or `HexFiend/` sources to a package
target.**

## Planned follow-up (P1) — `SameBoyApple`

A second target `SameBoyApple` (depending on `SameBoyCore`) can be added
later without restructuring: wrap `AppleCommon/` (`GBViewMetal.m` video,
`GBAudioClient.m` audio), gate it to iOS/macOS, and link Metal +
AVFoundation. It gets its own headers directory under
`SPM/SameBoyApple/include/`. `SameBoyCore` stays UIKit/Metal-free so headless
`swift test` keeps working on any host. Swift emulator wrappers belong in
consuming apps, not in this repository.
