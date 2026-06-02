#pragma once
// glTF / GLB skinned-mesh loader for the editor. Produces a RiggedMeshAsset
// (rigid single-bone skinning, DSMA-compatible) used by the viewport preview and
// the NDS exporter. Mirrors the math in dsma-library/tools/gltf_to_dsma.py.

#include <string>
#include "../map/map_types.h"

// Loads the first skinned mesh (a node with both a mesh and a skin) plus all
// animation clips. Returns false and fills *err on failure. <=29 bones.
bool LoadRiggedGLTF(const std::string& path, RiggedMeshAsset& out, std::string* err = nullptr);
