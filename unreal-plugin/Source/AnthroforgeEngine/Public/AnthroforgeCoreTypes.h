// AnthroforgeCoreTypes.h
//
// Byte-for-byte C++ mirrors of every #[repr(C)] struct exported by the Rust
// core (anthroforge_core), plus function-pointer typedefs for the
// reconciled FFI surface below.
//
// ============================================================================
// RECONCILED FFI SURFACE (see also the Phase 5 write-up delivered alongside
// this code for the full derivation). Applying the Phase 5 clothing-wiring
// change plus the FFI error-propagation change on top of the four phase
// diffs (themselves applied to the real Phase 1 lib.rs) converges on
// exactly these nine exported symbols:
//
//   bool                    init_part_registry(const char* asset_dir);
//   MeshOutputBuffer*       generate_character(const CharacterDNA* dna);
//   void                    free_mesh_buffer(MeshOutputBuffer* buffer);      // ADDED Phase 5, see note 1 below
//   RuntimeAtlasOutput*     generate_runtime_atlas(const RawImage* head,
//                                                   const RawImage* torso,
//                                                   const RawImage* legs,
//                                                   const RawImage* feet,
//                                                   uint32 target_atlas_size);
//   void                    free_atlas_buffer(RuntimeAtlasOutput* output);
//   ClothAnchorBuffer*      build_cloth_anchors_for_part(                     // ADDED Phase 5, see note 2 below
//                                                   const SkinnedVertex* cloth_vertices_ptr,
//                                                   uint32 cloth_vertex_count,
//                                                   const SkinnedVertex* default_skin_vertices_ptr,
//                                                   uint32 default_skin_vertex_count);
//   void                    free_cloth_anchor_buffer(ClothAnchorBuffer* buffer); // ADDED Phase 5
//   bool                    fit_clothing_to_character(                        // ADDED Phase 5, see note 2 below
//                                                   const SkinnedVertex* skin_vertices_ptr,
//                                                   uint32 skin_vertex_count,
//                                                   SkinnedVertex* cloth_vertices_ptr,
//                                                   uint32 cloth_vertex_count,
//                                                   const ClothAnchor* anchors_ptr,
//                                                   uint32 anchor_count,
//                                                   const CharacterDNA* dna,
//                                                   float clearance_epsilon);
//   const char*             anthroforge_last_error(void);                    // ADDED Phase 5, see note 3 below
//
// Modules compiled into the crate (obj_loader, skeleton_resolver) still
// contribute no exported symbols of their own — both are folded into
// init_part_registry's part scan. clothing_deformer now contributes the
// three exports above (see note 2). texture_atlas contributes the atlas
// pair and nothing else (blit_to_quadrant/remap_uvs_for_quadrant stay
// internal to the .cdylib).
//
// GAPS FLAGGED (per the brief: "flag anywhere the Rust exports from phases
// 1-4 don't actually give C++ enough information to do its job correctly,
// rather than silently working around it"):
//
//   1. free_mesh_buffer did not exist. Every phase's doc comments and the
//      Phase 5 "Known Constraints" both assert it exists and must be
//      called after every generate_character(), but Phases 1-4 never
//      defined it — every MeshOutputBuffer produced by generate_character
//      was leaked for the process lifetime. It has been added to lib.rs
//      as part of this reconciliation (mirroring free_atlas_buffer's
//      existing, working pattern exactly) rather than worked around on the
//      C++ side, since a C++-side "just don't free it" workaround would
//      reintroduce the exact leak this constraint exists to prevent.
//
//   2. clothing_deformer::{build_cloth_anchors, fit_clothing_to_skin,
//      dna_scale_from_character_dna} were compiled into the crate but were
//      plain `pub fn`, not `#[no_mangle] extern "C"` — there was no FFI
//      entry point this plugin could call to fit clothing at all. As of
//      Phase 5, build_cloth_anchors/fit_clothing_to_skin became reachable
//      via the build_cloth_anchors_for_part / fit_clothing_to_character
//      exports declared below (mirroring generate_runtime_atlas/
//      free_atlas_buffer's exact ownership and null-handling conventions);
//      dna_scale_from_character_dna is exercised internally by
//      fit_clothing_to_character rather than exposed as its own symbol.
//      That closed the FFI-reachability half of the original gap. The
//      other half — blockers (b) and (c) below — was flagged as open in
//      Phase 5 and is now [RESOLVED — see PHASE_6_MERGE_NOTES.md]:
//        (b) [RESOLVED in Phase 6] CharacterDNA's equipped-clothing-item
//            list (equipped_clothing_ids_ptr/equipped_clothing_count,
//            growing the struct from 24 to 40 bytes — see
//            FAnthroforgeCharacterDNA below) is now fully consumed by
//            generate_character: every id in the list is resolved against
//            the part registry, fitted to this character's mutated skin
//            via clothing_deformer::fit_clothing_to_skin using this
//            (head_id, torso_id) pair's cached anchors (see
//            Registry::get_or_build_clothing_anchors), and merged into
//            the returned mesh. An id that doesn't resolve, or that fails
//            to fit, is logged to stderr and skipped — it does not fail
//            the whole generate_character call.
//        (c) [RESOLVED in Phase 6] generate_character now runs a real
//            DNA-mutation step before any clothing fitting happens: the
//            merged head+torso skin is mutated by
//            body_mutation::mutate_skin_vertices using a per-axis scale
//            derived from height_modifier/weight_modifier by
//            clothing_deformer::dna_scale_from_character_dna, and that
//            mutated buffer — not a caller-supplied placeholder — is what
//            equipped clothing is now fitted against.
//      With both (b) and (c) resolved, generate_character() alone now
//      produces a fully composed, clothed, DNA-mutated character; a
//      caller no longer needs to drive build_cloth_anchors_for_part /
//      fit_clothing_to_character by hand for the common case (those two
//      exports remain available for callers that want to fit clothing
//      outside of a generate_character call, e.g. a live "try on" preview
//      against an already-generated body).
//
//   3. [RESOLVED this phase] generate_character previously had no error
//      CODE, only a null/non-null return, with every failure path in
//      init_part_registry_impl/generate_character/generate_runtime_atlas
//      collapsed to a single bool/nullptr and the real reason only on
//      Rust's own stderr (which a packaged, non-console UE build never
//      sees). A new export, anthroforge_last_error(), now returns the
//      specific reason for the most recent failure on the calling thread
//      (see the typedef and doc comment below, and RESULTS-05.md for the
//      full design). No existing signature changed. AAssembler code that
//      wants to surface *why* a generation failed (e.g. in
//      GenerateCharacterMeshData's null-Buffer branch, or
//      BeginPlay's bRegistryInitialized=false branch) can now call this
//      immediately after the failing call, on the same thread, and log or
//      surface the returned string instead of only guessing from context.
//      This C++ wiring itself is not part of this phase's deliverable
//      (scoped to the Rust core + header per the Phase 5 brief) but is
//      now possible without any further Rust-side changes.
//
//   4. RawImage/RuntimeAtlasOutput give no channel-order guarantee in the
//      struct itself (the Rust doc comments say "RGBA8", but that's a
//      comment, not part of the type). CreateTransientRuntimeTexture below
//      assumes RGBA8 byte order per those comments; if a future Rust
//      change reorders channels, nothing in this header will catch it at
//      compile time.
//
//   5. [RESOLVED — see RESULTS-03.md] `panic = "abort"` in the crate's
//      release profile (carried over from Phase 1, untouched by Phases
//      2-4) meant texture_atlas's own `catch_unwind`/`ThreadPanicked`
//      recovery path could not run in a release build — an internal Rust
//      panic aborted the whole process, taking the UE5 game process down
//      with it, rather than returning `null` from generate_runtime_atlas
//      the way its doc comment promised. Test 03 changed the profile to
//      `panic = "unwind"` (see Cargo.toml's comment for the full
//      empirical verification): `generate_runtime_atlas`/
//      `free_atlas_buffer` now genuinely recover from an internal panic
//      and return null/no-op instead of aborting. `init_part_registry`
//      and `generate_character` were audited and do NOT gain the same
//      protection — Rust's FFI-unwind guard still aborts the process if a
//      panic would unwind past either of those two plain `extern "C"`
//      functions, regardless of this profile setting — so an internal
//      panic reaching either of them is still a hard process abort with
//      nothing C++ can do to catch it. Flagged here so that distinction
//      isn't silently assumed away.
//
//   6. [Found and fixed, unrelated to error propagation] The crate as
//      delivered did not actually compile: generate_runtime_atlas's
//      concurrent blit captured `&RawImage` into a std::thread::scope
//      closure without being Send, which rustc rejects (a struct holding
//      a raw pointer, like RawImage, is not Send/Sync by auto-trait
//      inference). This was never caught because the Phase 1-5 sources
//      had never actually been run through a Rust toolchain (see the
//      Phase 5 README's build note). Fixed in texture_atlas.rs with an
//      explicit `unsafe impl Send`/`Sync for RawImage` (each spawned
//      thread only ever reads its own, disjoint source image — see that
//      impl's doc comment for the full soundness argument) plus an
//      explicit whole-value re-capture of the existing `SendPtr`-wrapped
//      atlas pointer inside each spawned closure, working around Rust
//      2021's disjoint-closure-capture rules. This does not change any
//      behavior, signature, or the FFI surface documented above.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "AnthroforgeCoreTypes.generated.h"

