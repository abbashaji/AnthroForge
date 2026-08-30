# Phase 5 Consolidation — Merge Notes

This project's `rust-core` and `unreal-plugin` sources are the result of
manually reconciling **seven independent test rounds** (`01-fix.patch` /
`RESULTS-01.md` through `07-fix.patch` / `RESULTS-07.md`), each of which was
run fresh against the original `anthroforge-engine-phase5.zip` rather than
against each other's output. That meant several rounds independently
rediscovered and re-fixed the same defect in different ways. This file
records how those were reconciled and confirms the final state builds and
passes its full test suite.

## The recurring conflict: `texture_atlas.rs` thread-safety bug

The crate as delivered did not compile: `generate_runtime_atlas`'s
concurrent blit captured `&RawImage` (a struct holding a raw `*mut u8`) into
a `std::thread::scope` closure, which is not `Send`. Five of the seven
rounds (01, 02, 03, 05, 06, 07) independently found and fixed this, using
three different approaches:

- **01, 02, 03, 05**: assorted per-call-site wrapper types (`SendConstPtr`,
  `SendSourceRef`, `SendSource`) wrapping either the source pointer or
  reference for each spawned closure.
- **06, 07**: a single `unsafe impl Send`/`Sync for RawImage` directly on
  the struct, which makes any `&RawImage` `Send` automatically — simpler,
  and functionally equivalent given each thread only ever reads its own,
  non-aliased source image.

**Decision:** the direct-impl approach (06/07) was kept as the canonical
fix; the four redundant wrapper types from 01/02/03/05 were removed rather
than left as dead code, with a short note left in place explaining why
they're absent for anyone diffing against those individual patches.

## Per-round disposition

| Round | Unique contribution kept | Notes |
|---|---|---|
| 01 | — | Fully superseded by 06/07's simpler fix; nothing else in this round. |
| 02 | `full_pipeline_all_quadrants_receive_correct_distinct_data` test | Adapted to call the canonical fix instead of its own wrapper. |
| 03 | `panic = "abort"` → `"unwind"` in Cargo.toml, plus `generate_runtime_atlas_panic_is_caught_not_propagated` regression test | Its own redundant Send-fix hunk dropped; a stray `source.0` left by a fuzzy patch match was corrected back to `source`. |
| 04 | Clothing-deformer FFI wiring (`build_cloth_anchors_for_part`, `fit_clothing_to_character`, `free_cloth_anchor_buffer`), `CharacterDNA` grown 24→40 bytes with an equipped-clothing list | Applied cleanly; no overlap with the texture_atlas conflict. |
| 05 | `error.rs` + `anthroforge_last_error()` FFI export, thread-local last-error propagation through `init_part_registry`/`generate_character`/`generate_runtime_atlas`/`free_atlas_buffer` | Its own redundant Send-fix hunk (`SendConstPtr`) dropped. Its test module's `CharacterDNA` literals were missing the two new clothing fields added by round 04 — updated to include `equipped_clothing_ids_ptr: null, equipped_clothing_count: 0`. |
| 06 | Canonical `texture_atlas.rs` Send/Sync fix | Applied first as the base for all later reconciliation. |
| 07 | `gltf_loader.rs` out-of-range index validation + tests | Its texture_atlas hunk (identical in substance to 06's) was skipped as redundant. |

## `AnthroforgeCoreTypes.h`

Rounds 04 and 05 both rewrote the header's "RECONCILED FFI SURFACE" summary
and "GAPS FLAGGED" sections. These were hand-merged so the final header:

- Lists all **nine** exported symbols (the eight from round 04 plus
  `anthroforge_last_error` from round 05).
- Marks gap item 3 (no error code, only null/non-null) as **resolved** by
  `anthroforge_last_error`, per round 05.
- Updates gap item 5 (`panic = "abort"` blocking `catch_unwind`) to reflect
  round 03's fix to `panic = "unwind"`, including the caveat that
  `init_part_registry`/`generate_character` still abort on an internal
  panic (Rust's FFI-unwind guard aborts regardless of the panic strategy
  for a plain `extern "C"` function whose body isn't wrapped in
  `catch_unwind`).
- Updates gap item 6 to describe the actual fix used (`unsafe impl
  Send`/`Sync for RawImage`) rather than the wrapper-type approach one
  individual round used.

## Build environment

No `Cargo.lock` shipped with the original zip. As found independently in
round 01 (see `RESULTS-01.md`), the crate's declared dependency ranges
resolve by default to a `rayon`/`rayon-core` version requiring rustc 1.80+,
while the available toolchain is 1.75.0. The same pin used in round 01 is
carried in this consolidation's committed `Cargo.lock`:

```
cargo update -p rayon --precise 1.10.0
cargo update -p rayon-core --precise 1.12.1
```

## Final verification

- `cargo build --release`: clean (only pre-existing dead-code warnings for
  internal helpers not yet called from outside their module).
- `cargo test --release`: **41 / 41 tests pass**, combining the original
  14 tests with every unique regression test contributed by rounds 02, 03,
  04, 05, and 07.
- Verified in a fresh `git clone` of the consolidated tree (not just the
  in-place working copy) that the build and test suite reproduce
  identically using the committed `Cargo.lock`.
