# Phase 6 Final Integration — Merge Notes

This records how `body_mutation.rs`, `mesh_merge.rs`, and the modified
`lib.rs` (clothing-anchor caching) were reconciled into the Phase 5
baseline (`anthroforge-engine-phase5-final.zip`), and wires all three plus
`clothing_deformer`'s existing `fit_clothing_to_skin`/
`dna_scale_from_character_dna` into `generate_character`, per
`CONTEXT_6D_MERGE_INTEGRATION.md`.

## Mismatch: no `clothing_manifest.rs` was actually delivered

The task write-up describes the third piece as "a modified `lib.rs`/new
`clothing_manifest.rs` adding clothing-manifest parsing and init-time
anchor caching to the `Registry`," and later refers to a
`registry.clothing_anchors(...)` accessor and to clothing anchors being
scoped to "one of the body types the clothing manifest declared."

What was actually delivered is only the **anchor-caching** half of that:
a modified `lib.rs` adding `Registry::skin_tree_cache` /
`clothing_anchor_cache`, `Registry::get_or_build_skin_tree` /
`get_or_build_clothing_anchors`, and the optional
`anthroforge_prewarm_clothing` prewarm entry point. There is no
`clothing_manifest.rs` file, no manifest-parsing logic anywhere in the
delivered `lib.rs` (confirmed: zero occurrences of "manifest" in the
file), and no accessor named `clothing_anchors` — the delivered accessor
is `get_or_build_clothing_anchors(head_id, torso_id, clothing_id)`.

**Decision:** treated the delivered anchor-caching code as authoritative
and integrated against its actual shape rather than the write-up's
assumed shape:

- Step 7 of the integration calls
  `registry.get_or_build_clothing_anchors(dna.head_id, dna.torso_id, id)`
  in place of the write-up's `registry.clothing_anchors(...)`. Same
  purpose (per-body cached anchors for one clothing id), different name,
  no behavior change needed on top of what was delivered.
- The write-up's caveat about a clothing id being skipped when "this
  exact head/torso combination simply isn't one of the body types the
  clothing manifest declared" doesn't apply as literally written — there
  is no manifest restricting which body types are "supported." What the
  delivered code actually does, and what this integration relies on, is
  build anchors on demand for *any* resolvable (`head_id`, `torso_id`,
  `clothing_id`) triple; the only ways to end up skipping an item are the
  ones `Registry::get_or_build_clothing_anchors`/`get_or_build_skin_tree`
  can actually return: an unresolvable id, or a failure inside
  `clothing_deformer::build_skin_kdtree`/`build_cloth_anchors_with_tree`
  (e.g. a non-finite vertex). `generate_character`'s doc comment and the
  per-item `eprintln!` in `Step 2` below both describe this accurately
  rather than repeating the manifest framing.
- No `clothing_manifest.json` was added to the new integration test's
  temp asset directory (Step 3 of the write-up), since nothing in the
  delivered code reads one. The test reuses the existing
  `make_temp_asset_dir_with_clothing` helper (already present in the
  delivered `lib.rs`), which already provides a head, a torso, and two
  clothing `.obj` parts plus a valid `master_skeleton.json` — sufficient
  for the actual (non-manifest) code path being exercised.

If a real `clothing_manifest.rs` is delivered in a later round, the
integration point to revisit is `generate_character`'s clothing loop: it
would gain a manifest lookup ahead of
`get_or_build_clothing_anchors`, and the "skip: unsupported body type"
case would become a distinct, named failure mode instead of falling out
of the generic anchor-build error path.

## `lib.rs` base: delivered piece vs. Phase 5 baseline

Diffed the delivered `lib.rs` against the Phase 5 baseline directly: the
delivered file is the baseline plus exactly the anchor-cache additions
(`Registry` gains `skin_tree_cache`/`clothing_anchor_cache`, the new
`ClothingAnchorError` enum, `Registry::get_or_build_skin_tree`/
`get_or_build_clothing_anchors`, `prewarm_clothing_anchors_impl`, the
`anthroforge_prewarm_clothing` FFI export, and their tests) — no other
drift. The delivered file was used directly as the integration base
rather than hand-diffing a merge, since there was nothing else to
reconcile against.

## Step 1 — module wiring

Added `mod body_mutation;` and `mod mesh_merge;` to `lib.rs`'s module
list (alphabetized alongside the existing five). Both modules compiled
against `crate::SkinnedVertex` with no signature changes needed — their
delivered signatures already matched what `generate_character` needed
(`mesh_merge::merge_parts(&[(&[SkinnedVertex], &[u32])]) -> Result<(Vec<SkinnedVertex>, Vec<u32>), MeshMergeError>`
and
`body_mutation::mutate_skin_vertices(&[SkinnedVertex], [f32; 3]) -> Result<Vec<SkinnedVertex>, BodyMutationError>`).

## Step 2 — `generate_character` rewrite