// ----------------------------------------------------------------------------
// Plain (non-USTRUCT) FFI mirrors.
//
// These are deliberately NOT UPROPERTY-reflected USTRUCTs: they are read
// directly out of buffers the Rust side allocated (SkinnedVertex arrays via
// raw pointer arithmetic, RawImage pixel buffers via memcpy), so their C++
// layout must match Rust's #[repr(C)] layout exactly and nothing may be
// inserted by Unreal Header Tool. All of them use natural (unpacked)
// alignment, which is what Rust's #[repr(C)] itself uses, and every size is
// verified below with static_assert against the exact byte counts documented
// in the Rust source.
// ----------------------------------------------------------------------------

/// Mirrors `anthroforge_core::SkinnedVertex` (gltf_loader.rs / obj_loader.rs
/// / lib.rs). 56 bytes, zero padding.
struct FAnthroforgeSkinnedVertex
{
	float Position[3];     // offset 0,  12 bytes
	float Normal[3];       // offset 12, 12 bytes
	float UV[2];            // offset 24, 8 bytes
	uint16 BoneIndices[4]; // offset 32, 8 bytes
	float BoneWeights[4];  // offset 40, 16 bytes
};
static_assert(sizeof(FAnthroforgeSkinnedVertex) == 56, "FAnthroforgeSkinnedVertex must exactly match Rust SkinnedVertex's #[repr(C)] layout (56 bytes).");

