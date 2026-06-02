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
std::vector<uint32_t> BuildDSM(const RiggedMeshAsset& rm, int texW, int texH);

// Build the DSA animation blob (u32 words: version, frameCount, boneCount, then
// per-frame per-bone pos.xyz + quat.wxyz in 20.12 fixed point) for one clip.
std::vector<uint32_t> BuildDSA(const RiggedMeshAsset& rm, int clipIdx);

} // namespace DsmaEmit
