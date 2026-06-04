// Affinity PSP runtime — per-mesh spatial bucketing for frustum culling.
// Each mesh is partitioned once into a 4x4x4 grid of buckets by triangle
// centroid; at draw time only buckets whose world-space sphere is in view are
// submitted to the GE. Lets one giant subdivided level mesh self-partition.
#pragma once

// Build buckets for every mesh in afn_meshes (call once, after data is ready).
void meshcull_build(void);

// Draw mesh `meshIdx` placed by the given instance transform, culling buckets
// against the camera in true VIEW space (handles camera pitch — fwd/right/up are
// the camera basis vectors). The caller must have set the gum MODEL matrix to
// the SAME transform and bound the texture; this only issues the draw calls for
// visible buckets. tanH/tanV are the (padded) half-FOV tangents; drawDist 0 = unlimited.
void meshcull_draw(int meshIdx,
                   float ix, float iy, float iz,
                   float scale, float rotY, float rotX, float rotZ,
                   float camX, float camY, float camZ,
                   float fwdX, float fwdY, float fwdZ,
                   float rgtX, float rgtY, float rgtZ,
                   float upX,  float upY,  float upZ,
                   float tanH, float tanV, float drawDist);