/// Mirrors `anthroforge_core::MeshOutputBuffer` (lib.rs). Field order is
/// mandatory: both pointers before both counts (see lib.rs's own comment on
/// why reordering silently changes struct size via padding).
struct FAnthroforgeMeshOutputBuffer
{
	FAnthroforgeSkinnedVertex* VerticesPtr; // offset 0,  8 bytes
	uint32* IndicesPtr;                     // offset 8,  8 bytes
	uint32 VerticesCount;                   // offset 16, 4 bytes
	uint32 IndicesCount;                    // offset 20, 4 bytes
};
static_assert(sizeof(FAnthroforgeMeshOutputBuffer) == 24, "FAnthroforgeMeshOutputBuffer must exactly match Rust MeshOutputBuffer's #[repr(C)] layout (24 bytes).");

/// Mirrors `anthroforge_core::CharacterDNA` (lib.rs). `Seed` is Rust's `u64`
/// mirrored as `int64` per the fixed constraint for this phase: Unreal's
/// UPROPERTY/Blueprint system has no native `uint64`. Treat `Seed` as an
/// opaque 64-bit bit pattern ONLY — never compare it as a signed magnitude,
/// never assume `Seed >= 0` means anything, and round-trip it to Rust with a
/// straight bitwise reinterpret (int64 -> uint64), not a numeric conversion.
///
/// `HeadId`/`TorsoId` mirror Rust's `u32` as `int32`; both are small,
/// designer-assigned part ids in practice, but the same "opaque bit
/// pattern, not a magnitude" caution applies if a part id is ever near
/// INT32_MAX.
///
/// PHASE 5 ADDITION: `CharacterDNA` grew an equipped-clothing-item list
/// (see GAPS FLAGGED item 2 above, blocker (b)). It is deliberately NOT
/// mirrored on this Blueprint-facing USTRUCT: Unreal's UPROPERTY system has
/// no clean way to expose a raw pointer + count pair to designers. As of
/// Phase 6, generate_character DOES fully consume this list (blocker (c)
/// is resolved — see GAPS FLAGGED item 2 above), so populating it now has
/// a real, observable effect; this struct simply still has no
/// Blueprint-editable field for it. It exists only on
/// FAnthroforgeCharacterDNA_FFI below, for a C++-side caller (e.g. an
/// equipped-items TArray already held by the assembler) to populate
/// directly at the FFI boundary. A separate piece of work adds the actual
/// equipped-clothing field to this Blueprint-facing struct.
USTRUCT(BlueprintType)
struct ANTHROFORGEENGINE_API FAnthroforgeCharacterDNA
{
	GENERATED_BODY()

