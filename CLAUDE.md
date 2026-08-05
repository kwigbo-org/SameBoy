# SameBoy (kwigbo-org fork) — agent working notes

Anchor doc for any Claude Code session working in this repo, on either host.
Read this before assuming a workflow rule — including the rule that this file
is not itself canonical (see below).

## Canonical process docs

The source of truth for review/merge workflow and the multi-agent lane model
lives in the `agent-server-manager` repo, checked out as a sibling of this one:

| Doc | Covers |
|---|---|
| [`../agent-server-manager/CODE_REVIEW_PROCESS.md`](../agent-server-manager/CODE_REVIEW_PROCESS.md) | TAD phase, reviewer panel + tiers, majority-vote marker, iteration discipline, merge gates |
| [`../agent-server-manager/CLAUDE.md`](../agent-server-manager/CLAUDE.md) | Strict-lane model — who may write where |

Relative `../` paths on purpose: the same link resolves on the Linux box
(`/home/ubuntu/…`) and on the operator's Mac (`~/Desktop/Desktop/Git/…`).

**Re-read the canon at the moment a rule is invoked.** Do not act on a cached
summary — panel composition, majority threshold, and merge gates have all
changed before. That warning applies to this file too: everything below is a
fork-local convenience, and the canon wins on any conflict.

## Branch model (fork shape — differs from standard kwigbo repos)

| Branch | Role | Rule |
|---|---|---|
| `master` | Upstream mirror of `LIJI32/SameBoy` | Pull-and-push only for upstream sync. **Never merge into it** — it must stay fast-forwardable from upstream. |
| `develop` | Stable kwigbo branch (effective main) | Internal PRs target this. |
| `next` | Working branch | PRs go `next` → `develop`; reset from `develop` after each merge. |

Internal PRs: `next → develop`, or `feature/<name> → develop` when a change
has a plausible upstream story (cut those from `master`, not `develop`, so
kwigbo-only commits don't leak into an upstream PR).

Merge requires the fork-mode env vars — without them `merge-pr.sh` defaults to
`base=main head=develop` and refuses with a base/head mismatch:

```sh
MERGE_PR_BASE=develop MERGE_PR_HEAD=next ~/merge-pr.sh <PR>
```

`gh pr create` on a fork defaults to **upstream** (`LIJI32/SameBoy`). Confirm
`git config remote.origin.gh-resolved` is `base` before diagnosing this as a
token problem.

## Two-host lane

Unlike the single-host lanes, this fork is developed across two machines
because neither can build all of it. Both sides are required; neither is
optional.

| Capability | Host |
|---|---|
| `make tester`, `make sdl`, `make libretro`, `make lib`, `make bootroms` | Linux box |
| `kwigbo-gb-sdk` harness + pytest goldens that consume `sameboy_tester` | Linux box |
| Review panel + merge scripts (`~/code-review.sh`, `~/merge-pr.sh`) | Either — Linux by default |
| `swift build` / `swift test` | **Mac only** |
| `xcodebuild` for iOS simulator + device destinations | **Mac only** |
| `make cocoa`, `make ios*`, QuickLook | **Mac only** |
| Any SPM package change (`Package.swift`, `Core/include/`) | **Mac only** for acceptance |

Corollary that has already cost one release: **a macOS-host `swift build`
does not prove iOS-buildability.** SwiftPM's resource auto-scan covers a
target's whole `path` regardless of any `sources:` filter, so a root-scoped
target picked up `Cocoa/*.xib` and broke iOS while macOS builds stayed green
(v0.1.0-spm → v0.1.1-spm). Package changes are accepted only against an
**iOS destination**. See [`SPM/TAD.md`](SPM/TAD.md) Decision 6 amendment.

Apple-side build invocations (the generated scheme is named for the *package*,
not the product — `-scheme SameBoyCore` fails):

```sh
swift build                                                                   # macOS host
xcodebuild -scheme SameBoy -destination 'platform=iOS Simulator,name=iPhone 16 Pro' build
xcodebuild -scheme SameBoy -destination 'generic/platform=iOS' build
```

## Keeping the two hosts in sync

Work crosses hosts through **git, not conversation**. A merge tells the other
host that something changed; it does not say who owns what is left. So every
cross-host handoff is recorded in-tree:

1. **Spec first.** Cross-lane primitives and new features need a TAD before
   code (canon's TAD phase). Place it per canon's binary rule —
   `<subdir>/TAD.md` when the code has one natural home, else
   `docs/tads/<feature>.md`.
2. **Name the owner of each step.** The TAD's *Steps* table says which host
   performs each Action and Validate. A step only one host can run must say so.
3. **Name the consumers.** Cross-lane consumers listed in *Client review
   status* gate the TAD — their STATUS:CLEAN is the interface contract
   sign-off, not an informal message.
4. **Record follow-ups elsewhere in *Downstream commitments*.** This is how
   work owned by another repo or host travels — it reaches the other side on
   `git pull` instead of depending on a session that may not exist anymore.
5. **Append to the *Progress log* when a step actually lands**, with the
   evidence (command + result). An unrecorded verification reads as still
   owed, and someone will redo it.

The handoff itself is then just: merge to `develop` → other host pulls → it
reads its own name in Steps / Downstream commitments.

Cross-lane notes to Manager conventionally go in
`agent-server-manager/feedback/`, but that directory is **gitignored and local
to the Linux box** — anything written there from the Mac syncs nowhere. From
the Mac, use *Downstream commitments* or route via the operator.

## Guardrails

- **`iOS/` and `HexFiend/` are excepted from the repository LICENSE.** Never
  add them to an SPM target or any redistributed artifact.
- **`Package.swift`'s target path stays `"Core"`.** Widening it back to `"."`
  reintroduces the resource-scan bug above.
- **`GB_VERSION` in `Package.swift` is a literal pinned to `version.mk`**
  (currently 1.0.3). Re-sync it on upstream version bumps.
- SPM-visible releases get `v*.*.*-spm` tags so consumers can re-pin.
- Consumers must wire `GB_set_rgb_encode_callback` **and**
  `GB_set_pixels_output` before the first `GB_run_frame`; the core calls the
  rgb-encode callback unconditionally and segfaults on NULL.
- `GB_run` returns **8MHz ticks**, not T-cycles ([`Core/gb.h`](Core/gb.h)) —
  on DMG single-speed, T-cycles = ticks ÷ 2. Anything reporting cycle costs
  must state its unit.
