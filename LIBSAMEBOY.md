# libsameboy — substrate notes

Snapshot of this fork's `libsameboy.{so,a,o,dylib,dll}` build target — what it exposes, what its prereqs are, and how the `kwigbo-gb-sdk` test harness will consume it once the Phase 3 migration trigger fires (see [ROADMAP.md](ROADMAP.md)).

**Last verified**: 2026-05-17 against `develop` @ `2801179`.

This doc is a re-runnable substrate audit, not a forward-looking plan. Refresh after every "Branch sync" upstream merge; flag drift to the operator.

---

## Build prereqs

| Dep | Status on verification host | Notes |
|---|---|---|
| `gcc` or `clang` | ✅ `gcc` at `/usr/bin/gcc` | Makefile recommends clang but gcc works on Linux |
| `rgbasm` / `rgbgfx` (RGBDS) | ✅ at `/usr/local/bin/` | Needed for boot ROM builds, included in `make lib`'s dependency chain |
| **`cppp`** | ❌ Not installed | https://github.com/LIJI32/cppp — same author as SameBoy. Required for public header cleanup. |

### The cppp silent-failure footgun

If `cppp` is missing, `make lib` **returns exit code 0 but produces 0-byte header files** under `build/include/sameboy/`. The compiled libs in `build/lib/` build correctly without it — only the public C headers fail. The Makefile's pipeline is `sed | cppp | sed > out.h` with no `set -e` and no exit-status check, so each failed pipe silently writes an empty file.

Result: a libsameboy consumer that `#include`s `<sameboy/gb.h>` gets nothing, and the diagnosis costs hours unless you already know to check `build/include/sameboy/*.h` for size.

**Install cppp on dev and CI hosts before any libsameboy integration work begins.** Python `ctypes` bindings don't strictly need the cleaned headers (signatures can be hand-written from `Core/gb.h`), but anything that genuinely consumes the lib as a C library does.

---

## Build outputs

`make lib` produces:

| Artifact | Verified size | Purpose |
|---|---|---|
| [build/lib/libsameboy.a](build/lib/) | 1.0 MB | Static archive for static linking |
| [build/lib/libsameboy.o](build/lib/) | 1.0 MB | Single relocatable object |
| [build/lib/libsameboy.so](build/lib/) | 677 KB | Shared library — `.dylib` on macOS, `.dll` on Windows |
| `build/include/sameboy/*.h` | (22 files; 0 bytes each when cppp missing) | cppp-cleaned public headers |

Exported symbol count in `libsameboy.so`: **182**. Verified via `nm -D --defined-only`.

---

## Public API surface

`GB_gameboy_t` is opaque outside `GB_INTERNAL` (forward-declared at [Core/gb.h](Core/gb.h) around line 864). All state access goes through accessor functions. Surface relevant to the SDK harness use case, grouped by purpose:

### Emulator lifecycle

- `GB_alloc()` → opaque pointer
- `GB_init(gb, model)` → opaque pointer (same as `gb`)
- `GB_free(gb)` / `GB_dealloc(gb)`
- `GB_reset(gb)` / `GB_quick_reset(gb)` / `GB_switch_model_and_reset(gb, model)`
- `GB_is_inited(gb)` / `GB_get_model(gb)` / `GB_is_cgb(gb)` / `GB_is_sgb(gb)`

### ROM and boot ROM loading

- `GB_load_rom(gb, path)` / `GB_load_rom_from_buffer(gb, buf, size)`
- `GB_load_boot_rom(gb, path)` / `GB_load_boot_rom_from_buffer(gb, buf, size)`
- `GB_load_isx(gb, path)` — IS-CGB-DEBUGGER format
- `GB_load_gbs(gb, path, info)` / `GB_load_gbs_from_buffer(gb, buf, size, info)` — GBS audio ROMs
- `GB_get_rom_title(gb, char[17])` / `GB_get_rom_crc32(gb)`