	/// Opaque 64-bit seed. Do NOT expose this as a Blueprint-editable
	/// "random-looking small number" — treat it as a bit pattern (see the
	/// class comment above).
	UPROPERTY(BlueprintReadWrite, Category = "Anthroforge|DNA")
	int64 Seed = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Anthroforge|DNA")
	float HeightModifier = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Anthroforge|DNA")
	float WeightModifier = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Anthroforge|DNA")
	int32 HeadId = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Anthroforge|DNA")
	int32 TorsoId = 0;

	/// Part ids of every clothing item this character has equipped
	/// (resolved against the same part registry as HeadId/TorsoId). May
	/// be empty — an empty array means no clothing is equipped, which is
	/// a fully valid, non-error state.
	UPROPERTY(BlueprintReadWrite, Category = "Anthroforge|DNA")
	TArray<int32> EquippedClothingIds;
};
static_assert(sizeof(int64) == 8 && sizeof(float) == 4 && sizeof(int32) == 4,
	"FAnthroforgeCharacterDNA's FFI conversion assumes these exact primitive sizes.");

/// Plain, non-reflected mirror used only at the actual FFI call boundary
/// (built from FAnthroforgeCharacterDNA immediately before the call). Kept
/// separate from the USTRUCT above so Unreal's reflection/GC metadata for
/// the Blueprint-facing struct can never silently drift from the exact
/// layout `generate_character`/`fit_clothing_to_character` require.
///
/// PHASE 5: grew from 24 to 40 bytes (see GAPS FLAGGED item 2, blocker
/// (b)) with an equipped-clothing-item id list. `EquippedClothingIdsPtr`
/// may be null iff `EquippedClothingCount == 0`.
///
/// PHASE 6: `generate_character` now fully resolves and fits equipped
/// clothing from these two fields (see GAPS FLAGGED item 2, blocker (c),
/// now resolved). Concretely, `generate_character` reads
/// `EquippedClothingIdsPtr`/`EquippedClothingCount`, resolves each id
/// against the loaded part registry, builds/reuses per-body clothing
/// anchors via the lazy `Registry::get_or_build_clothing_anchors` cache,
/// fits each resolved item via `clothing_deformer::fit_clothing_to_skin`,
/// and merges every successfully-fitted item into the returned mesh. An
/// id that doesn't resolve, or that fails to fit, is logged and skipped —
/// it does not fail the whole `generate_character` call.
struct FAnthroforgeCharacterDNA_FFI
{
	uint64 Seed;                        // offset 0,  8 bytes
	float HeightModifier;               // offset 8,  4 bytes
	float WeightModifier;               // offset 12, 4 bytes
	uint32 HeadId;                       // offset 16, 4 bytes
	uint32 TorsoId;                      // offset 20, 4 bytes
	const uint32* EquippedClothingIdsPtr; // offset 24, 8 bytes
	uint32 EquippedClothingCount;        // offset 32, 4 bytes
	// 4 bytes of trailing padding to keep 8-byte pointer alignment.
};
static_assert(sizeof(FAnthroforgeCharacterDNA_FFI) == 40, "FAnthroforgeCharacterDNA_FFI must exactly match Rust CharacterDNA's #[repr(C)] layout (40 bytes as of Phase 5).");

/// Mirrors `anthroforge_core::texture_atlas::RawImage` (texture_atlas.rs).
/// Pixel format is RGBA8, row-major, no row padding (per Rust doc comments
/// — see GAPS FLAGGED item 4 above: not enforced by the type itself).
struct FAnthroforgeRawImage
{
	uint32 Width;   // offset 0,  4 bytes
	uint32 Height;  // offset 4,  4 bytes
	uint8* PixelsPtr; // offset 8,  8 bytes (natural 8-byte alignment)
	uint32 TotalBytes; // offset 16, 4 bytes (struct pads to 24 total)
};
static_assert(sizeof(FAnthroforgeRawImage) == 24, "FAnthroforgeRawImage must exactly match Rust RawImage's #[repr(C)] layout (24 bytes, naturally padded).");

