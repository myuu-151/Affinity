// Affinity PSP runtime — per-mesh spatial bucketing for frustum culling.
// Each mesh is partitioned once into a 4x4x4 grid of buckets by triangle
// centroid; at draw time only buckets whose world-space sphere is in view are
// submitted to the GE. Lets one giant subdivided level mesh self-partition.
#pragma once

// Build buckets for every mesh in afn_meshes (call once, after data is ready).
void meshcull_build(void);

// Draw mesh `meshIdx` placed by the given instance transform, culling buckets
// against the camera. The caller must have already set the gum MODEL matrix to
// the SAME transform (scale -> rotY -> rotX -> rotZ -> translate) and bound the
// texture; this routine only issues the sceGumDrawArray calls for visible
// buckets. camSin/camCos are sin/cos of the camera yaw; drawDist 0 = unlimited.
void meshcull_draw(int meshIdx,
                   float ix, float iy, float iz,
                   float scale, float rotY, float rotX, float rotZ,
                   float camX, float camY, float camZ,
                   float camSin, float camCos, float drawDist);
