#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../gba/gba_package.h" // Reuse export structs — they're platform-neutral

namespace Affinity
{

// A rigged (DSMA skinned) mesh, pre-converted to DS display-list / animation
// blobs by the editor (DsmaEmit). The packager only writes these as static
// const u32 arrays into mapdata.h; the runtime hands them to DSMA_DrawModel.
struct GBARiggedMeshExport
{
    std::string name;
    // One geometry group per material slot. Each group is its own DSM (only the
    // triangles tagged with that material) plus its own base-color texture; the
    // DS binds one texture per draw, so multi-material rigs draw group-by-group.
    // All groups share the same bones, so they share the clips' DSA below.
    struct MatGroup {
        std::vector<uint32_t> dsm;    // DSM (geometry display list) for this slot
        bool textured = false;
        int texW = 0, texH = 0;
        std::vector<uint8_t> texPixels;   // one byte per pixel, palette index 0..255
        uint32_t texPalette[256] = {};    // RGBA8 (256-colour, GL_RGB256)
    };
    std::vector<MatGroup> groups;
    struct Clip {
        std::string name;
        int frames = 0;
        bool loop = true;             // false = play once, hold last frame
        std::vector<uint32_t> dsa;    // DSA (animation)
    };
    std::vector<Clip> clips;
    bool cameraLight = false;         // light follows the camera (headlamp)
    float lightX = 0.0f, lightY = 0.0f; // headlamp aim (pitch/yaw degrees)
    int  cullMode = 0;                // 0 = Back, 1 = Front, 2 = None
    bool useAlpha = false;            // palette index 0 = transparent (DS COLOR0_TRANSPARENT)

    // Convenience: a rig is renderable if it has at least one non-empty group.
    bool hasGeometry() const { return !groups.empty() && !groups[0].dsm.empty(); }
};

// Package the current map into a .nds ROM.
// runtimeDir: path to nds_runtime/ directory
// outputPath: where to write the final .nds
// Returns true on success. errorMsg receives details on failure.
bool PackageNDS(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<GBASpriteExport>& sprites,
                const std::vector<GBASpriteAssetExport>& assets,
                const GBACameraExport& camera,
                const std::vector<GBAMeshExport>& meshes,
                float orbitDist,
                const std::vector<GBASoundSampleExport>& soundSamples,
                const std::vector<GBASoundInstanceExport>& soundInstances,
                const std::vector<GBASkyFrameExport>& skyFrames,
                bool ndsAntialiasing,
                const GBAScriptExport& script,
                const std::vector<GBABlueprintExport>& blueprints,
                const std::vector<GBABlueprintInstanceExport>& bpInstances,
                const std::vector<GBAHudElementExport>& hudElements,
                const std::vector<GBATmSceneExport>& tmScenes,
                int startMode,
                float midiMasterDb,
                const std::vector<GBARiggedMeshExport>& rigs,
                std::string& errorMsg);

} // namespace Affinity