/// Mirrors `anthroforge_core::texture_atlas::RuntimeAtlasOutput`.
struct FAnthroforgeRuntimeAtlasOutput
{
	FAnthroforgeRawImage AtlasImage; // offset 0,  24 bytes
	uint32 QuadrantWidth;             // offset 24, 4 bytes
	uint32 QuadrantHeight;            // offset 28, 4 bytes
};
static_assert(sizeof(FAnthroforgeRuntimeAtlasOutput) == 32, "FAnthroforgeRuntimeAtlasOutput must exactly match Rust RuntimeAtlasOutput's #[repr(C)] layout (32 bytes).");

/// Mirrors `anthroforge_core::clothing_deformer::ClothAnchor`
/// (clothing_deformer.rs). One clothing vertex's binding to the default
/// skin mesh, produced by `build_cloth_anchors_for_part` and consumed
/// (read-only) by `fit_clothing_to_character`. PHASE 5 ADDITION — see
/// GAPS FLAGGED item 2 above.
struct FAnthroforgeClothAnchor
{
	uint32 TargetSkinVertexIndex; // offset 0,  4 bytes
	float LocalOffset[3];          // offset 4,  12 bytes
	float ThicknessClearance;      // offset 16, 4 bytes
};
static_assert(sizeof(FAnthroforgeClothAnchor) == 20, "FAnthroforgeClothAnchor must exactly match Rust ClothAnchor's #[repr(C)] layout (20 bytes).");

/// Mirrors `anthroforge_core::clothing_deformer::ClothAnchorBuffer`.
/// Caller-owned output of `build_cloth_anchors_for_part`; must be
/// released via `free_cloth_anchor_buffer` and by no other allocator,
/// exactly like FAnthroforgeMeshOutputBuffer/FAnthroforgeRuntimeAtlasOutput
/// above. PHASE 5 ADDITION.
struct FAnthroforgeClothAnchorBuffer
{
	FAnthroforgeClothAnchor* AnchorsPtr; // offset 0, 8 bytes
	uint32 AnchorCount;                   // offset 8, 4 bytes
	// 4 bytes of trailing padding to keep 8-byte pointer alignment.
};
static_assert(sizeof(FAnthroforgeClothAnchorBuffer) == 16, "FAnthroforgeClothAnchorBuffer must exactly match Rust ClothAnchorBuffer's #[repr(C)] layout (16 bytes).");

// ----------------------------------------------------------------------------
// Function-pointer typedefs, one per real exported symbol in the reconciled
// surface. Names match the Rust #[no_mangle] symbol names exactly.
// ----------------------------------------------------------------------------

typedef bool (*FAnthroforge_InitPartRegistry)(const char* AssetDir);

typedef FAnthroforgeMeshOutputBuffer* (*FAnthroforge_GenerateCharacter)(const FAnthroforgeCharacterDNA_FFI* Dna);

/// See GAPS FLAGGED item 1 above: this export did not exist prior to this
/// phase's reconciliation and was added to lib.rs to close the leak.
typedef void (*FAnthroforge_FreeMeshBuffer)(FAnthroforgeMeshOutputBuffer* Buffer);

typedef FAnthroforgeRuntimeAtlasOutput* (*FAnthroforge_GenerateRuntimeAtlas)(
	const FAnthroforgeRawImage* Head,
	const FAnthroforgeRawImage* Torso,
	const FAnthroforgeRawImage* Legs,
	const FAnthroforgeRawImage* Feet,
	uint32 TargetAtlasSize);

typedef void (*FAnthroforge_FreeAtlasBuffer)(FAnthroforgeRuntimeAtlasOutput* Output);

