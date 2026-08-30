# Anthroforge Engine — Phase 5: Unreal Engine 5 Plugin

## Reconciled FFI surface

Applying the Phase 2/3/4 `lib.rs` diffs on top of the real Phase 1 `lib.rs`
(not the diffs' own context lines, which drifted — see "Inconsistencies
found" below) produces a crate with:

- **Modules:** `gltf_loader`, `skeleton_resolver`, `obj_loader`,
  `clothing_deformer`, `texture_atlas`.
- **Dependencies:** `gltf`, `base64`, `serde`/`serde_json` (Phase 1),
  `tobj` (Phase 2), `rayon` (Phase 3). `texture_atlas` (Phase 4) adds none
  — it's pure `std`.
- **Exported `extern "C"` symbols**, exactly:

  | Symbol | Signature |
  |---|---|
  | `init_part_registry` | `(asset_dir: *const c_char) -> bool` |
  | `generate_character` | `(dna: *const CharacterDNA) -> *mut MeshOutputBuffer` |
  | `free_mesh_buffer` | `(buffer: *mut MeshOutputBuffer)` — **added this phase, see below** |
  | `generate_runtime_atlas` | `(head, torso, legs, feet: *const RawImage, target_atlas_size: u32) -> *mut RuntimeAtlasOutput` |
  | `free_atlas_buffer` | `(output: *mut RuntimeAtlasOutput)` |

This is the surface `AnthroforgeCoreTypes.h` mirrors.

## Inconsistencies found while reconciling

1. **`free_mesh_buffer` was documented everywhere but never implemented.**
   Phase 1's module doc comment, and Phase 1/2's inline comments, all say
   `MeshOutputBuffer` "must eventually be released via `free_mesh_buffer`"
   — but no phase 1-4 output ever defines it. Every `generate_character`
   call was leaking its buffer for the process lifetime. Rather than work
   around this in C++ (which would just relocate the leak), `rust-core/src/lib.rs`
   now implements it, mirroring `free_atlas_buffer`'s existing, working
   `Box::from_raw` + `Vec::from_raw_parts` pattern exactly.
2. **Phase 4's `lib.rs` diff context doesn't match the real file.** Its
   diff shows `mod vertex_dna;` as an existing line it's inserting
   `mod texture_atlas;` next to — `vertex_dna` doesn't exist anywhere in
   Phases 1-3. Treated as a diff-authoring error and ignored; `texture_atlas`
   was added to the real module list instead (alongside `obj_loader` and
   `clothing_deformer`, which needed the same treatment against Phase 1's
   actual `mod` list rather than Phase 2/3's diff context).
3. **`panic = "abort"` (Phase 1's release profile) silently defeats Phase
   4's panic-recovery design.** `texture_atlas::generate_runtime_atlas`
   wraps its implementation in `panic::catch_unwind` specifically so an
   internal panic collapses to a `null` return instead of crossing the FFI
   boundary — but with `panic = "abort"` already set crate-wide, the
   process aborts before any unwinding happens, so `catch_unwind` never
   actually runs in a release build. Left as-is (see `Cargo.toml`'s comment)
   rather than changed unilaterally, since flipping to `panic = "unwind"`
   is a real decision affecting every other export too.
4. **`clothing_deformer` (Phase 3) is compiled in but has no FFI entry
   point at all**, and isn't reachable from `generate_character` either —
   Phase 3's own write-up says as much. Concretely blocked on: (a) an
   exported "build anchors for this clothing part" entry point; (b)
   `CharacterDNA` needs an equipped-clothing list (today it only has
   `head_id`/`torso_id`); (c) `generate_character` needs an actual
   DNA-mutation step to produce the `mutated_vertices` buffer
   `fit_clothing_to_skin` binds against — none of Phases 1-4 implement
   DNA-driven mesh mutation. This is a Rust-side gap; nothing on the C++
   side can paper over it, so `AnthroforgeCoreTypes.h` declares no
   clothing-related function pointers.
5. **No error code crosses the FFI boundary anywhere** — every failure is
   a `bool`/`null` with the real reason only on Rust's `stderr`, which a
   packaged, non-console build will never surface. `AssembleCharacterAsync`
   can log "it failed" but not *why* beyond what's inferable from context.

Full detail on all five, plus two smaller data-format caveats (RGBA8 byte
order isn't type-enforced; `SkinnedVertex`'s bone data has no consumer in
`UDynamicMeshComponent`), is in the header comment block at the top of
`unreal-plugin/Source/AnthroforgeEngine/Public/AnthroforgeCoreTypes.h`.

## Layout

```
anthroforge-engine/
├── copy_lib.sh / copy_lib.bat
├── rust-core/                    # Cargo.toml + src/*.rs, reconciled per above
└── unreal-plugin/
    ├── AnthroforgeEngine.uplugin
    └── Source/AnthroforgeEngine/
        ├── AnthroforgeEngine.Build.cs
        ├── Public/  (AnthroforgeEngineModule.h, AnthroforgeCoreTypes.h, AnthroforgeCharacterAssembler.h)
        └── Private/ (AnthroforgeEngineModule.cpp, AnthroforgeCharacterAssembler.cpp)
sample-assets/
├── master_skeleton.json
└── parts/   (empty — drop .gltf/.glb/.obj modular parts here)
```

## Build

```
./copy_lib.sh      # macOS/Linux
copy_lib.bat        # Windows
```

Builds `rust-core` in release mode and copies the resulting
`.dll`/`.so`/`.dylib` into
`unreal-plugin/Source/AnthroforgeEngine/Binaries/ThirdParty/`, which is
where `UAnthroforgeCharacterAssembler::GetDylibPath()` looks for it at
runtime.

Note: this sandbox has no Rust toolchain available (network is restricted
to package registries, not `rustup`/`static.rust-lang.org`), so the
reconciled `rust-core` above was reviewed by hand for layout/type
correctness against the four phase outputs rather than compiled here. Run
`cargo build` locally as a first step before `copy_lib`.