Implemented exactly the 9-point sequence in the write-up: resolve both
`head_id`/`torso_id` (new torso check, with its own specific last-error
message); merge with `mesh_merge::merge_parts`; derive scale via
`clothing_deformer::dna_scale_from_character_dna`; mutate with
`body_mutation::mutate_skin_vertices`; read
`equipped_clothing_ids_ptr`/`equipped_clothing_count` (same pattern
`anthroforge_prewarm_clothing` already uses for the same field pair,
including treating a null pointer with nonzero count as "no equipped
clothing" rather than a hard failure — no exported function in this
crate reads that pointer without also being defensive about a
caller-side mismatch there); look up each clothing part + call
`get_or_build_clothing_anchors` (see mismatch note above) + call
`clothing_deformer::fit_clothing_to_skin` directly, skipping-not-failing
on any per-item error; final `mesh_merge::merge_parts` over the mutated
body plus every successfully-fitted clothing item; `shrink_to_fit` +
`mem::forget` + `Box::into_raw`, unchanged from before. Added the
`CLOTHING_CLEARANCE_EPSILON: f32 = 0.001` crate-level constant next to
`SKIN_KDTREE_DECIMATION_STRIDE`. Updated the function's doc comment to
describe the new composition/mutation/clothing-fit behavior instead of
the old "stub" description.

### Pre-existing test needed a small fix, not just a passthrough

`successful_call_after_failure_clears_last_error` (already in the
delivered `lib.rs`, unchanged from the Phase 5 baseline) constructed its
`CharacterDNA` with `height_modifier: 0.0, weight_modifier: 0.0` and
`torso_id: 0`. Both were harmless placeholders before this integration,
since `generate_character` never read `torso_id` and never derived a
scale from the modifiers. After this integration:

- `torso_id: 0` no longer resolves (no test asset uses id `0`), which
  would now trip the new torso-resolution check this test isn't trying
  to exercise.
- `height_modifier`/`weight_modifier` feed
  `dna_scale_from_character_dna` -> `mutate_skin_vertices`, which
  rejects a `<= 0.0` scale component — `[0.0, 0.0, 0.0]` would now fail
  DNA mutation instead of reaching the success path this test asserts.

Fixed by changing `torso_id: 0` to reuse the same loaded part id as
`head_id` (this test only needs *a* resolvable torso, not an
anatomically distinct one) and both modifiers to `1.0`. No other
existing test constructs a live (non-null-DNA, non-unresolvable-head-id)
`CharacterDNA` with a zero modifier or a `torso_id` expected to resolve,
so this was the only test needing this kind of update.

## Step 3 — integration tests

Added two tests to `lib.rs`'s existing `mod tests`, reusing the
already-delivered `registry_with_test_clothing_parts`/
`make_temp_asset_dir_with_clothing` helpers rather than duplicating asset
setup:

- `generate_character_composes_head_torso_and_fitted_clothing`: builds a
  `CharacterDNA` with one equipped clothing id and a deliberately
  non-uniform, non-identity scale (`height_modifier: 2.0,
  weight_modifier: 1.5` — identity/uniform scale was checked first and
  confirmed it would *not* move the bind-pose-identical test geometry
  enough to satisfy the "actually did something" assertion, since this
  fixture's head/torso/clothing parts are all the same triangle at the
  same coordinates); asserts `vertices_count` equals the sum of the three
  parts' vertex counts (3 + 3 + 3 = 9); asserts at least one of the
  trailing (clothing) output vertices differs from that clothing part's
  original bind-pose position by more than a small epsilon; frees the
  buffer.
- `generate_character_unknown_torso_id_sets_specific_last_error`: valid
  `head_id`, `torso_id: u32::MAX`; asserts null return and a last-error
  message containing "no part loaded for torso_id" specifically (not a
  generic or head-shaped message).

## Step 4 — verification

- `cargo build --release`: clean (same three pre-existing
  `texture_atlas.rs` dead-code warnings as the Phase 5 baseline; nothing
  new).
- `cargo test --release`: **62 / 62 tests pass** — the 41 tests already
  in the Phase 5 baseline, plus 5 already added by the delivered
  clothing-anchor-cache `lib.rs` piece, plus 3 already added by the
  delivered `clothing_deformer.rs` piece (`build_skin_kdtree`/
  `build_cloth_anchors_with_tree` and their tests), plus 5 from
  `mesh_merge.rs` and 6 from `body_mutation.rs` (delivered with their own
  `#[cfg(test)]` modules, unmodified), plus the 2 new integration tests
  from Step 3 above (41 + 5 + 3 + 5 + 6 + 2 = 62).
- Toolchain: `rustc`/`cargo` 1.75.0 (matching the toolchain pin noted in
  `MERGE-NOTES.md`); `cargo build --release` resolved current crates.io
  versions of `rayon`/`rayon-core` without needing the `--precise` pin
  `MERGE-NOTES.md` recorded for the Phase 5 round — no `Cargo.lock` was
  carried over from that round, and a fresh resolution against today's
  crates.io index built cleanly on this toolchain.
