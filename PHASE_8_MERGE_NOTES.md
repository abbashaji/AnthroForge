# Phase 8 Merge Notes — Header Reconciliation (8a) + Blueprint Clothing Wiring (8b)

This records how `AnthroforgeCoreTypes-8a.h` and `AnthroforgeCoreTypes-8b.h`
(both diffed against the Phase 6 baseline, `anthroforge-engine-phase6-final.zip`)
were reconciled into one `AnthroforgeCoreTypes.h`, and how 8b's
`AnthroforgeCharacterAssembler.cpp` was carried forward, per
`CONTEXT_8C_MERGE.md`.

## Step 1 — `AnthroforgeCoreTypes.h`

Diffed both delivered copies against the Phase 6 baseline separately.

**8b** stayed exactly within its assigned boundary: the only change is a
single hunk inside `FAnthroforgeCharacterDNA`'s struct body, adding

```cpp
UPROPERTY(BlueprintReadWrite, Category = "Anthroforge|DNA")
TArray<int32> EquippedClothingIds;
```

immediately before the struct's closing brace. Nothing else in the file
was touched.

**8a mostly stayed within its assigned boundary, but touched two more
comment blocks than the task write-up named.** The write-up said 8a
should have touched "the top GAPS FLAGGED comment block, the
`FAnthroforgeCharacterDNA_FFI` doc comment, and the addition of a new
`FAnthroforge_PrewarmClothing` typedef." What was actually delivered
also rewrites two more doc comments to reflect blockers (b)/(c) being
resolved in Phase 6:

- The doc comment directly above `struct FAnthroforgeCharacterDNA`
  itself (not `_FFI`) — originally explaining why the Blueprint-facing
  struct has no clothing field "since nothing on the Rust side reads
  this list yet"; updated to note Rust *does* now consume it, and that
  the missing field is "a separate piece of work" (i.e., exactly what
  8b adds).
- The doc comment above the `FAnthroforge_FitClothingToCharacter`
  typedef — originally noting the caller must supply its own
  placeholder mutated-skin buffer "since nothing in the crate produces
  that buffer yet"; updated to note `generate_character` now produces
  one internally, but still doesn't hand it back out, so a standalone
  caller of this export still needs to supply their own.

**Decision:** treated this as compatible, not a conflict. Both extra
edits are prose-only, thematically part of the same "Phase 6 resolved
blockers (b) and (c)" narrative as the two locations the write-up did
name, and neither overlaps a single line with 8b's one struct-field
hunk (confirmed by line-range comparison: 8a's edits sit at lines
65–100, 214–228, 255–274, and 356–379+append against the baseline; 8b's
single insertion sits at baseline line 242, inside the struct body,
between 8a's two nearest edits). Merging was therefore a straightforward
union: applied 8a's file as the base (it already contains every 8a
change on top of the baseline) and inserted 8b's one field exactly where
its diff placed it.

The `FAnthroforge_PrewarmClothing` typedef 8a added:

```cpp
typedef void (*FAnthroforge_PrewarmClothing)(
    uint32 HeadId,
    uint32 TorsoId,
    const uint32* ClothingIdsPtr,
    uint32 ClothingCount);
```

was checked against the actual Rust export in `rust-core/src/lib.rs`:

```rust
pub extern "C" fn anthroforge_prewarm_clothing(
    head_id: u32,
    torso_id: u32,
    clothing_ids_ptr: *const u32,
    clothing_count: u32,
)
```

Parameter order, types, and `void` return all match. No fix needed.

## Step 2 — `AnthroforgeCharacterAssembler.cpp`

Only 8b touched this file, so its delivered copy was adopted directly
(diffed against the Phase 6 baseline to confirm scope: it adds
`LastErrorFn` resolution in `LoadLibraryAndResolveExports`, the
zero-initialized `FfiDna = {}` construction with the
`EquippedClothingIds` → `EquippedClothingIdsPtr`/`EquippedClothingCount`
reinterpret-cast wiring, and the `anthroforge_last_error()`-based error
log replacing the old head-id-only guess — nothing else changed).

## Step 3 — consistency check

- **Field name match:** the header's `FAnthroforgeCharacterDNA` gets
  `EquippedClothingIds` (from 8b) and the `.cpp`'s
  `GenerateCharacterMeshData` reads `DNA.EquippedClothingIds.GetData()`/
  `.Num()` (also from 8b, since only 8b touched the `.cpp`) — same name,
  confirmed by direct comparison, not just by assumption that both
  pieces used the same name.
- **`FAnthroforgeCharacterDNA_FFI`'s doc comment vs. actual `.cpp`
  behavior:** 8a's corrected comment says
  `EquippedClothingIdsPtr`/`EquippedClothingCount` are read by
  `generate_character`, resolved against the part registry, fitted via
  `clothing_deformer::fit_clothing_to_skin` using cached per-body
  anchors, and merged into the output mesh, with a per-item
  skip-not-fail policy on error. Verified directly against
  `generate_character`'s body in `rust-core/src/lib.rs` — this is an
  accurate description of the Rust-side behavior. Separately, the
  merged `.cpp` now actually *populates* those two fields from real DNA
  (`DNA.EquippedClothingIds`) rather than leaving them zero-initialized
  or garbage, so the comment's premise (that populating this pair has a
  real, observable effect) is also true of the C++ call site, not just
  the Rust side.
- **`FAnthroforge_PrewarmClothing` typedef:** checked above against
  `rust-core/src/lib.rs`'s actual export; matches.

## Step 4 — what this merge does and does not verify

This was a careful textual/structural reconciliation of two
non-overlapping (modulo the two extra comment locations noted above)
diffs against a common baseline, plus a line-by-line cross-check of the
header's claims against the actual Rust source and the `.cpp`'s actual
field usage. It is **not** a compiled verification. The `static_assert`s
already in the header (unchanged by this merge) can catch a `#[repr(C)]`
size/layout mismatch on the *Rust* side, but nothing in this environment
— nor in either delivered piece — had access to a real Unreal Engine
toolchain, so nothing here catches a mismatch against actual UE5 headers
(`TArray` layout/ABI assumptions, `UPROPERTY`/`UHT` codegen for the new
`EquippedClothingIds` reflection, `GENERATED_BODY()` macro expansion,
etc.). That verification is a separate, later step requiring an actual
UE5 project build. This merge should not be treated as "done and
verified" beyond what a careful read can establish.

Also note: `AnthroforgeCharacterAssembler.h` (the header, as opposed to
the `.cpp`) was provided as project context but was not part of this
merge task — it already contains the `LastErrorFn` member the `.cpp`
resolves and calls, and was left untouched.
