#pragma once
// DSM (geometry display list) + DSA (animation) emitters for DSMA. C++ port of
// dsma-library/tools/dsma_common.py (DisplayList + emit_triangles_to_dsm +
// save_animation). Output is the exact byte/word layout the DSMA runtime loads.

#include <cstdint>
#include <vector>
#include "../../map/map_types.h"

namespace DsmaEmit {

// Build the DSM display list (u32 words, leading word = size) for a rig's bind
// geometry. texW/texH scale the 0..1 UVs into texel coordinates (DS has no
// float texcoords), matching the converter's --texture argument.
// smooth=true emits per-vertex normals (smooth shading); false emits per-face
// normals (flat, the DSMA default).
// matSlot >= 0 emits only triangles tagged with that material slot (multi-
// material rigs build one DSM per slot); -1 emits all triangles.
std::vector<uint32_t> BuildDSM(const RiggedMeshAsset& rm, int texW, int texH, bool smooth = false, int matSlot = -1);

// Build the DSA animation blob (u32 words: version, frameCount, boneCount, then
// per-frame per-bone pos.xyz + quat.wxyz in 20.12 fixed point) for one clip.
std::vector<uint32_t> BuildDSA(const RiggedMeshAsset& rm, int clipIdx);

} // namespace DsmaEmit
