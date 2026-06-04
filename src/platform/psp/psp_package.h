#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../gba/gba_package.h"  // Reuse export structs — they're platform-neutral
#include "../nds/nds_package.h"  // GBARiggedMeshExport lives here

namespace Affinity
{

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
                std::string& errorMsg);

} // namespace Affinity
