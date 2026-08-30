# `rust-core` Unsafe Audit

Systematic pass over every `unsafe` usage in `rust-core/src/*.rs`, done by
manual/static reading (no Miri/ASan/TSan available in this environment,
per the task instructions — a later CI step covers that). `cargo build
--release` and `cargo test --release` were run at the end of this pass;
both succeed, and all 62 pre-existing tests still pass (none added or
removed).

## Scope note

`grep -n "unsafe" rust-core/src/*.rs` was run first, then followed up
with a search restricted to actual `unsafe` keyword usages (excluding
comment-only mentions) to get the working list below: **46** matches,
confirming the count named in the task brief. All 46 are in `lib.rs`
(12), `clothing_deformer.rs` (13), `texture_atlas.rs` (18), and `error.rs`
(3, all in `#[cfg(test)]` code). **`body_mutation.rs` and `mesh_merge.rs`
contain zero `unsafe` usages** — both are pure safe-Rust, operating only
on already-validated `&[SkinnedVertex]`/`&[u32]` slices with no raw
pointers or FFI surface of their own. The task brief's framing ("`unsafe`
... across `lib.rs`, `clothing_deformer.rs`, `mesh_merge.rs`,
`body_mutation.rs`, and `texture_atlas.rs`") does not match the actual
crate as delivered; noted here rather than silently working around it.

Entries below group tightly-related blocks doing the same kind of
operation (e.g. the four `Vec::from_raw_parts`/`Box::from_raw` free
functions, which all follow one pattern) and split apart anything doing
a materially different kind of unsafe operation, per the task's
instruction not to force artificial splitting or lumping.

---

## `lib.rs`

### 1. `unsafe impl Send for PartData` / `unsafe impl Sync for PartData` (lines 170–171)

**What it does.** Manually asserts `PartData` (a `struct { vertices:
Vec<SkinnedVertex>, indices: Vec<u32> }`) is `Send`/`Sync`, so
`GLOBAL_REGISTRY: OnceLock<Registry>` (which contains a
`HashMap<u32, PartData>`) can itself be `Sync` and be read from multiple
FFI-calling threads concurrently.

**Invariant.** `PartData` and everything it transitively contains must
genuinely contain no interior mutability and no raw pointers — otherwise
sharing `&PartData` across threads (what `Sync` permits) could be a data
race.

**Where enforced.** `SkinnedVertex` (`#[derive(Clone, Copy)]`, all fields
`[f32; N]`/`[u16; 4]`) has no raw pointers or interior mutability; `Vec<T>`
is itself already auto-`Send`/`Sync` when `T` is. Verified by reading
`SkinnedVertex`'s full field list (lines 88–96) and `PartData`'s (lines
161–164) — neither field type introduces anything that would block the
auto-trait.

**Verdict: SOUND, but the manual `unsafe impl` is redundant today** —
`PartData` would already be auto-`Send`/`Sync` without it, since none of
its fields opt out. This isn't wrong (asserting an already-true fact
causes no unsoundness), but it's worth flagging as a latent risk: if a
future field is added to `PartData` or `SkinnedVertex` that *does* break
the auto-trait (e.g. a raw pointer, a `Cell`, an `Rc`), the auto-trait
would silently stop applying but this manual `unsafe impl` would keep
asserting `Send`/`Sync` with no compiler signal that the justification
changed. Not something this pass changes (it's existing, accurate-today
code, not a doc gap), but worth a follow-up note if `PartData` is ever
extended.

---

### 2. `generate_character`'s `dna` dereference (line 477)

**What it does.** `let dna: &CharacterDNA = unsafe { &*dna };` — turns
the caller-supplied `*const CharacterDNA` into a shared reference.

**Invariant.** `dna` must be non-null, correctly aligned, and point at a
fully-initialized `CharacterDNA` for the call's duration.

**Where enforced.** Non-null is checked immediately above (lines
460–464, returns null before reaching the `unsafe` block if
`dna.is_null()`). Alignment/initialization are **not** checked in this
crate — they can't be, from inside the callee — and are stated as an
explicit caller contract both on this function's own doc comment (lines
441–443) and the module-level safety contract (lines 9–16).

