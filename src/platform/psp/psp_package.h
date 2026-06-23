#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../gba/gba_package.h"  // Reuse export structs — they're platform-neutral
#include "../nds/nds_package.h"  // AfnRiggedMeshExport lives here

namespace Affinity
{

// ---- Raw rig data for the PSP runtime (CPU-skinned each frame) -------------
// AfnRiggedMeshExport carries DS-specific DSM/DSA blobs that are useless on
// PSP. These structs keep the un-baked rig so the PSP can skin on its FPU:
// DSMA is rigid (1 bone/vertex), baseVerts are in their bone's local space,
// and clip frames are ABSOLUTE bone transforms — so the skinned vertex is just
//   skinned = animPose[bone] · baseVert
// (no inverse-bind needed at runtime; that's already folded into baseVert).
struct PSPRigVertex {
    float px, py, pz;   // position in its bone's local space
    float nx, ny, nz;   // normal in its bone's local space (for camera-light shading)
    float u, v;         // texcoord (0..1, V flipped at emit like meshes)
    int   bone;         // bone index
};
struct PSPRigBonePose {
    float px, py, pz;          // translation
    float qw, qx, qy, qz;      // orientation quaternion (absolute)
};
struct PSPRigClip {
    std::string name;
    int  frameCount = 0;
    bool loop = true;
    std::vector<PSPRigBonePose> frames;   // flattened [frame*boneCount + bone]
};
struct PSPRigMaterial {
    bool textured = false;
    int  texW = 0, texH = 0;
    std::vector<uint8_t> pixels;          // indexed 0..255 (texW*texH)
    uint32_t palette[256] = {};           // RGBA8 (rig palettes are RGBA8)
};
struct PSPRigExport {
    std::string name;
    int  boneCount = 0;
    int  cullMode  = 0;       // 0 Back, 1 Front, 2 None
    bool useAlpha  = false;
    bool cameraLight = false; // headlamp follows the camera (per-material toggle)
    float lightX = 50.0f, lightY = 180.0f;   // headlamp aim: pitch/yaw deg off camera
    float yawOffset = 0.0f;   // model forward correction, degrees (added to all yaw)
    int   collisionType = 0;                 // 0 = none, 1 = box (AABB proxy)
    float colCenter[3]  = {0,0,0};           // box center, rig-local (model) space
    float colExtents[3] = {1,1,1};           // box half-extents, rig-local space
    std::vector<PSPRigVertex>   verts;
    std::vector<uint32_t>       indices;       // triangle indices into verts
    std::vector<uint8_t>        triMaterial;   // slot per triangle (empty = all 0)
    std::vector<PSPRigMaterial> materials;     // material slots
    std::vector<PSPRigClip>     clips;
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
                             const std::vector<PSPRigExport>& pspRigs,
                             int playerRigIdx,
                             std::string& errorMsg,
                             bool emitRig = true);   // false: caller emits its own rig header (PSV multi-rig)

// Package the current map into a PSP EBOOT.PBP.
//
// The PSP has a real FPU, 32 MB of RAM, and the sceGu/sceGum hardware T&L
// pipeline, so unlike the GBA/NDS exporters this one emits *float* geometry and
// native RGBA8888 textures — no fixed-point packing or palette quantization.
// The generated header (psp_mapdata.h) is compiled into the runtime, which
// renders the Mode 4 (3D) scene with sceGumDrawArray.
//
// runtimeDir: path to psp_runtime/ directory
// outputPath: where to write the final EBOOT.PBP (or its containing folder)
// Returns true on success. errorMsg receives details on failure.
//
// The signature mirrors PackageNDS so the editor's export path can call either
// interchangeably; arguments not yet consumed by the PSP runtime are accepted
// and ignored (HUD/script/sound are filled in over subsequent passes).
bool PackagePSP(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<AfnSpriteExport>& sprites,
                const std::vector<AfnSpriteAssetExport>& assets,
                const AfnCameraExport& camera,
                const std::vector<AfnMeshExport>& meshes,
                float orbitDist,
                const std::vector<AfnSoundSampleExport>& soundSamples,
                const std::vector<AfnSoundInstanceExport>& soundInstances,
                const std::vector<AfnSkyFrameExport>& skyFrames,
                const AfnScriptExport& script,
                const std::vector<AfnBlueprintExport>& blueprints,
                const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                const std::vector<AfnHudElementExport>& hudElements,
                const std::vector<AfnTmSceneExport>& tmScenes,
                int startMode,
                float midiMasterDb,
                const std::vector<AfnRiggedMeshExport>& rigs,
                const std::vector<PSPRigExport>& pspRigs,
                int playerRigIdx,             // which rig is the player (-1 = none)
                std::string& errorMsg);

} // namespace Affinity
