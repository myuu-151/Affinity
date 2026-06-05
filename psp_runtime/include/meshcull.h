// Affinity PSP runtime — load-time triangle tessellation for the scene meshes.
// The PSP GE does NOT clip triangles that overflow the guard band — it DROPS
// them (the DS clips, the PSP doesn't). A big floor triangle whose corner is
// both near the camera and far off-axis overflows and the whole triangle
// vanishes, leaving large wedges of missing floor at grazing angles. We fix it
// by subdividing any oversized triangle once at load so the on-screen region is
// always covered by small triangles that stay on-screen. No runtime culling.
#pragma once

// Tessellate oversized triangles in every mesh (call once, after data is ready).
void meshcull_build(void);

// Draw mesh `meshIdx`. The caller has already set the gum MODEL matrix and bound
// the texture. The transform/camera args are vestigial (kept so callers don't
// change) — there is no culling, the whole (tessellated) mesh is always drawn.
void meshcull_draw(int meshIdx,
                   float ix, float iy, float iz,
                   float scale, float rotY, float rotX, float rotZ,
                   float camX, float camY, float camZ,
                   float fwdX, float fwdY, float fwdZ,
                   float rgtX, float rgtY, float rgtZ,
                   float upX,  float upY,  float upZ,
                   float tanH, float tanV, float drawDist);
