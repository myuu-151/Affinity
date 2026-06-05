// Affinity PSP runtime — bucketed frustum culling + boundary clipping.
// The PSP GE does NOT clip triangles that overflow the guard band — it DROPS
// them (the DS clips, the PSP doesn't), leaving wedges of missing floor at
// grazing angles. We tile each mesh into a spatial bucket grid: buckets fully
// inside the frustum draw directly, buckets fully outside are skipped, and only
// the few buckets straddling a frustum plane fall to exact per-triangle
// Sutherland-Hodgman clipping. Cheap like culling, wedge-free like clipping,
// and the accurate AABB test never false-culls. See meshcull.c for the rationale.
#pragma once

// Build the per-mesh bucket grid + scratch buffers (call once, after mesh data).
void meshcull_build(void);

// Draw mesh `meshIdx`, bucket-culled + boundary-clipped to the camera described
// by the args. The caller has already set the gum MODEL matrix and bound the
// texture; emitted verts/indices are in the mesh's LOCAL space so it applies.
void meshcull_draw(int meshIdx,
                   float ix, float iy, float iz,
                   float scale, float rotY, float rotX, float rotZ,
                   float camX, float camY, float camZ,
                   float fwdX, float fwdY, float fwdZ,
                   float rgtX, float rgtY, float rgtZ,
                   float upX,  float upY,  float upZ,
                   float tanH, float tanV, float drawDist);