**Verdict: SOUND.** Null is verified; the rest is correctly documented
as an external, caller-side contract rather than silently assumed.

---

### 3. `generate_character`'s `equipped_clothing_ids` slice (line 546)

**What it does.** Builds `&[u32]` from `dna.equipped_clothing_ids_ptr` /
`dna.equipped_clothing_count`.

**Invariant.** When `equipped_clothing_count != 0`, the pointer must be
non-null and valid for reads of that many consecutive `u32`s.

**Where enforced.** The zero-count case returns `&[]` without touching
the pointer (lines 536–537); a null pointer with nonzero count is caught
and degraded to `&[]` with a logged warning (lines 538–544) *before* the
`unsafe` block runs, so the block at line 546 is only reached when the
pointer is non-null and the count is nonzero — matching exactly the case
the caller contract (this field's own doc comment, lines 106–125, and
`CharacterDNA`'s layout comment) requires. The one part of the invariant
that genuinely can't be checked here — that the pointer actually points
at `count` valid, readable `u32`s, as opposed to merely being non-null —
is external caller contract, same as entry #2.

**Verdict: SOUND.**

---

### 4. `free_mesh_buffer`: `Box::from_raw(buffer)` (line 675)

**What it does.** Reconstructs the `Box<MeshOutputBuffer>` that
`generate_character` leaked via `Box::into_raw` (line 643), taking
ownership back so it can be dropped.

**Invariant.** `buffer` must be either null (handled separately, lines
668–670) or a pointer previously returned by `generate_character`,
exactly once, not already freed.

**Where enforced.** This is a "must not double-free / must not use an
arbitrary pointer" contract that is inherently external — nothing inside
`free_mesh_buffer` can verify that `buffer` really came from
`generate_character` rather than being forged or freed twice by a buggy
caller. Correctly stated as caller contract in the function's own doc
comment (lines 659–665) and the module-level contract (lines 17–20).

**Verdict: SOUND** (as an `extern "C"` boundary function whose contract
is necessarily partly external — stated explicitly, not implied).

---

### 5. `free_mesh_buffer`: `Vec::from_raw_parts` for `vertices_ptr` / `indices_ptr` (lines 688, 700)

**What it does.** Reconstructs the two `Vec`s (`vertices`, `indices`)
that `generate_character` leaked via `mem::forget` after transferring
their raw parts into the `MeshOutputBuffer`, so they can be dropped.

**Invariant.** `Vec::from_raw_parts(ptr, len, cap)` requires `ptr` to
have been allocated by the same global allocator with capacity exactly
`cap`, and `len <= cap` with the elements at `[0, len)` initialized —
here specifically `len == cap == vertices_count`/`indices_count`.

**Where enforced.** Traced to the write side in `generate_character`:
`vertices.shrink_to_fit()` / `indices.shrink_to_fit()` are called
(lines 618–619) immediately before `vertices_count`/`indices_count` are
read off `.len()` (lines 621–622) and the pointers off `.as_mut_ptr()`
(lines 623–624), and *before* `mem::forget` (lines 631–632) — so at the
point of forgetting, `len == capacity == vertices_count`/`indices_count`
by construction, and that's exactly the triple `free_mesh_buffer` later
reconstructs with. Both sides live in the same file and were read
together to confirm this, not assumed from the comment alone.

**Verdict: SOUND**, with the same "caller must not double-free / must
pass the exact untouched pointer" residual external contract as entry
#4 — already stated in both this function's doc comment (lines 660–665)
and inline (lines 684–687, 699).

---

### 6. `anthroforge_prewarm_clothing`'s `clothing_ids_ptr` slice (line 762)

**What it does.** Copies `clothing_count` `u32`s out of caller memory
into an owned `Vec<u32>` before handing the ids to a spawned background
thread.

**Invariant.** `clothing_ids_ptr` valid for reads of `clothing_count`
consecutive `u32`s when `clothing_count != 0`; may be null only when
`clothing_count == 0`.

**Where enforced.** Both preconditions are checked immediately above,
before the `unsafe` block: `clothing_count == 0` returns early (lines
739–741), and a null pointer with nonzero count also returns early with
a logged warning (lines 742–748) — so the `unsafe` block is only reached
with a non-null pointer and nonzero count. The remaining "points at
`count` genuinely readable `u32`s" is correctly-documented external
caller contract (function doc comment, lines 728–731).

**Verdict: SOUND.** Worth calling out as a good pattern specifically
*because* the copy happens before the spawn (comment at lines 755–757
explains why: the raw pointer's validity is only guaranteed for this
call's duration, not for however long the background thread later
takes) — this is exactly the kind of "invariant holds only because of
where in the function this runs" reasoning the task asked to verify
rather than accept on the strength of a plausible-sounding comment.

---

### 7. `init_part_registry_impl`'s `CStr::from_ptr(asset_dir)` (line 780)

**What it does.** Wraps the caller's `asset_dir: *const c_char` as a
`CStr` before validating/converting it to UTF-8.

**Invariant.** `asset_dir` must be non-null, and point at a
NUL-terminated byte sequence valid for reads for the call's duration.

**Where enforced.** Non-null is checked immediately above (lines
774–776, returns `Err` before the `unsafe` block if null). NUL-
termination and readability for the string's actual length are, as with
the other `extern "C"` string-pointer entries in this crate, external
caller contract — correctly stated as such on the module-level contract
(line 12–13: *"a valid, non-null, NUL-terminated UTF-8 C string for the
duration of the call"*).

**Verdict: SOUND.**

---

### 8. Test-only `CStr::from_ptr`/raw-pointer dereferences (lines 964, 1429, 1437)

**What they do.** `tests::last_error_message()` wraps
`anthroforge_last_error()`'s return as a `CStr`; two `generate_character`
integration tests dereference the `*mut MeshOutputBuffer` they were just
handed and build a `&[SkinnedVertex]` from its `vertices_ptr`/
`vertices_count`.

**Invariant.** Same shape as entries #2 and #7 (pointer just returned by
the function under test, not yet freed, used immediately on the same
thread).

**Where enforced.** Each call site is a handful of lines below the
producing call with no intervening call into the library, and each
already carries its own `// SAFETY:` comment tying the invariant to
"just returned, not yet freed" (lines 960–963, 1427–1428, 1435–1436).

**Verdict: SOUND.**

---

## `clothing_deformer.rs`

### 9. `build_cloth_anchors_for_part_impl`'s two input slices (lines 794, 797)

**What it does.** Converts `cloth_vertices_ptr`/`cloth_vertex_count` and
`default_skin_vertices_ptr`/`default_skin_vertex_count` into
`&[SkinnedVertex]` via the shared `raw_parts_to_slice` helper (entry
#12).

**Invariant.** Each pointer valid for reads of its paired count of
`SkinnedVertex` when that count is nonzero; null accepted only when the
paired count is `0`.

**Where enforced.** Delegated to `raw_parts_to_slice`, which performs the
null/zero-count check itself (see entry #12) — so from this call site,
the invariant genuinely reachable to check locally *is* checked; what's
left (that a non-null pointer truly is valid for `count` reads) is
`build_cloth_anchors_for_part`'s documented external caller contract
(lines 738–743).

**Verdict: SOUND.**

---

### 10. `free_cloth_anchor_buffer`: `Box::from_raw` + `Vec::from_raw_parts` (lines 839, 847)

**What it does.** Same shape as lib.rs entries #4/#5, for
`ClothAnchorBuffer`/`anchors_ptr` instead of `MeshOutputBuffer`.

**Invariant.** Same as those entries, with `ClothAnchorBuffer`'s
`anchors_ptr`/`anchor_count` in place of `vertices_ptr`/`vertices_count`.

**Where enforced.** Traced the same way: the write side
(`build_cloth_anchors_for_part_impl`, lines 807–816) calls
`anchors.shrink_to_fit()` immediately before reading `.len()` into
`anchor_count` and leaking via `ManuallyDrop`, so `len == capacity ==
anchor_count` at the point of leaking — matching what
`Vec::from_raw_parts` here requires. The "not a forged/double-freed
pointer" half is documented external contract (lines 825–827).

**Verdict: SOUND.**

**One difference worth flagging (documentation-accuracy finding, not a
soundness finding):** the whole FFI block this entry sits in (comment
block at lines 683–712) says the `catch_unwind` wrapping
`build_cloth_anchors_for_part`/`free_cloth_anchor_buffer`/
`fit_clothing_to_character` "cannot actually run in a release build
today" because "the crate's `Cargo.toml` (`panic = \"abort\"`)". **That's
stale.** `Cargo.toml`'s `[profile.release]` was changed to `panic =
"unwind"` (see its own extensive comment, and `lib.rs`'s module doc
comment, lines 28–58) specifically so `catch_unwind` *would* work for
`texture_atlas`'s `generate_runtime_atlas`/`free_atlas_buffer` — and by
the same reasoning, for these three `clothing_deformer.rs` exports too,
since they use the exact same `panic::catch_unwind(AssertUnwindSafe(...))`
pattern (lines 759, 830, 926). This comment was evidently written before
that `Cargo.toml` change and never updated to match. It doesn't change
whether any `unsafe` block here is sound — `catch_unwind` either running
or not running doesn't affect pointer/lifetime validity — but it *is*
exactly the kind of "documented but not actually true of the current
crate state" gap the task asked to check for at this FFI boundary
(parallel to the C++-side uninitialized-struct-fields gap from Phase 8).
Not fixed in this pass (it's a prose correction to an existing block
comment discussing crate-wide panic behavior, not a missing
per-`unsafe`-block safety comment, so it falls outside what this pass
was scoped to touch) — flagging precisely so a follow-up can correct
lines 706–711 to match `lib.rs`'s already-accurate module doc comment.

---

### 11. `fit_clothing_to_character_impl`'s pointer conversions (lines 971, 978, 979, 994)

**What it does.** Dereferences `dna` (line 971, same shape as lib.rs
entry #2), then builds `&[SkinnedVertex]` for `skin_vertices` (978) and
`&[ClothAnchor]` for `anchors` (979) via `raw_parts_to_slice`, then
builds `&mut [SkinnedVertex]` for `cloth_vertices` (994) via
`slice::from_raw_parts_mut`.

**Invariant.** `dna` non-null/aligned/initialized; `skin_vertices_ptr`/
`anchors_ptr` valid for reads of their paired counts; `cloth_vertices_ptr`
valid for **reads and writes** of `cloth_vertex_count` (since this
function mutates in place).

**Where enforced.** `dna.is_null()` is checked immediately above (lines
965–967) before the deref. `skin_vertices`/`anchors` go through the same
`raw_parts_to_slice` null/zero-count gate as entry #9. `cloth_vertices_ptr`
is separately null-checked (lines 981–990): null with nonzero count is an
error, null with zero count returns `Ok(())` without reaching the
`unsafe` block, so the `unsafe { slice::from_raw_parts_mut(...) }` at
line 994 is only reached with a non-null pointer. The read/write (not
just read) requirement is stated explicitly in the function's doc
comment (lines 894–896). No aliasing concern between this mutable slice
and the two read-only ones: `skin_vertices`/`anchors`/`cloth_vertices`
are three separately-supplied pointers with no code here that could
cause them to alias short of the caller passing the same memory for two
of them, which is (correctly) the caller's documented responsibility, not
something the callee could detect.

**Verdict: SOUND.**

---

### 12. `raw_parts_to_slice` helper (lines 1010, 1022)

**What it does.** Shared `(ptr, count) -> Result<&'a [T], String>`
conversion used by entries #9 and #11.

**Invariant.** `ptr` valid for reads of `count` consecutive `T` when
`count != 0`.

**Where enforced.** The function itself performs the null/zero-count
half of the check (lines 1015–1020: null + zero count is `Ok(&[])`, null
+ nonzero count is `Err`), and only reaches `slice::from_raw_parts` at
line 1022 once `ptr` is known non-null. The remaining half — that a
non-null pointer really is valid for `count` reads — is, correctly, an
unsafe-fn-level contract on its own caller (doc comment, lines
1006–1009), forwarded transparently from each of its own call sites'
`extern "C"` contracts.

Note on the unconstrained `'a` in the signature (`unsafe fn
raw_parts_to_slice<'a, T>(...) -> Result<&'a [T], String>`): this is not
a foot-gun here — it's the same shape as `std::slice::from_raw_parts`
itself, whose signature is identical in this respect (`pub unsafe fn
from_raw_parts<'a, T>(data: *const T, len: usize) -> &'a [T]`). The
`unsafe` on the function is exactly what puts the burden of choosing a
sound `'a` on each caller, same as the standard library function it
wraps.

**Verdict: SOUND.**

---

### 13. Test-only pointer dereferences/slices (lines 1295, 1301, 1466)

**What they do.** `ffi_build_and_free_round_trip` dereferences a
just-returned `*mut ClothAnchorBuffer` and reads its `anchors_ptr`/
`anchor_count` as a slice; `character_dna_equipped_clothing_fields_round_trip`
reads a `CharacterDNA`'s `equipped_clothing_ids_ptr` back as a slice
right after constructing it from a local array.

**Invariant/where enforced.** Same shape as lib.rs entry #8 — pointer
just produced, used immediately, backing storage (`ids` array) provably
outlives the read (same-scope local). Each already carries its own
`// SAFETY:` comment (lines 1293–1294, 1299–1300, 1465).

**Verdict: SOUND.**

---

## `texture_atlas.rs`

### 14. `unsafe impl Send for RawImage` / `unsafe impl Sync for RawImage` (lines 50–51)

**What it does.** Asserts `RawImage` (which holds `pixels_ptr: *mut u8`,
otherwise not auto-`Send`/`Sync` because it contains a raw pointer) can
be shared and sent across the `std::thread::scope` worker threads in
`generate_runtime_atlas_impl`.

**Invariant.** Every place that actually *writes* through a `RawImage`'s
`pixels_ptr` must do so with exclusive, non-overlapping access; every
place that only *reads* through it can tolerate ordinary shared-read
concurrency (no writer aliasing a reader).

**Where enforced.** Traced every use of a `&RawImage` across the thread
boundary (lines 460–521): each worker thread only ever receives a
`&RawImage` for a **source** image and only ever *reads* through
`source.pixels_ptr` (`blit_to_quadrant_raw`, line 283, `as *const u8` —
never mutated). The one place bytes are actually *written* is the
**separate** `atlas_ptr`/`SendPtr`, restricted to a byte range proven
disjoint per thread by `Quadrant::origin` plus the size checks in
`blit_to_quadrant_raw` (see entry #16). So `RawImage`'s own pointer is
genuinely read-only across every thread that touches it in this crate —
confirmed by reading the call sites, not just the adjacent comment (lines
40–49).

**Verdict: SOUND**, confirmed against actual call sites as the task asked
(not just "plausible-sounding").

---

### 15. `blit_to_quadrant`'s call into the raw core (line 177)

**What it does.** The safe public wrapper converts `atlas_buffer: &mut
[u8]` into a raw pointer + length pair to call `blit_to_quadrant_raw`.

**Invariant.** `atlas_ptr`/`atlas_len` must describe a region valid for
reads/writes for the call's duration, with no concurrent aliasing.

**Where enforced.** `atlas_buffer` is a live `&mut [u8]` for the entire
call (ordinary Rust borrow-checked exclusivity — no concurrency involved
here at all, single-threaded safe wrapper), so `.as_mut_ptr()`/`.len()`
trivially satisfy the raw core's contract. Comment at lines 174–176
states this correctly.

**Verdict: SOUND.**

---

### 16. `blit_to_quadrant_raw` (unsafe fn, line 201) and its `ptr::copy_nonoverlapping` (line 282)

**What it does.** Row-by-row `memcpy` from a source image's pixel buffer
into one quadrant of the destination atlas buffer.

**Invariant.** Source row `[src_row_start, src_row_start + row_bytes)`
must be within `source.pixels_ptr`'s valid region; destination row
`[dst_row_start, dst_row_start + row_bytes)` must be within
`atlas_ptr`'s valid region; the two regions must not overlap; no other
thread may be concurrently writing the same destination bytes.

**Where enforced, verified per-check rather than assumed:**
- Source-fits-in-quadrant is checked before any write (lines 224–233,
  explicit comment that this "MUST happen before any write occurs").
- Declared-length-matches-declared-dimensions is checked (lines 235–246)
  via `checked_mul` (overflow-safe) before it's used to derive per-row
  bounds.
- Per-row source-read bound (lines 258–261) and destination-write bound
  (lines 268–271) are both re-checked on every iteration — real
  defense-in-depth, not just a comment claiming it: the checks are
  literally inside the `for row in 0..source.height` loop, immediately
  before the `unsafe` block that uses their results.
- Non-overlap between quadrants: follows from `Quadrant::origin`'s
  disjoint (0,0)/(qw,0)/(0,qh)/(qw,qh) placement (lines 82–89) combined
  with the size check above; confirmed by reading `Quadrant::origin`
  directly rather than taking the safety comment's word for it.
- Source vs. destination being different allocations: true whenever
  `atlas_buffer` and `source.pixels_ptr`'s backing storage are genuinely
  distinct, which is the one part of this invariant that is caller
  contract (an `extern "C"` caller could theoretically pass the same
  backing memory as both an input `RawImage` and — indirectly — part of
  what becomes the atlas, though nothing in this crate's own call graph
  does that; not explicitly re-stated in `blit_to_quadrant_raw`'s own
  `# Safety` section, see the "SOUND BUT UNDOCUMENTED" note below).
- Concurrent-write exclusion: this function's own doc comment states it
  as caller contract (lines 198–200); at the one call site inside this
  crate that runs it concurrently (`generate_runtime_atlas_impl`, entry
  #18), it's actually upheld by construction (disjoint quadrants per
  thread) — checked directly at that call site, not just assumed here.

**Verdict: SOUND BUT UNDOCUMENTED (partial)** — everything above the
"Source vs. destination distinct allocations" bullet is fully checked in
code. That one sub-invariant (source and destination don't alias) is
real, is relied upon (`copy_nonoverlapping` is UB on overlapping regions),
and is not explicitly listed in the function's `# Safety` doc comment
(lines 194–200), which only calls out the byte-range-validity and
no-concurrent-write requirements. Suggested comment to add to that
`# Safety` list: *"`atlas_ptr`'s backing allocation and `source.pixels_ptr`'s
backing allocation must be distinct (non-overlapping) buffers."* Not
added in this pass since it's a doc-comment addition to a function
signature block rather than a same-line `// SAFETY:` note on a specific
`unsafe` block, and the task's comment-only allowance was scoped to
per-block findings — flagged here precisely enough for a follow-up.

---

### 17. `unsafe impl Send for SendPtr` / `unsafe impl Sync for SendPtr` (lines 313–314)

**What it does.** Lets a bare `*mut u8` be moved into (and, incidentally,
shared into) the `std::thread::scope` worker closures in
`generate_runtime_atlas_impl`.

**Invariant.** Same partitioned-disjoint-access invariant as entry #14's
write side.

**Where enforced.** Verified directly at the one place `SendPtr` is
actually constructed and used (`generate_runtime_atlas_impl`, lines
474–521): one `SendPtr` per spawned thread, each wrapping the *same*
underlying pointer value but used by each thread only within that
thread's own `Quadrant`-derived, disjoint byte range (enforced inside
`blit_to_quadrant_raw`, entry #16). `Sync` specifically: sharing a
`SendPtr` *value* (not the memory it points at) across threads is
trivially safe since `SendPtr` has no methods that dereference through
`&self` — it's only ever consumed by value into `blit_to_quadrant_raw`'s
raw-pointer parameter. Struct doc comment (lines 305–310) states this
correctly.

**Verdict: SOUND.**

---

### 18. `generate_runtime_atlas_impl`'s four `.as_ref()` calls (lines 451–454)

**What it does.** Converts `head`/`torso`/`legs`/`feet`
(`*const RawImage`) into `Option<&RawImage>`.

**Invariant.** Each pointer must be either null or valid, aligned, and
point at a fully-initialized `RawImage` for the call's duration.

**Where enforced.** `.as_ref()` itself handles the null case safely
(returns `None`); `head_ref`/`torso_ref` are then required non-`None` at
lines 456–457 (the two required sources). The "if non-null, actually
valid" half is external caller contract, correctly stated on the
function's own `# Safety` doc comment (lines 361–364).

**Verdict: SOUND.**

---

### 19. Worker-thread call into `blit_to_quadrant_raw` (line 500)

**What it does.** Each spawned thread calls the raw blit core (entry
#16) with its own `SendPtr`-wrapped atlas pointer and its own source.

**Invariant.** Same as entry #16, specifically the concurrent-write
exclusion clause.

**Where enforced.** Verified directly (not just cited): `jobs` (line 460)
is built with one `(Quadrant, &RawImage)` pair per provided source, each
`scope.spawn` closure captures exactly one job's `quadrant`/`source`
(lines 482–484, by-value capture inside the `.map()` closure — no shared
mutable state between iterations), and `Quadrant::origin` guarantees the
four quadrants are pairwise disjoint rectangles (entry #16). So no two
threads' `blit_to_quadrant_raw` calls ever target overlapping
destination bytes. Confirmed by reading the actual closure capture and
job-construction code, not the surrounding comments alone (which are
also accurate here, including the specific note at lines 487–495 about
why the *whole* `SendPtr` must be captured rather than just its `.0`
field, to avoid RFC 2229 disjoint-capture bypassing `SendPtr`'s manual
`Send` impl — verified this reasoning is correct: capturing only the
inner `*mut u8` field directly would indeed sidestep the wrapper type
entirely).

**Verdict: SOUND.**

---

### 20. `free_atlas_buffer`: `Box::from_raw` + `Vec::from_raw_parts` (lines 564, 572)

**What it does.** Same shape as lib.rs entries #4/#5 and
clothing_deformer.rs entry #10, for `RuntimeAtlasOutput`/`pixels_ptr`.

**Invariant/where enforced.** Traced the write side the same way:
`generate_runtime_atlas_impl` calls `atlas_buffer.shrink_to_fit()` (line
531) immediately before reading `total_bytes`/`.as_mut_ptr()` into the
output struct and leaking via `ManuallyDrop` (lines 532–544), so `len ==
capacity == total_bytes` at the point of leaking. `output`/`buffer`
not-null-not-double-freed is documented external contract (lines
551–553).

**Verdict: SOUND.** This is also the pair the `Cargo.toml`
`panic = "unwind"` reasoning is specifically about (re-verified below).

---

### 21. `catch_unwind`/panic-safety re-verification for `generate_runtime_atlas`/`free_atlas_buffer`

Not a new `unsafe` block, but the task specifically asked to re-derive
(not just cite) the `RESULTS-03.md`/`Cargo.toml` reasoning that these two
functions' `panic::catch_unwind(AssertUnwindSafe(...))` wrapping (lines
377–379, 556) is real protection now that `panic = "unwind"`. Re-checked:
- Both functions genuinely wrap their entire fallible body in
  `catch_unwind`, with the `Err` arm converting the payload to a message
  and returning `null`/no-op rather than re-panicking (lines 381–403,
  583–599) — read directly, not assumed.
- `AssertUnwindSafe` is used because closures over `*const RawImage` and
  friends aren't `UnwindSafe` by default; nothing inside either closure
  relies on a partially-mutated shared state surviving a caught panic in
  a way that `AssertUnwindSafe`'s "I've checked this is actually fine"
  promise would violate — `generate_runtime_atlas_impl` only mutates a
  locally-owned `atlas_buffer: Vec<u8>` that's simply dropped if the
  function returns early/panics before the final `Ok`, and
  `free_atlas_buffer`'s body only touches the one `boxed`/`reclaimed`
  values it locally owns.
- `RESULTS-03.md` (referenced repeatedly by `Cargo.toml` and `lib.rs`'s
  module doc comment) is **not present in this project** as delivered —
  could not be found under any path in the archive. The empirical
  probe-binary verification it's supposed to contain therefore could not
  be independently re-checked in this pass beyond the static reasoning
  above. This is worth flagging on its own: the crate's `panic = "unwind"`
  rationale rests partly on a document that doesn't ship with the
  project.

**Verdict:** the static reasoning holds (**SOUND**, re-derived, not just
cited) but flagging as **NEEDS ATTENTION** that `RESULTS-03.md` is
missing from the delivered project — anyone relying on the `Cargo.toml`
comment's claim of "empirical verification" to trust this can't actually
find that verification here.

---

### 22. Test-only pointer dereferences/slices (lines 670, 689, 777, 834; comment-only additions made in this pass)

**What they do.** Four tests dereference a just-returned
`*mut RuntimeAtlasOutput` and/or read its `pixels_ptr`/`total_bytes` as a
byte slice, immediately after the producing call.

**Invariant/where enforced.** Same shape as lib.rs entry #8 — pointer
just produced by the call directly above, used immediately, not stored
past a further library call.

**Verdict: SOUND BUT UNDOCUMENTED going in — fixed in this pass.** Three
of these four (lines 670, 777, 834) had no `// SAFETY:` comment, unlike
every other `unsafe` block in this file (including the fourth, line 689,
which already had one). Added a `// SAFETY:` comment to each of the
three, stating the same "just returned, not yet freed" justification
already used elsewhere in this file. No logic changed — see the diff in
`texture_atlas.rs`.

---

## `error.rs`

### 23. Test-only `CStr::from_ptr` calls (lines 144, 162, 173; comment-only additions made in this pass)

**What they do.** Three tests wrap the `*const c_char` just returned by
`anthroforge_last_error()` as a `CStr` to check its contents.

**Invariant.** Per `anthroforge_last_error`'s own contract (module doc
comment, lines 32–48, and the function's own `# Safety` section, lines
106–111): the returned pointer, if non-null, is valid until the same
thread makes another call into this library.

**Where enforced.** Each call site reads the pointer immediately after
calling `anthroforge_last_error()` on the same thread, with no
intervening library call — checked directly in each test body (lines
139–147, 157–165, 167–175).

**Verdict: SOUND BUT UNDOCUMENTED going in — fixed in this pass.** None
of these three had a `// SAFETY:` comment, even though this file's
module doc comment (line 56) states *"No unsafe code is required in this
module at all"* for the module's actual `pub`/`pub(crate)` surface — true
for that surface, but slightly misleading in that the test module *does*
use `unsafe` three times against a different (test-only) contract than
the module's own production code. Added a `// SAFETY:` comment to each
of the three call sites explaining why the pointer is still valid at the
point of use. No logic changed — see the diff in `error.rs`.

---

## Summary

| File | Unsafe sites | Sound | Sound but undocumented (fixed here) | Needs attention |
|---|---|---|---|---|
| `lib.rs` | 12 | 12 | 0 | 0 |
| `clothing_deformer.rs` | 13 | 13 | 0 | 0 (see doc-accuracy note under entry #10) |
| `texture_atlas.rs` | 18 | 17 | 3 (now fixed) | 1 sub-invariant (entry #16) |
| `error.rs` | 3 | 3 | 3 (now fixed) | 0 |
| **Total** | **46** | **46** | **6 comments added** | **2 items flagged for follow-up** |

Every `unsafe` block's core memory-safety invariant was verified as
actually sound. No `NEEDS ATTENTION`-grade soundness gap (a case where
the invariant could not be verified, or a call path was found where it
might not hold) was found anywhere in the crate — the two items flagged
above are not soundness gaps in current code, they're a **documentation
staleness** issue (entry #10: the `catch_unwind`/`panic = "abort"`
comment in `clothing_deformer.rs` no longer matches `Cargo.toml`) and a
**missing supporting artifact** (entry #21: `RESULTS-03.md` is referenced
repeatedly but not present in the delivered project). Both are flagged
precisely enough for a targeted follow-up, per the task's instructions,
rather than fixed here.

## Comment-only changes made in this pass

- `rust-core/src/texture_atlas.rs` — added three `// SAFETY:` comments
  (test-only `unsafe` blocks, entry #22).
- `rust-core/src/error.rs` — added three `// SAFETY:` comments (test-only
  `unsafe` blocks, entry #23).

No other file was modified. No logic, test behavior, or `Cargo.toml`
setting was changed. `cargo build --release` and `cargo test --release`
were re-run after these comment additions; the same 62 tests still pass.
