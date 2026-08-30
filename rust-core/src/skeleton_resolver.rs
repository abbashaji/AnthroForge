//! `skeleton_resolver` — the Master Skeleton Alignment Matrix.
//!
//! Every modular part is authored against its own local skin (its
//! `JOINTS_0` accessor indexes into that part's own joint list). If two
//! parts from different source files were combined without correction,
//! "local joint 3" in the head might mean `neck_01` while "local joint 3"
//! in the torso means `spine_02` — same index, different bone, which tears
//! the mesh apart at runtime.
//!
//! This module loads `master_skeleton.json` (a flat bone-name -> global
//! index map) once, then rewrites each part's per-vertex `bone_indices`
//! from part-local joint indices to that single global numbering, using
//! bone *names* as the join key. A part that references a bone name absent
//! from the master skeleton is a data error, not something to silently
//! paper over, so it is rejected with `Err`.

use std::collections::HashMap;
use std::fmt;
use std::path::{Path, PathBuf};

use crate::SkinnedVertex;

/// Name -> global skeleton bone index, as loaded from `master_skeleton.json`.
pub type MasterSkeleton = HashMap<String, u32>;

#[derive(Debug)]
pub enum SkeletonError {
    /// `master_skeleton.json` could not be read from disk.
    Io { path: PathBuf, source: std::io::Error },
    /// `master_skeleton.json` was not valid JSON, or not shaped as a flat
    /// `{ "bone_name": index, ... }` object.
    Parse {
        path: PathBuf,
        source: serde_json::Error,
    },
    /// A part referenced a bone name that does not exist anywhere in the
    /// master skeleton. This is intentionally NOT silently defaulted, per
    /// spec: a missing bone means the part is incompatible with the rig
    /// and must be rejected outright.
    UnknownBone {
        bone_name: String,
        local_joint_index: usize,
    },
    /// A vertex's local `bone_indices` entry pointed past the end of the
    /// part's own joint list — malformed/corrupt part data.
    LocalJointIndexOutOfRange {
        vertex_index: usize,
        local_index: u16,
        local_joint_count: usize,
    },
}

impl fmt::Display for SkeletonError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SkeletonError::Io { path, source } => {
                write!(f, "failed to read '{}': {source}", path.display())
            }
            SkeletonError::Parse { path, source } => {
                write!(f, "failed to parse '{}' as JSON: {source}", path.display())
            }
            SkeletonError::UnknownBone {
                bone_name,
                local_joint_index,
            } => write!(
                f,
                "bone '{bone_name}' (local joint index {local_joint_index}) does not exist in master_skeleton.json; part is incompatible with the master rig"
            ),
            SkeletonError::LocalJointIndexOutOfRange {
                vertex_index,
                local_index,
                local_joint_count,
            } => write!(
                f,
                "vertex[{vertex_index}] references local joint index {local_index}, but the part only defines {local_joint_count} joints"
            ),
        }
    }
}

impl std::error::Error for SkeletonError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            SkeletonError::Io { source, .. } => Some(source),
            SkeletonError::Parse { source, .. } => Some(source),
            _ => None,
        }
    }
}

/// Load `master_skeleton.json` (a flat `{ "bone_name": global_index, ... }`
/// object) from disk. Never panics; I/O and parse failures both become
/// `Err(SkeletonError)`.
pub fn load_master_skeleton(path: &Path) -> Result<MasterSkeleton, SkeletonError> {
    let raw = std::fs::read_to_string(path).map_err(|source| SkeletonError::Io {
        path: path.to_path_buf(),
        source,
    })?;

    let skeleton: MasterSkeleton =
        serde_json::from_str(&raw).map_err(|source| SkeletonError::Parse {
            path: path.to_path_buf(),
            source,
        })?;

    Ok(skeleton)
}

/// Rewrite `vertices[..].bone_indices` in place from part-local joint
/// indices (indices into `local_bone_names`) to global master-skeleton
/// bone indices.
///
/// `local_bone_names[i]` must be the bone name for part-local joint index
/// `i` (this is exactly what [`crate::gltf_loader::LoadedMesh::local_bone_names`]
/// provides). Every local index actually referenced by a vertex is
/// resolved through `master` by name; a name with no entry in `master` is
/// a hard error (`SkeletonError::UnknownBone`) rather than a silent
/// default, since silently defaulting would reintroduce the exact vertex
/// tearing this module exists to prevent.
pub fn resolve_bone_indices(
    vertices: &mut [SkinnedVertex],
    local_bone_names: &[String],
    master: &MasterSkeleton,
) -> Result<(), SkeletonError> {
    // Resolve the full local -> global lookup table once, up front. This
    // both validates every bone the part uses (regardless of whether a
    // given vertex happens to reference it with nonzero weight) and avoids
    // repeated HashMap lookups per-vertex.
    let mut local_to_global: Vec<u32> = Vec::with_capacity(local_bone_names.len());
    for (local_joint_index, bone_name) in local_bone_names.iter().enumerate() {
        let global_index = *master
            .get(bone_name)
            .ok_or_else(|| SkeletonError::UnknownBone {
                bone_name: bone_name.clone(),
                local_joint_index,
            })?;
        local_to_global.push(global_index);
    }

    let local_joint_count = local_bone_names.len();

    for (vertex_index, vertex) in vertices.iter_mut().enumerate() {
        for slot in 0..4 {
            let local_index = vertex.bone_indices[slot];

            // A weight of 0 means this influence slot is unused; some
            // exporters leave the accompanying index as a stale/garbage
            // 0 rather than a valid joint reference. Skip remapping slots
            // that carry no influence so they can't spuriously fail the
            // range check below.
            if vertex.bone_weights[slot] == 0.0 {
                continue;
            }

            if local_index as usize >= local_joint_count {
                return Err(SkeletonError::LocalJointIndexOutOfRange {
                    vertex_index,
                    local_index,
                    local_joint_count,
                });
            }

            let global_index = local_to_global[local_index as usize];
            // SkinnedVertex.bone_indices is u16; master skeleton indices
            // are expected to fit comfortably within a full production
            // rig's bone count. Saturate rather than silently truncate/
            // wrap if a pathological master skeleton ever exceeds u16::MAX
            // bones.
            vertex.bone_indices[slot] = global_index.min(u16::MAX as u32) as u16;
        }
    }

    Ok(())
}
