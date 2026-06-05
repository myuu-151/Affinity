#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../gba/gba_package.h"  // Reuse export structs — they're platform-neutral
#include "../nds/nds_package.h"  // GBARiggedMeshExport lives here

namespace Affinity
{

// ---- Raw rig data for the PSP runtime (CPU-skinned each frame) -------------
// GBARiggedMeshExport carries DS-specific DSM/DSA blobs that are useless on
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
    std::vector<PSPRigVertex>   verts;
    std::vector<uint32_t>       indices;       // triangle indices into verts
    std::vector<uint8_t>        triMaterial;   // slot per triangle (empty = all 0)
    std::vector<PSPRigMaterial> materials;     // material slots
    std::vector<PSPRigClip>     clips;
};

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
                const std::vector<GBASpriteExport>& sprites,
                const std::vector<GBASpriteAssetExport>& assets,
                const GBACameraExport& camera,
                const std::vector<GBAMeshExport>& meshes,
                float orbitDist,
                const std::vector<GBASoundSampleExport>& soundSamples,
                const std::vector<GBASoundInstanceExport>& soundInstances,
                const std::vector<GBASkyFrameExport>& skyFrames,
                const GBAScriptExport& script,
                const std::vector<GBABlueprintExport>& blueprints,
                const std::vector<GBABlueprintInstanceExport>& bpInstances,
                const std::vector<GBAHudElementExport>& hudElements,
                const std::vector<GBATmSceneExport>& tmScenes,
                int startMode,
                float midiMasterDb,
                const std::vector<GBARiggedMeshExport>& rigs,
                const std::vector<PSPRigExport>& pspRigs,
                int playerRigIdx,             // which rig is the player (-1 = none)
                std::string& errorMsg);

} // namespace Affinity
