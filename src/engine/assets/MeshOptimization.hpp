#pragma once

// Import-time tuning for GPU-friendly ordering and LOD generation.

#include "Assets.hpp"

#include <cstdint>

// Import-time tuning for GPU-friendly ordering and LOD generation.
struct MeshOptimizationSettings {
    // Each simplified level targets this fraction of the previous level's
    // index count.
    float lodTargetRatio{0.5f};
    // A simplified level must keep at most this fraction of the previous
    // level's indices; generation stops at the first level that does not.
    float maximumLodRetainedRatio{0.8f};
    // Simplification never exceeds this deviation, as a fraction of the
    // primitive's extent.
    float maximumLodRelativeError{0.05f};
    // Primitives with fewer indices keep only their full-detail level.
    uint32_t minimumLodSourceIndices{192};
};

// Optimizes every triangle primitive of a mesh in place:
// 1. Reorders triangles for the post-transform vertex cache, then for overdraw.
// 2. Reorders vertices for fetch locality when every primitive references its
//    own contiguous vertex range (as ModelImporter produces). Every per-vertex
//    stream moves together: Vertex (including joints and weights) and each
//    morph target's deltas. Vertex count and ranges are unchanged; vertices
//    no triangle references keep a slot after the referenced ones.
// 3. Appends up to maximumMeshLods - 1 simplified index ranges per primitive to
//    MeshAsset::indices and records them in MeshPrimitive::simplifiedLods.
//    Simplified ranges reuse the full-detail vertices, keep each primitive's
//    open borders fixed so adjacent primitives stay crack-free, and record
//    their object-space geometric error. Skinned primitives and meshes with
//    morph targets keep only their full-detail level, because the error is
//    measured in bind pose and does not bound the deformed surface.
// Primitives whose index count is not a multiple of three, or whose indices
// reach past the vertex buffer, are left untouched.
void optimizeMesh(MeshAsset& mesh, const MeshOptimizationSettings& settings = {});
