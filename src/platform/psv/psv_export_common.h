#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../common/afn_export_ir.h"  // Reuse export structs — they're platform-neutral

namespace Affinity
{

// ---- Raw rig data for the PSP runtime (CPU-skinned each frame) -------------
// AfnRiggedMeshExport carries DS-specific DSM/DSA blobs that are useless on
// PSP. These structs keep the un-baked rig so the PSP can skin on its FPU:
// DSMA is rigid (1 bone/vertex), baseVerts are in their bone's local space,
// and clip frames are ABSOLUTE bone transforms — so the skinned vertex is just
//   skinned = animPose[bone] · baseVert
// (no inverse-bind needed at runtime; that's already folded into baseVert).
struct AfnRigVertex {
    float px, py, pz;   // position in its bone's local space
    float nx, ny, nz;   // normal in its bone's local space (for camera-light shading)
    float u, v;         // texcoord (0..1, V flipped at emit like meshes)
    int   bone;         // bone index
};
struct AfnRigBonePose {
    float px, py, pz;          // translation
    float qw, qx, qy, qz;      // orientation quaternion (absolute)
};
struct AfnRigClip {
    std::string name;
    int  frameCount = 0;
    bool loop = true;
    float speed = 1.0f;                    // playback rate multiplier (0..2), runtime-applied
    std::vector<AfnRigBonePose> frames;   // flattened [frame*boneCount + bone]
};
struct AfnRigMaterial {
    bool textured = false;
    int  texW = 0, texH = 0;
    std::vector<uint8_t> pixels;          // indexed 0..255 (texW*texH)
    uint32_t palette[256] = {};           // RGBA8 (rig palettes are RGBA8)
};
struct AfnRigExport {
    std::string name;
    int  boneCount = 0;
    int  cullMode  = 0;       // 0 Back, 1 Front, 2 None
    bool useAlpha  = false;
    bool cameraLight = false; // headlamp follows the camera (per-material toggle)
    float lightX = 50.0f, lightY = 180.0f;   // headlamp aim: pitch/yaw deg off camera
    float shadowIntensity = 1.0f;            // shading depth: 0 flat, 1 default (amb 8/31), 2 black shadows
    float yawOffset = 0.0f;   // model forward correction, degrees (added to all yaw)
    int   collisionType = 0;                 // 0 = none, 1 = box (AABB proxy)
    float colCenter[3]  = {0,0,0};           // box center, rig-local (model) space
    float colExtents[3] = {1,1,1};           // box half-extents, rig-local space
    std::vector<AfnRigVertex>   verts;
    std::vector<uint32_t>       indices;       // triangle indices into verts
    std::vector<uint8_t>        triMaterial;   // slot per triangle (empty = all 0)
    std::vector<AfnRigMaterial> materials;     // material slots
    std::vector<AfnRigClip>     clips;
};

// Write the shared PSP/PS-Vita runtime headers (mapdata/rig/sky/sprites/sound/
// player). `hdrPrefix` is the filename prefix ("psp_" or "psv_"); `dataInclude`
// the scene-data contract header ("affinity_psp.h" / "affinity_psv.h"). The PSP
// and Vita runtimes share an identical float/RGBA8888 data layout, so both the
// PSP and PSV exporters call this and differ only in the build step that follows.
bool GenerateAffinityHeaders(const std::string& runtimeDir,
                             const char* hdrPrefix, const char* dataInclude,
                             const std::vector<AfnSpriteExport>& sprites,
                             const std::vector<AfnSpriteAssetExport>& assets,
                             const AfnCameraExport& camera,
                             const std::vector<AfnMeshExport>& meshes,
                             float orbitDist,
                             const std::vector<AfnSoundSampleExport>& soundSamples,
                             const std::vector<AfnSoundInstanceExport>& soundInstances,
                             const std::vector<AfnSkyFrameExport>& skyFrames,
                             const std::vector<AfnRigExport>& pspRigs,
                             int playerRigIdx,
                             std::string& errorMsg,
                             bool emitRig = true);   // false: caller emits its own rig header (PSV multi-rig)

} // namespace Affinity