### Frame loop and input

- `GB_run_frame(gb)` → `uint64_t` cycles consumed in the frame
- `GB_run(gb)` → unsigned; runs until the next vblank
- `GB_set_key_state(gb, key, pressed)`
- `GB_set_key_state_for_player(gb, key, player, pressed)` — multi-player link cable
- `GB_set_vblank_callback(gb, callback)` — fires per frame

### State inspection (the Phase 3 capabilities)

- **`GB_get_registers(gb)`** → `GB_registers_t *` with `A/F/BC/DE/HL/SP/PC`. Outside any `GB_INTERNAL` block at [Core/gb.h](Core/gb.h) line 934.
- **`GB_get_direct_access(gb, access, &size, &bank)`** → raw `void *` pointer to a region. Enum values from [Core/gb.h](Core/gb.h) lines 917–928:

  | Enum | Region |
  |---|---|
  | `GB_DIRECT_ACCESS_ROM` / `ROM0` | Cartridge ROM (current bank / bank 0) |
  | `GB_DIRECT_ACCESS_RAM` | WRAM |
  | `GB_DIRECT_ACCESS_CART_RAM` | Cartridge SRAM |
  | `GB_DIRECT_ACCESS_VRAM` | Tile data + tilemaps |
  | `GB_DIRECT_ACCESS_OAM` | Sprite attribute table (`$FE00–$FE9F`) |
  | `GB_DIRECT_ACCESS_HRAM` | High RAM |
  | `GB_DIRECT_ACCESS_IO` | I/O register page (warning in header: some regs need `GB_read/write_memory` to be correct) |
  | `GB_DIRECT_ACCESS_BGP` / `OBP` | BG / OBJ palette regs |
  | `GB_DIRECT_ACCESS_BOOTROM` | Boot ROM |
  | `GB_DIRECT_ACCESS_IE` | Interrupt enable |

- `GB_safe_read_memory(gb, addr)` → `uint8_t`, side-effect-free memory read. Declared in [Core/memory.h](Core/memory.h), not gb.h.
- `GB_get_pixels_output(gb)` → `uint32_t *` to the 160×144 (or 256×224 SGB) frame buffer.
- `GB_get_clock_rate(gb)` / `GB_get_unmultiplied_clock_rate(gb)` — for cycle-budget math.
- `GB_get_built_in_accessory(gb)` — printer / camera / workboy etc.

### Save state and battery

- `GB_save_state_to_buffer` / `GB_load_state_from_buffer` (BESS format)
- `GB_save_battery_size(gb)` / `GB_save_battery_to_buffer` / `GB_save_battery(gb, path)`
- `GB_load_battery_from_buffer` / `GB_load_battery(gb, path)`
- `GB_get_battery_dirty(gb)` / `GB_clear_battery_dirty(gb)`

### Debugger — limited public surface

- `GB_debugger_break(gb)` — immediate halt (not symbol-conditional)
- `GB_debugger_execute_command(gb, char *)` — REPL-string interface; destroys input
- `GB_debugger_evaluate(gb, expr, &result, &result_bank)` — expression eval
- `GB_debugger_name_for_address(gb, addr)` / `GB_debugger_describe_address(gb, addr, bank, exact, prefer_local)`
- `GB_debugger_load_symbol_file(gb, path)` / `GB_debugger_clear_symbols(gb)`
- `GB_debugger_is_stopped(gb)` / `GB_debugger_set_disabled(gb, disabled)`

**No `GB_set_breakpoint`.** This is why ROADMAP.md keeps `--stop-at` in Tester regardless of libsameboy migration — from Python, "stop at symbol" would have to be a PC-peek loop after each frame or a REPL-string injection via `GB_debugger_execute_command`. Both fragile.

### Misc

