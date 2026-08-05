# sameboy_tester — kwigbo-org fork extensions

The headless tester (`make tester` → `build/bin/tester/sameboy_tester`)
carries fork-only options used by the kwigbo-gb-sdk harness. Upstream options
are unchanged; everything below is inert unless its flag is passed.

## State-trace harness (commit `265d676`)

| Option | Effect |
|---|---|
| `--sym <path>` | Load an RGBDS `.sym` file into the debugger's symbol map. |
| `--watch <sym_or_addr>[,...]` | Sample these WRAM/HW addresses once per frame. Tokens are hex (`0xC100`) or `.sym` names. |
| `--script <path>` | Scripted joypad timeline (`<frame> press\|release <buttons>`), replaces `--start` automation. |
| `--trace-out <path>` | Per-frame TSV: frame, held buttons, watch values. Incompatible with `--jobs > 1`. |

## Function-cycle profiler (`--profile`)

Design and decision record: [`TAD.md`](TAD.md) (D1–D12, client-signed by the
kwigbo-gb-sdk lane).

| Option | Effect |
|---|---|
| `--profile <sym_or_addr>[,...]` | Bracket each entry→return of the named routine(s) and record cycle costs. Max 8 symbols. Incompatible with `--jobs > 1`. |
| `--profile-out <path>` | CSV destination (default: stdout). Requires `--profile`. |

Symbols resolve like `--watch` tokens (hex literal or `.sym` name), with two
differences: matching is **bank-qualified** (a routine in `0x4000–0x7FFF`
only matches while its bank is mapped; hex literals match any bank), and a
name that is ambiguous across banks is a hard error — make `.sym` names
unique.

### How it measures

At routine entry the profiler captures SP and the return address stored at
it; the bracket closes when PC reaches that return address with the stack
unwound past the entry frame (`SP >= SP_entry + 2`). This survives early
returns, tail calls, multi-exit routines, recursion, and IRQ preemption.
Interrupts taken *inside* a bracket (dispatch to `0x40/0x48/0x50/0x58/0x60`)
are timed and subtracted from the exclusive column.

### Output contract (parsed by the SDK harness — do not change lightly)

```csv
# rom: <path>
# unit: T-cycles (GB_run 8MHz ticks / 2, single-speed)
symbol,bank,call_index,entry_frame,t_cycles_incl,t_cycles_excl,irq_count
Music.tick,0,0,84,620,620,0
# summary symbol=Music.tick calls=514 total_t_cycles_excl=318680 max_t_cycles_excl=620 max_t_cycles_incl=620
```

- **Unit is T-cycles**, converted from `GB_run`'s native 8MHz ticks (÷2,
  valid at single speed — the conversion is wrong in CGB double-speed mode
  and the tester warns if it sees it; the consumer is DMG-only).
- `t_cycles_incl` includes interrupts taken inside the bracket;
  `t_cycles_excl` subtracts them; `irq_count` says how many were subtracted.
  Budget assertions should use `t_cycles_excl`.
- One row per **completed** invocation; `call_index` is 0-based per symbol;
  `entry_frame` is the tester's frame counter at entry.
- Lines starting with `#` are comments; the `# summary` trailer (one per
  symbol) carries calls / total / max so simple consumers can skip the rows.
- Brackets still open at end of run are not emitted (warned on stderr).
