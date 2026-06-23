#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../common/afn_export_ir.h" // Reuse export structs — they're platform-neutral

namespace Affinity
{

// AfnRiggedMeshExport now lives in the shared export IR (common/afn_export_ir.h,
// included above), since the PSV header generator's signature references it.

// Package the current map into a .nds ROM.
// runtimeDir: path to nds_runtime/ directory
// outputPath: where to write the final .nds
// Returns true on success. errorMsg receives details on failure.
bool PackageNDS(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<AfnSpriteExport>& sprites,
                const std::vector<AfnSpriteAssetExport>& assets,
                const AfnCameraExport& camera,
                const std::vector<AfnMeshExport>& meshes,
                float orbitDist,
                const std::vector<AfnSoundSampleExport>& soundSamples,
                const std::vector<AfnSoundInstanceExport>& soundInstances,
                const std::vector<AfnSkyFrameExport>& skyFrames,
                bool ndsAntialiasing,
                const AfnScriptExport& script,
                const std::vector<AfnBlueprintExport>& blueprints,
                const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                const std::vector<AfnHudElementExport>& hudElements,
                const std::vector<AfnTmSceneExport>& tmScenes,
                int startMode,
                float midiMasterDb,
                const std::vector<AfnRiggedMeshExport>& rigs,
                std::string& errorMsg);

} // namespace Affinity