- `GB_set_log_callback(gb, cb)` — capture emulator log output
- `GB_set_execution_callback(gb, cb)` — fires on each executed instruction (heavy)
- `GB_set_lcd_line_callback(gb, cb)` — per scanline
- `GB_set_lcd_status_callback(gb, cb)` — LCD on/off transitions
- `GB_set_joyp_write_callback(gb, cb)` — game writes to JOYP register
- `GB_set_turbo_mode(gb, on, no_frame_skip)` / `GB_set_turbo_cap(gb, multiplier)`
- `GB_set_rendering_disabled(gb, disabled)` — skip pixel buffer fill for headless speedups

---

## Implications for Phase 3 in the roadmap

| Capability roadmap claims | Reachable? | Mechanism |
|---|---|---|
| CPU register watches (A/BC/DE/HL/SP/PC) | ✅ | `GB_get_registers(gb)` |
| Per-frame screen hash | ✅ | hash bytes from `GB_get_pixels_output(gb)` |
| Sub-frame / instruction stepping | ⚠️ Indirect | `GB_set_execution_callback` for per-instruction observation; no public single-step C call. Heavy if used continuously. |
| **OAM/VRAM/palette dumps** (not previously credited) | ✅ Bonus | `GB_get_direct_access(gb, ACCESS_*, &size, NULL)` — one call per region |
| **Per-frame cycle count** (Phase 1.4 in CLI) | ✅ Free | `GB_run_frame(gb)` returns it |

The bonus capabilities — direct region pointers and cycles-from-`GB_run_frame` — mean the libsameboy migration target is materially richer than the original roadmap claimed. Phase 1.2 (`--dump-range`) and Phase 1.4 (`--cycles`) are still required for CLI consumers (Tester), but a future `libsameboy_harness.py` gets them for free.

---

## Python binding choice: `ctypes`

When the trigger fires, use `ctypes` (stdlib).

Rationale:

- **No install dep** — `ctypes` is in the Python stdlib; `cffi` would add a wheel to the SDK's requirements.
- **Surface is small** (~15 functions for the harness's use case), with simple signatures (opaque pointer + ints + buffer pointers). No structs that need ABI-aware parsing.
- **Callbacks** for vblank / log are well-supported via `CFUNCTYPE`.
- **cppp moot for Python bindings** — `cffi`'s auto-bind-via-`#include` win evaporates when cppp's silent-failure mode is the default state; we'd be hand-writing signatures from `Core/gb.h` either way.

Sketch (illustrative — do not commit until trigger):

```python
import ctypes

lib = ctypes.CDLL("libsameboy.so")

GB_MODEL_DMG_B = 0x002  # from GB_model_t

lib.GB_alloc.restype = ctypes.c_void_p
lib.GB_init.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.GB_init.restype = ctypes.c_void_p
lib.GB_load_rom.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.GB_load_rom.restype = ctypes.c_int
lib.GB_run_frame.argtypes = [ctypes.c_void_p]
lib.GB_run_frame.restype = ctypes.c_uint64
lib.GB_safe_read_memory.argtypes = [ctypes.c_void_p, ctypes.c_uint16]
lib.GB_safe_read_memory.restype = ctypes.c_uint8
lib.GB_set_key_state.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_bool]
lib.GB_get_pixels_output.argtypes = [ctypes.c_void_p]
lib.GB_get_pixels_output.restype = ctypes.POINTER(ctypes.c_uint32)
# … ~10 more
```

---

## Re-verifying this audit

Run after every "Branch sync" upstream merge. Watch for regressions in any of these signals:

```bash
make clean
make lib
ls -la build/lib build/include/sameboy
nm -D --defined-only build/lib/libsameboy.so   # symbol count, exports
```

Drift to flag:

- Empty headers (cppp regression on host, or Makefile pipeline broken)
- Symbol count changes (new/removed public functions — update this doc)
- `GB_get_registers`, `GB_get_direct_access`, `GB_get_pixels_output` removed from exports (Phase 3 viability changes)
- Lib artifacts missing or wrong size class (build breakage)