/// PHASE 5 ADDITION — see GAPS FLAGGED item 2 above. Mirrors
/// `build_cloth_anchors_for_part`. `ClothVerticesPtr`/`DefaultSkinVerticesPtr`
/// may be null only if their paired count is 0.
typedef FAnthroforgeClothAnchorBuffer* (*FAnthroforge_BuildClothAnchorsForPart)(
	const FAnthroforgeSkinnedVertex* ClothVerticesPtr,
	uint32 ClothVertexCount,
	const FAnthroforgeSkinnedVertex* DefaultSkinVerticesPtr,
	uint32 DefaultSkinVertexCount);

/// PHASE 5 ADDITION. Mirrors `free_cloth_anchor_buffer`. Safe to call with
/// null (no-op); must not be called twice on the same pointer.
typedef void (*FAnthroforge_FreeClothAnchorBuffer)(FAnthroforgeClothAnchorBuffer* Buffer);

/// PHASE 5 ADDITION — see GAPS FLAGGED item 2 above. `SkinVerticesPtr`
/// must be whatever the caller considers this character's *mutated* skin
/// buffer. As of Phase 6 the crate does produce such a buffer internally
/// (`body_mutation::mutate_skin_vertices`, driven by
/// `generate_character` — see blocker (c), now resolved, above), but that
/// internal buffer is not itself exposed as a separate FFI output; a
/// caller of this standalone export must still supply its own mutated
/// skin buffer (e.g. for a live "try on" preview against a body it
/// already generated some other way), since `generate_character` does
/// not hand its internal mutated buffer back out to callers.
/// `ClothVerticesPtr` is mutated in place; `AnchorsPtr` should be the
/// exact buffer `BuildClothAnchorsForPart` returned for this clothing
/// item. Any pointer above may be null only if its paired count is 0.
/// Returns false (not a thrown exception / crash) on any validation
/// failure, mirroring `InitPartRegistry`'s bool-sentinel convention.
typedef bool (*FAnthroforge_FitClothingToCharacter)(
	const FAnthroforgeSkinnedVertex* SkinVerticesPtr,
	uint32 SkinVertexCount,
	FAnthroforgeSkinnedVertex* ClothVerticesPtr,
	uint32 ClothVertexCount,
	const FAnthroforgeClothAnchor* AnchorsPtr,
	uint32 AnchorCount,
	const FAnthroforgeCharacterDNA_FFI* Dna,
	float ClearanceEpsilon);

/// See GAPS FLAGGED item 3 above: this export did not exist prior to this
/// phase's reconciliation. Returns the specific reason for the most
/// recent failure of `init_part_registry`, `generate_character`, or
/// `generate_runtime_atlas` **on the calling thread**, or `nullptr` if no
/// error is currently recorded on that thread (nothing has failed yet, or
/// the most recent call on this thread succeeded).
///
/// # Ownership / lifetime (mirrors errno / Win32 GetLastError)
/// The returned `const char*` is BORROWED, not owned: it points into
/// memory the Rust core itself owns and manages. The caller must NEVER
/// call `free()` (or any deallocator) on it. It remains valid only until
/// the SAME thread makes another call into this library (including a
/// second call to this function itself) — copy it out (e.g. into an
/// `FString` via `UTF8_TO_TCHAR`) immediately if it needs to outlive that.
/// It reflects only errors from calls made on the calling thread; it is
/// not meaningful to call this from a different thread than the one that
/// observed the failure. This matches how this plugin already calls
/// `GenerateCharacterFn`/`InitPartRegistryFn`/`GenerateRuntimeAtlasFn` and
/// checks their result immediately afterward, on the same thread.
typedef const char* (*FAnthroforge_LastError)();

/// PHASE 6 ADDITION. Mirrors `anthroforge_prewarm_clothing`. Optional,
/// additive latency-hiding call: populates the clothing-anchor cache for
/// a given (head_id, torso_id) and a batch of clothing_ids ahead of time,
/// on a background thread — returns immediately, does not block the
/// calling thread, and does not indicate whether/when the cache was
/// actually populated. Every code path in `generate_character` already
/// works correctly whether or not this is ever called; this exists purely
/// so a caller (e.g. a loading-screen hook) can hide the first-use
/// anchor-build cost for combinations it already knows it will need.
/// `ClothingIdsPtr` may be null only if `ClothingCount == 0` (a no-op
/// call in that case).
typedef void (*FAnthroforge_PrewarmClothing)(
	uint32 HeadId,
	uint32 TorsoId,
	const uint32* ClothingIdsPtr,
	uint32 ClothingCount);
