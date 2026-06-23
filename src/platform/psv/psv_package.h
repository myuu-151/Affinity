#pragma once

#include <string>
#include <vector>
#include "../psp/psp_package.h"   // Reuses the PSP export structs + GenerateAffinityHeaders

namespace Affinity
{

// Package the current map for the PS Vita runtime (vitaGL / Mode 4 3D).
//
// The Vita shares the PSP's float-geometry + RGBA8888 data layout, so this
// regenerates the psv_runtime headers (psv_mapdata.h / psv_rig.h / psv_sky.h /
// psv_sprites.h / psv_sound.h / psv_player.h) via the shared GenerateAffinityHeaders
// — the ONLY differences from PSP are the "psv_" filename prefix, the
// "affinity_psv.h" data-contract include, and the build step: instead of pspdev
// through WSL, it builds affinity_psv.vpk through the VitaSDK CMake toolchain
// invoked via the devkitPro MSYS2 bash.
//
// The signature mirrors PackagePSP so the editor's export path can call either
// interchangeably.
//
// runtimeDir: path to psv_runtime/ directory
// outputPath: where the final .vpk should end up (build/affinity_psv.vpk)
bool PackagePSV(const std::string& runtimeDir,
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
                int playerRigIdx,
                std::string& errorMsg);

} // namespace Affinity
