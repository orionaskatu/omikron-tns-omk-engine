// SPDX-License-Identifier: GPL-3.0-or-later
// THE ROUNDED BODIES - the remaster's D1 (`todo/remaster.md`). NOT IN THE
// ORIGINAL: its characters are drawn as authored, every facet flat to the eye.
//
// A character's REST geometry subdivided ONCE, at load, by PHONG TESSELLATION
// (Boubekeur and Alexa, 2008): each triangle split into `level` x `level`
// smaller ones, every new point pulled toward the tangent planes of the
// triangle's three corners by `alpha` - a silhouette rounded from the normals
// the model already carries, no new data. Tier 6: no oracle, judged by eye.
//
// What keeps it safe for everything downstream (posing, lights, the GPU's
// affine path, the shadow map - all per corner or per mesh):
//   * every new triangle has ONE mesh (`cornerMesh`), so a pose moves it
//     whole; a triangle whose corners belong to different meshes (skinned to
//     an ancestor, 9405 corners over the shipped characters) is kept as it is;
//   * the FACE is kept as it is - a `.3DM` morph replaces its vertices by
//     `cornerVertex`, which a new corner does not have (-1);
//   * no CRACK: a point on an edge depends on the edge's two ends alone, and
//     an end's normal is one per POSITION within a mesh (its corners'
//     average). A position is PINNED - pulls nothing - where its corners'
//     normals part by more than 45 degrees (a crease the artist made), on a
//     mesh's open boundary (where body segments meet) and on any triangle
//     kept whole, so creases, joints and the face's rim stay where they were.
#pragma once

#include "o3de/geom3do.h"

#include <cstddef>
#include <cstdint>

namespace omk {

struct SmoothStats {
    std::size_t triangles = 0;   // in
    std::size_t split = 0;       // of them subdivided
    std::size_t keptSeam = 0;    // kept: corners of more than one mesh
    std::size_t keptFace = 0;    // kept: the face (`faceFrom..faceTo`)
    std::size_t pinned = 0;      // positions that pull nothing
    std::size_t positions = 0;
    std::size_t unpulled = 0;    // split with the interior unpulled: it folded
    std::size_t folded = 0;      // still folded after the guard (must be 0)
    std::size_t flatEdges = 0;   // edges the guard took to no pull
};

// `g` rebuilt in place; `faceFrom..faceTo` the face's GLOBAL vertex range
// (`FaceMesh::base`, `+ count`; empty when none). `level` 2..4, `alpha` 0..1
// (0.75 Phong's own). Every parallel per-corner vector is filled; `tieClass`
// is cleared (the pose rebuilds it); `revision` is the caller's to bump.
SmoothStats smoothBody(Geometry& g, std::size_t faceFrom, std::size_t faceTo, int level, float alpha);

}  // namespace omk
